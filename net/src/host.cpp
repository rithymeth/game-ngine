#include "aether/net/host.h"

#include <algorithm>
#include <chrono>

namespace aether::net {

namespace {
void PutU32(std::vector<u8>& b, u32 v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(v >> (8 * i)));
}
void PutU64(std::vector<u8>& b, u64 v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<u8>(v >> (8 * i)));
}
u32 GetU32(const u8* p) { return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24); }
u64 GetU64(const u8* p) { return static_cast<u64>(GetU32(p)) | (static_cast<u64>(GetU32(p + 4)) << 32); }
constexpr usize kHeader = 4 + 1; // protocol id, kind
} // namespace

const char* DisconnectReasonName(DisconnectReason r) {
    switch (r) {
    case DisconnectReason::Local: return "Local";
    case DisconnectReason::Remote: return "Remote";
    case DisconnectReason::Timeout: return "Timeout";
    case DisconnectReason::Denied: return "Denied";
    case DisconnectReason::ConnectFailed: return "ConnectFailed";
    }
    return "";
}

NetHost::NetHost(DatagramSocket& socket, HostConfig config)
    : socket_(socket), config_(std::move(config)),
      nonce_seed_(static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count()) ^ (static_cast<u64>(socket.LocalAddress().port) << 48)) {}

NetHost::~NetHost() = default;

void NetHost::SendRaw(const Address& to, Kind kind, std::span<const u8> payload) {
    std::vector<u8> b;
    b.reserve(kHeader + payload.size());
    PutU32(b, config_.protocol_id);
    b.push_back(static_cast<u8>(kind));
    b.insert(b.end(), payload.begin(), payload.end());
    socket_.Send(to, b);
}

NetHost::PeerState_* NetHost::ByAddress(const Address& a) {
    for (auto& p : peers_)
        if (p->address == a) return p.get();
    return nullptr;
}

void NetHost::Drop(PeerId peer, DisconnectReason reason) {
    const auto it = std::find_if(peers_.begin(), peers_.end(), [&](const auto& p) { return p->id == peer; });
    if (it == peers_.end()) return;
    events_.push_back({NetEventType::Disconnected, peer, reason, 0, {}});
    peers_.erase(it);
}

PeerId NetHost::Connect(const Address& server, f64 now) {
    if (PeerState_* existing = ByAddress(server)) return existing->id;
    auto p = std::make_unique<PeerState_>();
    p->id = next_id_++;
    p->address = server;
    p->state = PeerState::Connecting;
    nonce_seed_ = nonce_seed_ * 6364136223846793005ULL + 1442695040888963407ULL;
    p->nonce = nonce_seed_ ^ p->id;
    p->started = now;
    p->last_received = now;
    peers_.push_back(std::move(p));
    return peers_.back()->id;
}

void NetHost::Disconnect(PeerId peer, f64) {
    const auto it = std::find_if(peers_.begin(), peers_.end(), [&](const auto& p) { return p->id == peer; });
    if (it == peers_.end()) return;
    // Unacknowledged: sent a few times so one probably arrives.
    for (int i = 0; i < 3; ++i) SendRaw((*it)->address, Kind::Disconnect);
    Drop(peer, DisconnectReason::Local);
}

void NetHost::DisconnectAll(f64 now) {
    std::vector<PeerId> ids;
    for (const auto& p : peers_) ids.push_back(p->id);
    for (PeerId id : ids) Disconnect(id, now);
}

bool NetHost::Send(PeerId peer, u8 channel, std::span<const u8> data) {
    for (auto& p : peers_)
        if (p->id == peer) return p->state == PeerState::Connected && p->connection->Send(channel, data);
    return false;
}

usize NetHost::Broadcast(u8 channel, std::span<const u8> data, PeerId except) {
    usize n = 0;
    for (auto& p : peers_)
        if (p->id != except && p->state == PeerState::Connected && p->connection->Send(channel, data)) ++n;
    return n;
}

void NetHost::Handle(const Address& from, std::span<const u8> d, f64 now) {
    if (d.size() < kHeader || GetU32(d.data()) != config_.protocol_id) return; // not ours
    const Kind kind = static_cast<Kind>(d[4]);
    const std::span<const u8> body = d.subspan(kHeader);
    PeerState_* peer = ByAddress(from);
    switch (kind) {
    case Kind::ConnectRequest: {
        if (!listening_ || body.size() < 8) return;
        const u64 nonce = GetU64(body.data());
        if (peer == nullptr) {
            if (PeerCount() >= config_.max_peers) {
                SendRaw(from, Kind::Deny);
                return;
            }
            auto p = std::make_unique<PeerState_>();
            p->id = next_id_++;
            p->address = from;
            p->state = PeerState::Connected;
            p->nonce = nonce;
            p->started = p->last_received = now;
            p->connection = std::make_unique<Connection>(config_.connection);
            peer = p.get();
            peers_.push_back(std::move(p));
            events_.push_back({NetEventType::Connected, peer->id, DisconnectReason::Local, 0, {}});
        } else if (peer->nonce != nonce) {
            // The same address with a new request: the old client restarted. Start over.
            Drop(peer->id, DisconnectReason::Remote);
            Handle(from, d, now);
            return;
        }
        peer->last_received = now;
        // Accept (again, if the first was lost): the nonce and its id.
        std::vector<u8> accept;
        PutU64(accept, peer->nonce);
        PutU32(accept, peer->id);
        SendRaw(from, Kind::Accept, accept);
        return;
    }
    case Kind::Accept:
        if (peer == nullptr || body.size() < 12 || GetU64(body.data()) != peer->nonce) return;
        peer->last_received = now;
        if (peer->state == PeerState::Connecting) {
            peer->state = PeerState::Connected;
            peer->remote_id = GetU32(body.data() + 8);
            peer->connection = std::make_unique<Connection>(config_.connection);
            events_.push_back({NetEventType::Connected, peer->id, DisconnectReason::Local, 0, {}});
        }
        return;
    case Kind::Deny:
        if (peer != nullptr && peer->state == PeerState::Connecting) Drop(peer->id, DisconnectReason::Denied);
        return;
    case Kind::Disconnect:
        if (peer != nullptr) Drop(peer->id, DisconnectReason::Remote);
        return;
    case Kind::Data: {
        if (peer == nullptr || peer->state != PeerState::Connected) return;
        peer->last_received = now;
        peer->connection->ReceivePacket(body, now);
        Message m;
        while (peer->connection->Receive(m)) events_.push_back({NetEventType::Message, peer->id, DisconnectReason::Local, m.channel, std::move(m.data)});
        return;
    }
    }
}

void NetHost::Update(f64 now) {
    Address from;
    std::vector<u8> datagram;
    while (socket_.Receive(from, datagram)) Handle(from, datagram, now);

    std::vector<std::pair<PeerId, DisconnectReason>> drops;
    for (auto& p : peers_) {
        if (p->state == PeerState::Connecting) {
            if (now - p->started > config_.connect_timeout) {
                drops.push_back({p->id, DisconnectReason::ConnectFailed});
            } else if (now - p->last_attempt >= config_.connect_retry) {
                std::vector<u8> req;
                PutU64(req, p->nonce);
                SendRaw(p->address, Kind::ConnectRequest, req);
                p->last_attempt = now;
            }
            continue;
        }
        if (now - p->last_received > config_.timeout) {
            drops.push_back({p->id, DisconnectReason::Timeout});
            continue;
        }
        const bool keepalive = now - p->last_sent >= config_.keepalive;
        for (const std::vector<u8>& packet : p->connection->WritePackets(now, keepalive)) {
            SendRaw(p->address, Kind::Data, packet);
            p->last_sent = now;
        }
    }
    for (const auto& [id, reason] : drops) Drop(id, reason);
}

std::vector<NetEvent> NetHost::TakeEvents() {
    std::vector<NetEvent> out;
    out.swap(events_);
    return out;
}

std::optional<PeerInfo> NetHost::Peer(PeerId peer) const {
    for (const auto& p : peers_)
        if (p->id == peer) return PeerInfo{p->address, p->state, p->remote_id, p->connection.get()};
    return std::nullopt;
}

std::vector<PeerId> NetHost::Peers() const {
    std::vector<PeerId> out;
    for (const auto& p : peers_)
        if (p->state == PeerState::Connected) out.push_back(p->id);
    return out;
}

usize NetHost::PeerCount() const {
    return static_cast<usize>(std::count_if(peers_.begin(), peers_.end(), [](const auto& p) { return p->state == PeerState::Connected; }));
}

} // namespace aether::net
