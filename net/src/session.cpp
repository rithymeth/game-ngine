#include "aether/net/session.h"

#include "aether/net/bytes.h"

#include <algorithm>

namespace aether::net {

namespace {

enum Kind : u8 { kJoinRequest = 1, kJoinAccept = 2, kJoinReject = 3, kLobby = 4, kSetReady = 5, kStartGame = 6, kKick = 7 };

bool ValidName(const std::string& name) {
    if (name.empty() || name.size() > kMaxPlayerName) return false;
    return std::none_of(name.begin(), name.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; });
}

std::vector<u8> Message(Kind kind) { return {kSessionMessage, kind}; }

void WriteLobby(ByteWriter& w, const LobbyState& lobby) {
    w.U8(kSessionMessage);
    w.U8(kLobby);
    w.U8(static_cast<u8>(lobby.phase));
    w.String(lobby.map);
    w.Varint(lobby.players.size());
    for (const SessionPlayer& p : lobby.players) {
        w.U16(p.id);
        w.String(p.name);
        w.Bool(p.ready);
        w.Bool(p.is_host);
    }
}

bool ReadLobby(ByteReader& r, LobbyState& out) {
    LobbyState lobby;
    const u8 phase = r.U8();
    lobby.map = r.String();
    const u64 count = r.Varint();
    if (!r.Ok() || phase > static_cast<u8>(SessionPhase::InGame) || count > kMaxSessionPlayers || lobby.map.size() > 64) return false;
    lobby.phase = static_cast<SessionPhase>(phase);
    for (u64 i = 0; i < count; ++i) {
        SessionPlayer p;
        p.id = r.U16();
        p.name = r.String();
        p.ready = r.Bool();
        p.is_host = r.Bool();
        if (!r.Ok() || !ValidName(p.name)) return false;
        lobby.players.push_back(std::move(p));
    }
    if (r.Remaining() != 0) return false;
    out = std::move(lobby);
    return true;
}

} // namespace

// --- host -------------------------------------------------------------------

SessionHost::SessionHost(NetEndpoint& endpoint, const SessionConfig& config) : endpoint_(endpoint), config_(config) {
    config_.max_players = std::clamp<usize>(config_.max_players, 1, kMaxSessionPlayers);
    lobby_.map = config_.map;
    endpoint_.Listen();
    if (!config_.host_player_name.empty()) {
        SessionPlayer host;
        host.id = next_id_++;
        host.name = config_.host_player_name.substr(0, kMaxPlayerName);
        host.ready = true;
        host.is_host = true;
        lobby_.players.push_back(std::move(host));
    }
}

NetAddress SessionHost::AddressOf(u16 player_id) const {
    for (const auto& [peer, id] : players_) {
        if (id == player_id) return peer;
    }
    return 0;
}

u16 SessionHost::PlayerOf(NetAddress peer) const {
    auto it = players_.find(peer);
    return it == players_.end() ? 0 : it->second;
}

SessionInfo SessionHost::Advertisement(u16 game_port) const {
    SessionInfo info;
    info.name = config_.name;
    info.map = lobby_.map;
    info.game_version = config_.game_version;
    info.game_port = game_port;
    info.players = static_cast<u8>(std::min<usize>(lobby_.players.size(), 255));
    info.max_players = static_cast<u8>(config_.max_players);
    return info;
}

void SessionHost::Broadcast(std::vector<u8> message) {
    for (const auto& [peer, id] : players_) {
        (void)id;
        endpoint_.Send(peer, Channel::ReliableOrdered, message);
    }
}

void SessionHost::BroadcastLobby() {
    ByteWriter w;
    WriteLobby(w, lobby_);
    Broadcast(w.Take());
}

void SessionHost::Reject(NetAddress peer, JoinResult why) {
    ++rejected_;
    std::vector<u8> m = Message(kJoinReject);
    m.push_back(static_cast<u8>(why));
    endpoint_.Send(peer, Channel::ReliableOrdered, m);
    pending_.erase(peer);
    closing_[peer] = now_ + 1.0; // a client leaves on a rejection; one that doesn't is cut off
}

std::string SessionHost::UniqueName(const std::string& wanted) const {
    auto taken = [&](const std::string& n) {
        return std::any_of(lobby_.players.begin(), lobby_.players.end(), [&](const SessionPlayer& p) { return p.name == n; });
    };
    if (!taken(wanted)) return wanted;
    for (int i = 2;; ++i) {
        const std::string suffix = " (" + std::to_string(i) + ")";
        const std::string candidate = wanted.substr(0, kMaxPlayerName - std::min(kMaxPlayerName, suffix.size())) + suffix;
        if (!taken(candidate)) return candidate;
    }
}

bool SessionHost::HandleEvent(const NetEvent& event, f64 now) {
    now_ = now;
    if (event.type == NetEventType::Connected) {
        pending_[event.peer] = {now};
        return false;
    }
    if (event.type == NetEventType::Disconnected) {
        pending_.erase(event.peer);
        closing_.erase(event.peer);
        auto it = players_.find(event.peer);
        if (it != players_.end()) {
            const u16 id = it->second;
            players_.erase(it);
            lobby_.players.erase(std::remove_if(lobby_.players.begin(), lobby_.players.end(), [&](const SessionPlayer& p) { return p.id == id; }),
                                 lobby_.players.end());
            BroadcastLobby();
        }
        return false;
    }
    if (event.type != NetEventType::Message || event.data.size() < 2 || event.data[0] != kSessionMessage) return false;

    const u8 kind = event.data[1];
    if (kind == kJoinRequest) {
        HandleJoin(event.peer, event.data, now);
        return true;
    }
    auto player = players_.find(event.peer);
    if (player == players_.end()) return true; // not joined: nothing else counts
    if (kind == kSetReady) {
        ByteReader r({event.data.data() + 2, event.data.size() - 2});
        const bool ready = r.Bool();
        if (!r.Ok() || r.Remaining() != 0) {
            ++malformed_;
            return true;
        }
        for (SessionPlayer& p : lobby_.players) {
            if (p.id == player->second && p.ready != ready && lobby_.phase == SessionPhase::Lobby) {
                p.ready = ready;
                BroadcastLobby();
            }
        }
        return true;
    }
    ++malformed_; // a client sending a host-only or unknown message
    return true;
}

void SessionHost::HandleJoin(NetAddress peer, const std::vector<u8>& data, f64 now) {
    (void)now;
    if (players_.count(peer) != 0) return; // a repeat
    if (pending_.count(peer) == 0) return; // not a connection we are waiting on
    ByteReader r({data.data() + 2, data.size() - 2});
    std::string name = r.String();
    const u32 version = r.U32();
    const std::string password = r.String();
    if (!r.Ok() || r.Remaining() != 0) {
        ++malformed_;
        Reject(peer, JoinResult::BadName);
        return;
    }
    if (lobby_.phase == SessionPhase::InGame && !config_.allow_join_in_progress) return Reject(peer, JoinResult::InProgress);
    if (version != config_.game_version) return Reject(peer, JoinResult::VersionMismatch);
    if (!config_.password.empty() && password != config_.password) return Reject(peer, JoinResult::BadPassword);
    if (!ValidName(name)) return Reject(peer, JoinResult::BadName);
    if (lobby_.players.size() >= config_.max_players) return Reject(peer, JoinResult::Full);

    SessionPlayer p;
    p.id = next_id_++;
    p.name = UniqueName(name);
    p.ready = lobby_.phase == SessionPhase::InGame; // joining a running game: nothing to get ready for
    pending_.erase(peer);
    players_[peer] = p.id;
    lobby_.players.push_back(p);

    std::vector<u8> accept = Message(kJoinAccept);
    accept.push_back(static_cast<u8>(p.id & 0xFF));
    accept.push_back(static_cast<u8>(p.id >> 8));
    endpoint_.Send(peer, Channel::ReliableOrdered, accept);
    BroadcastLobby();
    if (lobby_.phase == SessionPhase::InGame) { // a late joiner is told to start right away
        ByteWriter w;
        w.U8(kSessionMessage);
        w.U8(kStartGame);
        w.String(lobby_.map);
        w.U32(seed_);
        endpoint_.Send(peer, Channel::ReliableOrdered, w.Data());
    }
}

void SessionHost::Update(f64 now) {
    now_ = now;
    std::vector<NetAddress> cut;
    for (const auto& [peer, p] : pending_) {
        if (now - p.since > config_.join_timeout) cut.push_back(peer);
    }
    for (const auto& [peer, deadline] : closing_) {
        if (now >= deadline) cut.push_back(peer);
    }
    for (NetAddress peer : cut) {
        pending_.erase(peer);
        closing_.erase(peer);
        endpoint_.Disconnect(peer);
    }
}

bool SessionHost::SetMap(const std::string& map) {
    if (lobby_.phase != SessionPhase::Lobby || map.empty() || map.size() > 64 || map == lobby_.map) return false;
    lobby_.map = map;
    BroadcastLobby();
    return true;
}

bool SessionHost::SetHostReady(bool ready) {
    for (SessionPlayer& p : lobby_.players) {
        if (!p.is_host) continue;
        if (p.ready != ready && lobby_.phase == SessionPhase::Lobby) {
            p.ready = ready;
            BroadcastLobby();
        }
        return true;
    }
    return false;
}

bool SessionHost::StartGame(bool force) {
    if (lobby_.phase != SessionPhase::Lobby || lobby_.players.empty()) return false;
    if (!force && !std::all_of(lobby_.players.begin(), lobby_.players.end(), [](const SessionPlayer& p) { return p.ready; })) return false;
    // A seed the host picks once, so every player starts from the same random state.
    seed_ = static_cast<u32>(lobby_.players.size() * 2654435761u) ^ static_cast<u32>(lobby_.map.size() * 40503u) ^ 0x9E3779B9u;
    for (SessionPlayer& p : lobby_.players) seed_ = seed_ * 1664525u + 1013904223u + p.id;
    lobby_.phase = SessionPhase::InGame;
    for (SessionPlayer& p : lobby_.players) p.ready = true;
    ByteWriter w;
    w.U8(kSessionMessage);
    w.U8(kStartGame);
    w.String(lobby_.map);
    w.U32(seed_);
    BroadcastLobby();
    Broadcast(w.Take());
    return true;
}

bool SessionHost::Kick(u16 player_id) {
    const NetAddress peer = AddressOf(player_id);
    if (peer == 0) return false;
    endpoint_.Send(peer, Channel::ReliableOrdered, Message(kKick)); // the client leaves on it; if not, Update cuts it off
    players_.erase(peer);
    lobby_.players.erase(std::remove_if(lobby_.players.begin(), lobby_.players.end(), [&](const SessionPlayer& p) { return p.id == player_id; }),
                         lobby_.players.end());
    closing_[peer] = now_ + 0.5;
    BroadcastLobby();
    return true;
}

// --- client -----------------------------------------------------------------

bool SessionClient::Join(NetAddress server, const std::string& player_name, u32 game_version, const std::string& password) {
    if (server_ != 0 || !endpoint_.Connect(server)) return false;
    server_ = server;
    name_ = player_name;
    password_ = password;
    version_ = game_version;
    joined_ = false;
    request_sent_ = false;
    lobby_ = {};
    return true;
}

void SessionClient::Leave() {
    if (server_ == 0) return;
    endpoint_.Disconnect(server_);
    server_ = 0;
    joined_ = false;
    lobby_ = {};
}

bool SessionClient::SetReady(bool ready) {
    if (!joined_) return false;
    std::vector<u8> m = Message(kSetReady);
    m.push_back(ready ? 1 : 0);
    return endpoint_.Send(server_, Channel::ReliableOrdered, m);
}

bool SessionClient::Poll(SessionEvent& out) {
    if (events_.empty()) return false;
    out = std::move(events_.front());
    events_.pop_front();
    return true;
}

bool SessionClient::HandleEvent(const NetEvent& event) {
    if (server_ == 0 || event.peer != server_) return false;
    if (event.type == NetEventType::Connected) {
        ByteWriter w;
        w.U8(kSessionMessage);
        w.U8(kJoinRequest);
        w.String(name_);
        w.U32(version_);
        w.String(password_);
        endpoint_.Send(server_, Channel::ReliableOrdered, w.Data());
        request_sent_ = true;
        return false;
    }
    if (event.type == NetEventType::Disconnected) {
        const bool was_in = joined_ || request_sent_;
        server_ = 0;
        joined_ = false;
        if (was_in) {
            SessionEvent e;
            e.type = SessionEvent::Type::Disconnected;
            Push(std::move(e));
        }
        return false;
    }
    if (event.type != NetEventType::Message || event.data.size() < 2 || event.data[0] != kSessionMessage) return false;

    const std::span<const u8> body(event.data.data() + 2, event.data.size() - 2);
    ByteReader r(body);
    switch (event.data[1]) {
    case kJoinAccept: {
        const u16 id = r.U16();
        if (!r.Ok() || r.Remaining() != 0 || id == 0) {
            ++malformed_;
            break;
        }
        player_id_ = id;
        joined_ = true;
        SessionEvent e;
        e.type = SessionEvent::Type::Joined;
        e.player_id = id;
        Push(std::move(e));
        break;
    }
    case kJoinReject: {
        const u8 why = r.U8();
        if (!r.Ok() || r.Remaining() != 0 || why == 0 || why > static_cast<u8>(JoinResult::InProgress)) {
            ++malformed_;
            break;
        }
        SessionEvent e;
        e.type = SessionEvent::Type::Rejected;
        e.reason = static_cast<JoinResult>(why);
        Push(std::move(e));
        endpoint_.Disconnect(server_);
        server_ = 0;
        break;
    }
    case kLobby: {
        LobbyState lobby;
        if (!joined_ || !ReadLobby(r, lobby)) {
            ++malformed_;
            break;
        }
        if (!(lobby == lobby_)) {
            lobby_ = std::move(lobby);
            SessionEvent e;
            e.type = SessionEvent::Type::LobbyChanged;
            Push(std::move(e));
        }
        break;
    }
    case kStartGame: {
        SessionEvent e;
        e.type = SessionEvent::Type::GameStarting;
        e.map = r.String();
        e.seed = r.U32();
        if (!joined_ || !r.Ok() || r.Remaining() != 0 || e.map.size() > 64) {
            ++malformed_;
            break;
        }
        Push(std::move(e));
        break;
    }
    case kKick: {
        SessionEvent e;
        e.type = SessionEvent::Type::Kicked;
        Push(std::move(e));
        endpoint_.Disconnect(server_);
        server_ = 0;
        joined_ = false;
        lobby_ = {};
        break;
    }
    default:
        ++malformed_;
        break;
    }
    return true;
}

} // namespace aether::net
