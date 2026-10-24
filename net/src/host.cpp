#include "aether/net/host.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>

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
u64 AddressKey(const Address& a) { return (static_cast<u64>(a.ip) << 16) | a.port; }
void PutAddress(std::vector<u8>& b, const Address& a) {
    PutU32(b, a.ip);
    b.push_back(static_cast<u8>(a.port)), b.push_back(static_cast<u8>(a.port >> 8));
}
u64 Ms(f64 now) { return now <= 0 ? 0 : static_cast<u64>(std::llround(now * 1000.0)); }
SipKey RandomKey() {
    std::random_device rd;
    SipKey k{};
    for (usize i = 0; i < k.size(); i += 4) {
        const u32 r = rd();
        for (usize j = 0; j < 4; ++j) k[i + j] = static_cast<u8>(r >> (8 * j));
    }
    return k;
}
// Session key for one direction: SipHash(secret, domain|dir|half|nonces|address), two halves.
SipKey DeriveKey(const SipKey& secret, u8 dir, u64 client_nonce, u64 server_nonce, const Address& client) {
    SipKey k{};
    for (u8 half = 0; half < 2; ++half) {
        std::vector<u8> m = {'A', 'E', 'K', dir, half};
        PutU64(m, client_nonce), PutU64(m, server_nonce), PutAddress(m, client);
        const u64 h = SipHash24(secret, m);
        for (int i = 0; i < 8; ++i) k[half * 8 + i] = static_cast<u8>(h >> (8 * i));
    }
    return k;
}
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
      nonce_seed_(static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count()) ^ (static_cast<u64>(socket.LocalAddress().port) << 48)) {
    if (config_.secret == SipKey{}) config_.secret = RandomKey();
    nonce_seed_ ^= SipHash24(config_.secret, {});
}

NetHost::~NetHost() = default;

void NetHost::SendRaw(const Address& to, Kind kind, std::span<const u8> payload) {
    std::vector<u8> b;
    b.reserve(kHeader + payload.size());
    PutU32(b, config_.protocol_id);
    b.push_back(static_cast<u8>(kind));
    b.insert(b.end(), payload.begin(), payload.end());
    socket_.Send(to, b);
}

void NetHost::SendSigned(const PeerState_& peer, Kind kind, std::span<const u8> payload) {
    std::vector<u8> b;
    b.reserve(kHeader + payload.size() + kTagSize);
    PutU32(b, config_.protocol_id);
    b.push_back(static_cast<u8>(kind));
    b.insert(b.end(), payload.begin(), payload.end());
    PutU64(b, SipHash24(peer.keys.send, b));
    socket_.Send(peer.address, b);
}

bool NetHost::Verify(const PeerState_& peer, std::span<const u8> d) const {
    if (d.size() < kHeader + kTagSize) return false;
    const usize n = d.size() - kTagSize;
    return TagEqual(SipHash24(peer.keys.receive, d.first(n)), GetU64(d.data() + n));
}

u64 NetHost::Cookie(const Address& from, u64 nonce, u64 time_ms) const {
    std::vector<u8> m = {'A', 'E', 'C'};
    PutU32(m, config_.protocol_id), PutAddress(m, from), PutU64(m, nonce), PutU64(m, time_ms);
    return SipHash24(config_.secret, m);
}

bool NetHost::Allow(const Address& from, usize bytes, bool handshake, f64 now) {
    const u64 key = AddressKey(from);
    auto it = buckets_.find(key);
    if (it == buckets_.end()) {
        if (config_.max_tracked_addresses == 0) return true;
        if (buckets_.size() >= config_.max_tracked_addresses) {
            // Bounded: forget the least recently seen address (it starts again with a full budget).
            auto oldest = buckets_.begin();
            for (auto b = buckets_.begin(); b != buckets_.end(); ++b)
                if (b->second.last < oldest->second.last) oldest = b;
            buckets_.erase(oldest);
        }
        it = buckets_.emplace(key, Bucket{config_.handshake_burst, config_.packet_burst, config_.byte_burst, now}).first;
    }
    Bucket& b = it->second;
    const f64 dt = std::max(0.0, now - b.last);
    b.last = now;
    b.handshakes = std::min(config_.handshake_burst, b.handshakes + dt * config_.handshakes_per_second);
    b.packets = std::min(config_.packet_burst, b.packets + dt * config_.packets_per_second);
    b.bytes = std::min(config_.byte_burst, b.bytes + dt * config_.bytes_per_second);
    const bool packets_ok = config_.packets_per_second <= 0 || b.packets >= 1.0;
    const bool bytes_ok = config_.bytes_per_second <= 0 || b.bytes >= static_cast<f64>(bytes);
    const bool hs_ok = !handshake || config_.handshakes_per_second <= 0 || b.handshakes >= 1.0;
    if (!packets_ok || !bytes_ok || !hs_ok) {
        ++security_.rate_limited;
        return false;
    }
    if (config_.packets_per_second > 0) b.packets -= 1.0;
    if (config_.bytes_per_second > 0) b.bytes -= static_cast<f64>(bytes);
    if (handshake && config_.handshakes_per_second > 0) b.handshakes -= 1.0;
    return true;
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
    if ((*it)->state == PeerState::Connected)
        for (int i = 0; i < 3; ++i) SendSigned(**it, Kind::Disconnect);
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
    if (d.size() < kHeader || GetU32(d.data()) != config_.protocol_id) {
        ++security_.malformed;
        return; // not ours
    }
    const Kind kind = static_cast<Kind>(d[4]);
    const bool handshake = kind == Kind::ConnectRequest || kind == Kind::ConnectResponse;
    if (!Allow(from, d.size(), handshake, now)) return;
    const std::span<const u8> body = d.subspan(kHeader);
    PeerState_* peer = ByAddress(from);
    switch (kind) {
    case Kind::ConnectRequest: {
        // Stateless: answer with a cookie, never bigger than the (padded) request.
        if (!listening_) return;
        if (d.size() < kConnectRequestSize) {
            ++security_.malformed;
            return;
        }
        const u64 nonce = GetU64(body.data());
        const u64 t = Ms(now);
        std::vector<u8> challenge;
        PutU64(challenge, nonce), PutU64(challenge, t), PutU64(challenge, Cookie(from, nonce, t));
        SendRaw(from, Kind::Challenge, challenge);
        ++security_.challenges_sent;
        return;
    }
    case Kind::Challenge: {
        if (peer == nullptr || peer->state != PeerState::Connecting || body.size() < 24 || GetU64(body.data()) != peer->nonce) return;
        peer->challenged = true;
        peer->cookie_time = GetU64(body.data() + 8);
        peer->cookie = GetU64(body.data() + 16);
        peer->last_attempt = -1e300; // answer right away
        return;
    }
    case Kind::ConnectResponse: {
        if (!listening_) return;
        if (d.size() < kConnectRequestSize) {
            ++security_.malformed;
            return;
        }
        const u64 nonce = GetU64(body.data()), t = GetU64(body.data() + 8), cookie = GetU64(body.data() + 16);
        const u64 now_ms = Ms(now), life_ms = Ms(config_.cookie_lifetime);
        if (!TagEqual(Cookie(from, nonce, t), cookie) || t > now_ms + 1000 || now_ms - std::min(now_ms, t) > life_ms) {
            ++security_.bad_cookies;
            return;
        }
        if (peer != nullptr && peer->state == PeerState::Connected && peer->nonce == nonce) {
            // Our Accept was lost: send it again (no new state).
        } else {
            for (auto it = used_cookies_.begin(); it != used_cookies_.end();)
                it = now_ms - std::min(now_ms, it->second) > life_ms + 1000 ? used_cookies_.erase(it) : std::next(it);
            if (used_cookies_.count(cookie) != 0) {
                ++security_.replayed_handshakes;
                return;
            }
            if (PeerCount() >= config_.max_peers) {
                std::vector<u8> deny;
                PutU64(deny, nonce);
                SendRaw(from, Kind::Deny, deny);
                return;
            }
            used_cookies_.emplace(cookie, t);
            // The same address with a new, verified handshake: the old client restarted. Start over.
            if (peer != nullptr) Drop(peer->id, DisconnectReason::Remote);
            auto p = std::make_unique<PeerState_>();
            p->id = next_id_++;
            p->address = from;
            p->state = PeerState::Connected;
            p->nonce = nonce;
            p->started = p->last_received = now;
            nonce_seed_ = nonce_seed_ * 6364136223846793005ULL + 1442695040888963407ULL;
            const u64 server_nonce = SipHash24(config_.secret, std::span<const u8>(reinterpret_cast<const u8*>(&nonce_seed_), 8));
            p->keys.receive = DeriveKey(config_.secret, 0, nonce, server_nonce, from); // client -> server
            p->keys.send = DeriveKey(config_.secret, 1, nonce, server_nonce, from);    // server -> client
            p->connection = std::make_unique<Connection>(config_.connection);
            peer = p.get();
            peers_.push_back(std::move(p));
            events_.push_back({NetEventType::Connected, peer->id, DisconnectReason::Local, 0, {}});
        }
        peer->last_received = now;
        // Accept: the nonce, its id and the session keys (client's send, client's receive). 49 bytes < 69.
        std::vector<u8> accept;
        PutU64(accept, peer->nonce);
        PutU32(accept, peer->id);
        accept.insert(accept.end(), peer->keys.receive.begin(), peer->keys.receive.end());
        accept.insert(accept.end(), peer->keys.send.begin(), peer->keys.send.end());
        SendRaw(from, Kind::Accept, accept);
        return;
    }
    case Kind::Accept:
        if (peer == nullptr || body.size() < 44 || GetU64(body.data()) != peer->nonce) return;
        if (peer->state == PeerState::Connecting) {
            peer->last_received = now;
            peer->state = PeerState::Connected;
            peer->remote_id = GetU32(body.data() + 8);
            std::copy_n(body.data() + 12, 16, peer->keys.send.begin());
            std::copy_n(body.data() + 28, 16, peer->keys.receive.begin());
            peer->connection = std::make_unique<Connection>(config_.connection);
            events_.push_back({NetEventType::Connected, peer->id, DisconnectReason::Local, 0, {}});
        }
        return;
    case Kind::Deny:
        if (peer != nullptr && peer->state == PeerState::Connecting && body.size() >= 8 && GetU64(body.data()) == peer->nonce)
            Drop(peer->id, DisconnectReason::Denied);
        return;
    case Kind::Disconnect:
        if (peer == nullptr || peer->state != PeerState::Connected) return;
        if (!Verify(*peer, d)) {
            ++security_.bad_macs;
            return;
        }
        Drop(peer->id, DisconnectReason::Remote);
        return;
    case Kind::Data: {
        if (peer == nullptr || peer->state != PeerState::Connected) return;
        if (!Verify(*peer, d)) {
            ++security_.bad_macs;
            return;
        }
        peer->last_received = now;
        peer->connection->ReceivePacket(body.first(body.size() - kTagSize), now);
        Message m;
        while (peer->connection->Receive(m)) events_.push_back({NetEventType::Message, peer->id, DisconnectReason::Local, m.channel, std::move(m.data)});
        return;
    }
    }
    ++security_.malformed;
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
                if (p->challenged) PutU64(req, p->cookie_time), PutU64(req, p->cookie);
                req.resize(kConnectRequestSize - kHeader, 0); // padding: see kConnectRequestSize
                SendRaw(p->address, p->challenged ? Kind::ConnectResponse : Kind::ConnectRequest, req);
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
            SendSigned(*p, Kind::Data, packet);
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
