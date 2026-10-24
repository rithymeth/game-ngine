#include "aether/server/dedicated_server.h"

#include "aether/core/log.h"
#include "aether/ecs/remote_call.h"
#include "aether/reflection/reflect_macros.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/components.h"

#include <algorithm>
#include <cstdio>

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
    return true;
}

void DedicatedServer::Stop() {
    // Say goodbye to everyone (best effort: one flush of the disconnects).
    if (host_) {
        host_->DisconnectAll(last_now_);
        host_->Update(last_now_);
    }
    rpc_.reset();
    replication_.reset();
    session_.reset();
    host_.reset();
    socket_.Close();
    if (world_)
        for (auto& [peer, pawn] : pawns_)
            if (world_->IsAlive(pawn)) world_->DestroyEntity(pawn);
    pawns_.clear();
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
    ++stats_.ticks;
}

ServerStats DedicatedServer::Stats() const {
    ServerStats s = stats_;
    if (!host_) return s;
    s.security = host_->SecurityStats();
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

// --- NetClient ----------------------------------------------------------------------------------

NetClient::~NetClient() {
    rpc_.reset();
    replication_.reset();
    session_.reset();
    host_.reset();
}

bool NetClient::Start(const NetClientConfig& config, f64 now, std::string* error) {
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
    replication_ = std::make_unique<ReplicationClient>(world_, *host_, server);
    replication_->OnSpawn(kPlayerArchetype, [](World& w, Entity e, const NetIdentity&) { w.AddComponent(e, ServerPlayer{}); });
    rpc_ = std::make_unique<RpcRouter>(world_, *host_, *replication_, server);
    start_ = now;
    return true;
}

void NetClient::Tick(f64 now) {
    if (done_ || !host_) return;
    host_->Update(now);
    for (const NetEvent& e : host_->TakeEvents())
        if (!session_->HandleEvent(e) && !replication_->HandleEvent(e)) rpc_->HandleEvent(e);
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
        const NetIdentity* ni = world_.IsAlive(e) ? world_.GetComponent<NetIdentity>(e) : nullptr;
        if (!ni || !ni->locally_owned || std::string(ni->archetype) != kPlayerArchetype) continue;
        ServerPlayer* sp = world_.GetComponent<ServerPlayer>(e);
        if (!sp) continue;
        result_.pawn_seen = true;
        if (config_.send_rpc && result_.rpcs_sent == 0) {
            const reflect::TypeInfo& type = reflect::Reflect<ServerPlayer>();
            std::vector<reflect::Any> args = {i32{42}};
            RemoteCallResult r = RemoteCallResult::Refused;
            CallFunction(world_, e, type, *type.FindFunction("Ping"), sp, args, nullptr, &r);
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
