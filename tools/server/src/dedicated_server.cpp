#include "aether/server/dedicated_server.h"

#include "aether/core/log.h"
#include "aether/ecs/remote_call.h"
#include "aether/reflection/reflect_macros.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/components.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <utility>

AETHER_REFLECT(aether::server::ServerPlayer, 1,
    AETHER_FIELD(pings, Field_Replicated),
    AETHER_FIELD(last_value, Field_Replicated),
    AETHER_METHOD(Ping, Fn_Server, {"value"})
)

namespace aether::server {

using namespace aether::net;

void RegisterServerComponents() {
    (void)GetComponentId<ServerPlayer>();
    (void)GetComponentId<NetIdentity>();
    (void)GetComponentId<Transform>();
}

DedicatedServer::~DedicatedServer() { Stop(); }

bool DedicatedServer::Start(const ServerConfig& config, World& world, std::string* error) {
    Stop();
    RegisterServerComponents();
    config_ = config;
    world_ = &world;
    std::string socket_error;
    if (!socket_.Open(config.port, &socket_error)) {
        if (error) *error = "can't bind UDP port " + std::to_string(config.port) + ": " + socket_error;
        world_ = nullptr;
        return false;
    }
    if (config.replicate_scene) {
        std::vector<Entity> tagged;
        world.ForEachArchetype([&](const Archetype& archetype) {
            for (usize c = 0; c < archetype.ChunkCount(); ++c) {
                const Entity* chunk = archetype.EntityArray(c);
                tagged.insert(tagged.end(), chunk, chunk + archetype.ChunkEntityCount(c));
            }
        });
        for (Entity e : tagged)
            if (world.HasComponent<Transform>(e) && !world.HasComponent<NetIdentity>(e)) {
                NetIdentity ni;
                SetNetArchetype(ni, kSceneArchetype);
                world.AddComponent(e, ni);
            }
    }
    HostConfig host_config;
    // Room for the players plus a few over, so a full server can say "Full" instead of ignoring them.
    host_config.max_peers = config.max_players + 4;
    host_ = std::make_unique<NetHost>(socket_, host_config);
    host_->Listen();
    SessionInfo info;
    info.name = config.name;
    info.map = config.scene;
    info.mode = "dedicated";
    info.port = Port();
    info.build = config.build_id;
    info.max_players = config.max_players;
    session_ = std::make_unique<SessionHost>(*host_, info, config.password);
    if (!config.token_secret.empty()) session_->SetJoinTokens({config.token_secret, config.require_token});
    replication_ = std::make_unique<ReplicationServer>(world, *host_);
    // Only players see the world: a peer that has connected but not yet joined (or was refused - a wrong
    // password, an old build) gets no entities. Its RPCs can't reach anything either: only an entity's owner
    // may call it, and only joined players own pawns.
    SessionHost* session = session_.get();
    replication_->SetRelevancy([session](PeerId peer, Entity) { return session->Player(peer) != nullptr; });
    rpc_ = std::make_unique<RpcRouter>(world, *host_, *replication_);
    stats_ = {};
    profile_ = {};
    profile_snapshot_bytes_ = 0, profile_snapshots_ = 0, profile_last_ = 0;
    profile_peer_bytes_.clear();
    if (config.lan_advertise) {
        std::string lan_error;
        if (lan_socket_.Open(config.lan_port, &lan_error)) {
            beacon_ = std::make_unique<LanBeacon>(lan_socket_);
            beacon_->info = session_->Info();
            beacon_->info.properties = {{"requires_token", !config.token_secret.empty() && config.require_token ? "1" : "0"}};
            AETHER_LOG_INFO("Server", "answering LAN discovery on UDP %u", lan_socket_.LocalAddress().port);
        } else {
            AETHER_LOG_WARN("Server", "not answering LAN discovery: UDP %u: %s (another server on this host?)", config.lan_port,
                            lan_error.c_str());
        }
    }
    return true;
}

void DedicatedServer::Stop() {
    // Say goodbye to everyone (best effort: one flush of the disconnects).
    if (host_) {
        host_->DisconnectAll(last_now_);
        host_->Update(last_now_);
    }
    beacon_.reset();
    lan_socket_.Close();
    rpc_.reset();
    replication_.reset();
    session_.reset();
    host_.reset();
    socket_.Close();
    if (world_)
        for (auto& [peer, pawn] : pawns_)
            if (world_->IsAlive(pawn)) {
                // A stopped session must retain RPC history from still-connected pawns.
                if (const ServerPlayer* sp = world_->GetComponent<ServerPlayer>(pawn))
                    stats_.pings += static_cast<u64>(sp->pings);
                world_->DestroyEntity(pawn);
            }
    pawns_.clear();
    // Live occupancy must reflect shutdown, while lifetime counters remain available.
    stats_.players = 0;
    world_ = nullptr;
}

void DedicatedServer::OnSessionEvent(const SessionEvent& e) {
    if (e.type == SessionEventType::PlayerJoined) {
        ++stats_.joins;
        NetIdentity ni;
        SetNetArchetype(ni, kPlayerArchetype);
        ni.owner = e.peer;
        const f32 x = static_cast<f32>(pawns_.size()) * 2.0f;
        const Entity pawn = world_->CreateEntity(ni, Transform{Vec3{x, 0, 0}, Quaternion{}}, ServerPlayer{});
        pawns_.push_back({e.peer, pawn});
        const std::optional<PeerInfo> peer = host_->Peer(e.peer);
        AETHER_LOG_INFO("Server", "player joined: '%s' from %s (%zu/%u)", e.name.c_str(),
                        peer ? peer->address.ToString().c_str() : "?", pawns_.size(), config_.max_players);
    } else if (e.type == SessionEventType::PlayerLeft) {
        ++stats_.leaves;
        auto it = std::find_if(pawns_.begin(), pawns_.end(), [&](const auto& p) { return p.first == e.peer; });
        if (it != pawns_.end()) {
            if (world_->IsAlive(it->second)) {
                if (const ServerPlayer* sp = world_->GetComponent<ServerPlayer>(it->second)) stats_.pings += static_cast<u64>(sp->pings);
                world_->DestroyEntity(it->second);
            }
            pawns_.erase(it);
        }
        AETHER_LOG_INFO("Server", "player left: '%s' (%zu/%u)", e.name.c_str(), pawns_.size(), config_.max_players);
    } else {
        ++stats_.refused;
        AETHER_LOG_WARN("Server", "join refused: '%s' (%s)", e.name.c_str(), JoinResultName(e.reason));
    }
    stats_.players = static_cast<u32>(pawns_.size());
    stats_.peak_players = std::max(stats_.peak_players, stats_.players);
}

void DedicatedServer::Tick(f64 now) {
    if (!Running()) return;
    last_now_ = now;
    host_->Update(now);
    for (const NetEvent& e : host_->TakeEvents())
        if (!session_->HandleEvent(e, now) && !replication_->HandleEvent(e)) rpc_->HandleEvent(e);
    session_->Update(now);
    for (const SessionEvent& e : session_->TakeEvents()) OnSessionEvent(e);
    rpc_->Tick(now);
    replication_->Update(now);
    if (beacon_) {
        beacon_->info.players = session_->Info().players;
        beacon_->Update();
    }
    ++stats_.ticks;
}

ServerStats DedicatedServer::Stats() const {
    ServerStats s = stats_;
    if (!host_) return s;
    s.security = host_->SecurityStats();
    if (beacon_) s.lan_answered = beacon_->Answered(), s.lan_rate_limited = beacon_->RateLimited(), s.lan_oversized = beacon_->Oversized();
    for (PeerId p : host_->Peers())
        if (const std::optional<PeerInfo> info = host_->Peer(p); info && info->connection) {
            s.bytes_sent += info->connection->Stats().bytes_sent;
            s.bytes_received += info->connection->Stats().bytes_received;
        }
    for (const auto& [peer, pawn] : pawns_)
        if (const ServerPlayer* sp = world_->GetComponent<ServerPlayer>(pawn)) s.pings += static_cast<u64>(sp->pings);
    s.replicated_entities = 0;
    world_->ForEachArchetype([&](const Archetype& archetype) {
        if (!archetype.Has(GetComponentId<NetIdentity>())) return;
        for (usize c = 0; c < archetype.ChunkCount(); ++c) s.replicated_entities += archetype.ChunkEntityCount(c);
    });
    return s;
}

std::string DedicatedServer::ProfileRecord(f64 now) {
    reflect::Json j;
    j["t"] = now;
    if (!host_) return j.dump();
    const ServerStats st = Stats();
    j["players"] = st.players;
    reflect::Json peers = reflect::Json::array();
    u64 sent_delta = 0, received_delta = 0;
    std::map<PeerId, std::pair<u64, u64>> seen;
    for (PeerId p : host_->Peers()) {
        const std::optional<PeerInfo> info = host_->Peer(p);
        if (!info || !info->connection) continue;
        const ConnectionStats& cs = info->connection->Stats();
        reflect::Json peer;
        peer["peer"] = p;
        peer["address"] = info->address.ToString();
        const SessionPlayer* player = session_->Player(p);
        peer["player"] = player ? player->name : std::string();
        peer["rtt_ms"] = cs.rtt * 1000.0;
        peer["loss"] = cs.packet_loss;
        peer["bytes_sent"] = cs.bytes_sent;
        peer["bytes_received"] = cs.bytes_received;
        peer["packets_sent"] = cs.packets_sent;
        peer["packets_received"] = cs.packets_received;
        peer["packets_lost"] = cs.packets_lost;
        peer["messages_resent"] = cs.messages_resent;
        reflect::Json channels = reflect::Json::array();
        for (usize c = 0; c < std::min(info->connection->ChannelCount(), ConnectionStats::kStatChannels); ++c)
            channels.push_back({{"channel", c}, {"bytes_sent", cs.channel_bytes_sent[c]}, {"bytes_received", cs.channel_bytes_received[c]}});
        peer["channels"] = std::move(channels);
        peers.push_back(std::move(peer));
        const auto last = profile_peer_bytes_.find(p);
        const std::pair<u64, u64> before = last != profile_peer_bytes_.end() ? last->second : std::pair<u64, u64>{0, 0};
        sent_delta += cs.bytes_sent - std::min(cs.bytes_sent, before.first);
        received_delta += cs.bytes_received - std::min(cs.bytes_received, before.second);
        seen[p] = {cs.bytes_sent, cs.bytes_received};
        ++profile_.peer_samples;
        profile_.rtt_sum += cs.rtt * 1000.0;
        profile_.rtt_max = std::max(profile_.rtt_max, cs.rtt * 1000.0);
        profile_.loss_max = std::max(profile_.loss_max, static_cast<f64>(cs.packet_loss));
    }
    profile_peer_bytes_ = std::move(seen);
    profile_.peak_peers = std::max(profile_.peak_peers, static_cast<u32>(peers.size()));
    j["peers"] = std::move(peers);
    const NetProfile& np = replication_->Profile();
    const u64 snap_bytes = np.snapshot_bytes - std::min(np.snapshot_bytes, profile_snapshot_bytes_);
    const u32 snaps = np.snapshots - std::min(np.snapshots, profile_snapshots_);
    profile_snapshot_bytes_ = np.snapshot_bytes, profile_snapshots_ = np.snapshots;
    const f64 avg = snaps ? static_cast<f64>(snap_bytes) / snaps : 0.0;
    reflect::Json interval;
    interval["seconds"] = now - profile_last_;
    interval["snapshots"] = snaps;
    interval["snapshot_bytes"] = snap_bytes;
    interval["avg_snapshot_bytes"] = avg;
    interval["bytes_sent"] = sent_delta;
    interval["bytes_received"] = received_delta;
    j["interval"] = std::move(interval);
    profile_last_ = now;
    j["security"] = {{"rate_limited", st.security.rate_limited}, {"challenges_sent", st.security.challenges_sent},
                     {"bad_cookies", st.security.bad_cookies}, {"replayed_handshakes", st.security.replayed_handshakes},
                     {"bad_macs", st.security.bad_macs}, {"malformed", st.security.malformed}};
    j["lan"] = {{"answered", st.lan_answered}, {"rate_limited", st.lan_rate_limited}, {"oversized", st.lan_oversized}};
    ++profile_.records;
    profile_.snapshots += snaps, profile_.snapshot_bytes += snap_bytes;
    profile_.bytes_sent += sent_delta, profile_.bytes_received += received_delta;
    profile_.max_snapshot_avg = std::max(profile_.max_snapshot_avg, avg);
    return j.dump();
}

std::string DedicatedServer::ProfileSummaryJson() const {
    reflect::Json j;
    j["records"] = profile_.records;
    j["peak_peers"] = profile_.peak_peers;
    j["rtt_ms_mean"] = profile_.peer_samples ? profile_.rtt_sum / static_cast<f64>(profile_.peer_samples) : 0.0;
    j["rtt_ms_max"] = profile_.rtt_max;
    j["loss_max"] = profile_.loss_max;
    j["snapshots"] = profile_.snapshots;
    j["snapshot_bytes"] = profile_.snapshot_bytes;
    j["avg_snapshot_bytes"] = profile_.snapshots ? static_cast<f64>(profile_.snapshot_bytes) / static_cast<f64>(profile_.snapshots) : 0.0;
    j["max_interval_avg_snapshot_bytes"] = profile_.max_snapshot_avg;
    j["bytes_sent"] = profile_.bytes_sent;
    j["bytes_received"] = profile_.bytes_received;
    const ServerStats st = Stats();
    j["rate_limited"] = st.security.rate_limited;
    j["bad_macs"] = st.security.bad_macs;
    j["bad_cookies"] = st.security.bad_cookies;
    j["lan_answered"] = st.lan_answered;
    j["lan_rate_limited"] = st.lan_rate_limited;
    return j.dump();
}

// --- LAN discovery ------------------------------------------------------------------------------

std::vector<LanSession> DiscoverServers(const DiscoverConfig& config, std::string* error, const std::function<void()>& pump) {
    UdpSocket socket;
    std::string socket_error;
    if (!socket.Open(0, &socket_error)) {
        if (error) *error = "can't open a UDP socket: " + socket_error;
        return {};
    }
    LanBrowser browser(socket, config.build_id, config.lan_port);
    browser.also_query = config.also_query;
    browser.auto_search = 0.25;
    browser.expiry = config.timeout + 1.0;
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    for (;;) {
        const f64 t = std::chrono::duration<f64>(Clock::now() - start).count();
        browser.Update(t);
        if (t >= config.timeout) break;
        if (pump) pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return browser.Results();
}

std::string DiscoveryJson(const std::vector<LanSession>& servers) {
    reflect::Json list = reflect::Json::array();
    for (const LanSession& s : servers) {
        const std::string* token = s.info.Property("requires_token");
        list.push_back({{"address", s.address.ToString()}, {"name", s.info.name}, {"mode", s.info.mode}, {"map", s.info.map},
                        {"players", s.info.players}, {"max_players", s.info.max_players}, {"build", s.info.build},
                        {"port", s.info.port}, {"compatible", s.compatible}, {"password", s.info.password},
                        {"requires_token", token && *token == "1"}, {"ping_ms", s.ping * 1000.0}});
    }
    reflect::Json j;
    j["count"] = servers.size();
    j["servers"] = std::move(list);
    return j.dump();
}

// --- NetClient ----------------------------------------------------------------------------------

bool NetClient::UseWorld(World& world) {
    if (host_) return false;
    world_ref_ = &world;
    return true;
}

void NetClient::OnSnapshotApplied(net::ReplicationClient::SnapshotAppliedFn fn) {
    snapshot_applied_ = std::move(fn);
    if (replication_) replication_->OnSnapshotApplied(snapshot_applied_);
}

NetClient::~NetClient() {
    rpc_.reset();
    replication_.reset();
    session_.reset();
    host_.reset();
}

bool NetClient::Start(const NetClientConfig& config, f64 now, std::string* error) {
    // Restarting this object would retain its old replication stream, socket,
    // and World entities. A new session currently requires a fresh NetClient.
    if (host_) {
        if (error) *error = "NetClient already started; use a fresh client for a new session";
        return false;
    }
    RegisterServerComponents();
    config_ = config;
    std::string socket_error;
    if (!socket_.Open(0, &socket_error)) {
        if (error) *error = "can't open a UDP socket: " + socket_error;
        return false;
    }
    host_ = std::make_unique<NetHost>(socket_);
    session_ = std::make_unique<SessionClient>(*host_);
    session_->Join(config.server, config.name, config.password, config.build_id, now, config.token);
    const PeerId server = session_->Server();
    replication_ = std::make_unique<ReplicationClient>(*world_ref_, *host_, server);
    replication_->OnSnapshotApplied(snapshot_applied_);
    replication_->OnSpawn(kPlayerArchetype, [](World& w, Entity e, const NetIdentity&) { w.AddComponent(e, ServerPlayer{}); });
    rpc_ = std::make_unique<RpcRouter>(*world_ref_, *host_, *replication_, server);
    start_ = now;
    return true;
}

void NetClient::Tick(f64 now) {
    if (done_ || !host_) return;
    host_->Update(now);
    for (const NetEvent& e : host_->TakeEvents()) {
        if (session_->HandleEvent(e)) continue;
        // Never apply replicated state (or invoke host snapshot callbacks)
        // until the session has successfully joined. A refused client may
        // receive an early empty snapshot while the handshake is in flight.
        if (session_->GetState() != SessionClient::State::Joined) continue;
        if (!replication_->HandleEvent(e)) rpc_->HandleEvent(e);
    }
    const SessionClient::State state = session_->GetState();
    if (state == SessionClient::State::Failed) {
        result_.join_result = JoinResultName(session_->Result());
        done_ = true;
        return;
    }
    if (state != SessionClient::State::Joined) {
        if (now - start_ > config_.join_timeout) result_.join_result = "Timeout", done_ = true;
        return;
    }
    if (joined_at_ < 0) {
        joined_at_ = now;
        result_.joined = true;
        result_.join_seconds = now - start_;
        result_.player_name = session_->PlayerName();
    }
    result_.entities_seen = std::max(result_.entities_seen, replication_->EntityCount());
    // Our pawn: Ping it once, then wait for the replicated counter to say the server ran it.
    for (const auto& [net_id, e] : replication_->Entities()) {
        const NetIdentity* ni = world_ref_->IsAlive(e) ? world_ref_->GetComponent<NetIdentity>(e) : nullptr;
        if (!ni || !ni->locally_owned || std::string(ni->archetype) != kPlayerArchetype) continue;
        ServerPlayer* sp = world_ref_->GetComponent<ServerPlayer>(e);
        if (!sp) continue;
        result_.pawn_seen = true;
        if (config_.send_rpc && result_.rpcs_sent == 0) {
            const reflect::TypeInfo& type = reflect::Reflect<ServerPlayer>();
            std::vector<reflect::Any> args = {i32{42}};
            RemoteCallResult r = RemoteCallResult::Refused;
            CallFunction(*world_ref_, e, type, *type.FindFunction("Ping"), sp, args, nullptr, &r);
            if (r == RemoteCallResult::Sent) ++result_.rpcs_sent, rpc_at_ = now;
        }
        if (result_.rpcs_sent > 0 && !result_.rpc_ack && sp->pings >= 1 && sp->last_value == 42) {
            result_.rpc_ack = true;
            result_.rpc_ack_seconds = now - rpc_at_;
        }
    }
    if (now - joined_at_ >= config_.duration) {
        Result(); // the connection's byte counts, before it goes
        left_ = true;
        session_->Leave(now);
        host_->Update(now); // flush the goodbye
        done_ = true;
    }
}

const NetClientResult& NetClient::Result() {
    if (host_ && !left_) {
        result_.snapshots = replication_->GetStats().snapshots;
        if (const std::optional<PeerInfo> info = host_->Peer(session_->Server()); info && info->connection) {
            result_.bytes_sent = info->connection->Stats().bytes_sent;
            result_.bytes_received = info->connection->Stats().bytes_received;
        }
    }
    return result_;
}

std::string NetClientResult::ToJson() const {
    reflect::Json j;
    j["joined"] = joined;
    j["join_result"] = join_result;
    j["player"] = player_name;
    j["join_seconds"] = join_seconds;
    j["entities_seen"] = entities_seen;
    j["snapshots"] = snapshots;
    j["pawn_seen"] = pawn_seen;
    j["rpcs_sent"] = rpcs_sent;
    j["rpc_ack"] = rpc_ack;
    j["rpc_ack_seconds"] = rpc_ack_seconds;
    j["bytes_sent"] = bytes_sent;
    j["bytes_received"] = bytes_received;
    return j.dump();
}

} // namespace aether::server
