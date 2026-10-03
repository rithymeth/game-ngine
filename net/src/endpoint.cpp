#include "aether/net/endpoint.h"

#include "aether/net/bytes.h"

#include <algorithm>

namespace aether::net {

namespace {

constexpr u32 kMagic = 0x48544541; // "AETH"
enum Kind : u8 { kConnect = 1, kAccept = 2, kReject = 3, kData = 4, kDisconnect = 5 };
constexpr u8 kHasAck = 1;
constexpr usize kHeaderBytes = 12;     // kind, flags, seq, ack, ack bits, message count
constexpr usize kMessageOverhead = 5;  // channel, reliable id, length
constexpr usize kMaxPacketsPerFlush = 32;
constexpr u16 kReliableWindow = 512;

bool SeqGreater(u16 a, u16 b) { return (a > b && a - b <= 32768) || (a < b && b - a > 32768); }

} // namespace

NetEndpoint::NetEndpoint(Transport& transport, const NetConfig& config) : transport_(transport), config_(config) {
    const usize room = transport_.MaxDatagramSize() > kHeaderBytes + kMessageOverhead + 8
                           ? transport_.MaxDatagramSize() - kHeaderBytes - kMessageOverhead - 8
                           : 0;
    config_.max_message_size = std::min(config_.max_message_size, room);
}

void NetEndpoint::Listen() { listening_ = true; }

bool NetEndpoint::Connect(NetAddress server) {
    if (server == 0 || peers_.count(server) != 0) return false;
    Peer& p = peers_[server];
    p.address = server;
    p.state = State::Connecting;
    p.started = now_;
    p.last_receive = now_;
    p.last_send = -1.0e9; // sends the first Connect on the next Update
    return true;
}

bool NetEndpoint::Send(NetAddress address, Channel channel, std::span<const u8> data) {
    auto it = peers_.find(address);
    if (it == peers_.end() || it->second.state != State::Connected || data.size() > config_.max_message_size) return false;
    Peer& p = it->second;
    if (channel == Channel::ReliableOrdered) {
        if (p.reliable_out.size() >= config_.max_reliable_pending) return false;
        Reliable r;
        r.id = p.next_reliable_id++;
        r.data.assign(data.begin(), data.end());
        p.reliable_out.push_back(std::move(r));
    } else {
        p.unreliable_out.emplace_back(data.begin(), data.end());
    }
    return true;
}

void NetEndpoint::Broadcast(Channel channel, std::span<const u8> data) {
    for (auto& [address, peer] : peers_) {
        if (peer.state == State::Connected) Send(address, channel, data);
    }
}

void NetEndpoint::Disconnect(NetAddress address) {
    auto it = peers_.find(address);
    if (it == peers_.end()) return;
    if (it->second.state == State::Connected) {
        for (int i = 0; i < 3; ++i) SendControl(address, kDisconnect); // a lost one must not strand the other side
    }
    Drop(address, DisconnectReason::Local);
}

void NetEndpoint::DisconnectAll() {
    std::vector<NetAddress> all;
    for (const auto& [address, peer] : peers_) {
        (void)peer;
        all.push_back(address);
    }
    for (NetAddress a : all) Disconnect(a);
}

void NetEndpoint::Drop(NetAddress address, DisconnectReason reason) {
    if (peers_.erase(address) == 0) return;
    NetEvent e;
    e.type = NetEventType::Disconnected;
    e.peer = address;
    e.reason = reason;
    Emit(std::move(e));
}

bool NetEndpoint::Poll(NetEvent& out) {
    if (events_.empty()) return false;
    out = std::move(events_.front());
    events_.pop_front();
    return true;
}

bool NetEndpoint::IsConnected(NetAddress address) const {
    auto it = peers_.find(address);
    return it != peers_.end() && it->second.state == State::Connected;
}

std::vector<NetAddress> NetEndpoint::ConnectedPeers() const {
    std::vector<NetAddress> out;
    for (const auto& [address, peer] : peers_) {
        if (peer.state == State::Connected) out.push_back(address);
    }
    return out;
}

bool NetEndpoint::Stats(NetAddress address, PeerStats& out) const {
    auto it = peers_.find(address);
    if (it == peers_.end()) return false;
    out = it->second.stats;
    out.reliable_pending = it->second.reliable_out.size(); // rtt and pending are live values; the rest accumulate
    out.rtt = it->second.srtt;
    return true;
}

f64 NetEndpoint::Rto(const Peer& peer) const { return std::clamp(peer.srtt * 1.5 + 0.02, 0.05, 1.0); }

void NetEndpoint::SendControl(NetAddress to, u8 kind, DisconnectReason reason) {
    ByteWriter w;
    w.U8(kind);
    w.U32(kMagic);
    if (kind == kConnect) w.U32(config_.protocol_version);
    if (kind == kReject) w.U8(static_cast<u8>(reason));
    transport_.Send(to, w.Data());
}

void NetEndpoint::Update(f64 now) {
    now_ = now;
    Datagram d;
    while (transport_.Receive(d)) HandleDatagram(d);

    std::vector<std::pair<NetAddress, DisconnectReason>> dead;
    for (auto& [address, p] : peers_) {
        if (p.state == State::Connecting) {
            if (now_ - p.started > config_.connect_timeout) {
                dead.emplace_back(address, DisconnectReason::Timeout);
            } else if (now_ - p.last_send >= config_.connect_retry) {
                SendControl(address, kConnect);
                p.last_send = now_;
            }
            continue;
        }
        if (now_ - p.last_receive > config_.timeout) {
            dead.emplace_back(address, DisconnectReason::Timeout);
            continue;
        }
        Flush(p);
    }
    for (const auto& [address, reason] : dead) Drop(address, reason);
}

void NetEndpoint::HandleDatagram(const Datagram& d) {
    if (d.data.empty()) return;
    const u8 kind = d.data[0];
    auto it = peers_.find(d.from);

    if (kind == kData) {
        if (it == peers_.end()) return; // not a peer: ignore
        Peer& p = it->second;
        if (p.state == State::Connecting) { // the Accept was lost; the server evidently accepted us
            p.state = State::Connected;
            p.last_receive = now_;
            NetEvent e;
            e.type = NetEventType::Connected;
            e.peer = d.from;
            Emit(std::move(e));
        }
        HandleData(p, d.data);
        return;
    }

    ByteReader r({d.data.data() + 1, d.data.size() - 1});
    if (r.U32() != kMagic || !r.Ok()) return;

    switch (kind) {
    case kConnect: {
        const u32 version = r.U32();
        if (!r.Ok() || !listening_) return;
        if (it != peers_.end()) { // a repeat: the Accept was lost
            if (it->second.state == State::Connected) {
                it->second.last_receive = now_;
                SendControl(d.from, kAccept);
            }
            return;
        }
        if (version != config_.protocol_version) {
            SendControl(d.from, kReject, DisconnectReason::VersionMismatch);
            return;
        }
        if (peers_.size() >= config_.max_clients) {
            SendControl(d.from, kReject, DisconnectReason::Full);
            return;
        }
        Peer& p = peers_[d.from];
        p.address = d.from;
        p.state = State::Connected;
        p.started = p.last_receive = now_;
        SendControl(d.from, kAccept);
        NetEvent e;
        e.type = NetEventType::Connected;
        e.peer = d.from;
        Emit(std::move(e));
        break;
    }
    case kAccept:
        if (it != peers_.end() && it->second.state == State::Connecting) {
            it->second.state = State::Connected;
            it->second.last_receive = now_;
            NetEvent e;
            e.type = NetEventType::Connected;
            e.peer = d.from;
            Emit(std::move(e));
        }
        break;
    case kReject:
        if (it != peers_.end() && it->second.state == State::Connecting) {
            const u8 code = r.U8();
            Drop(d.from, r.Ok() && code == static_cast<u8>(DisconnectReason::VersionMismatch) ? DisconnectReason::VersionMismatch
                                                                                                 : DisconnectReason::Full);
        }
        break;
    case kDisconnect:
        if (it != peers_.end()) Drop(d.from, DisconnectReason::Remote);
        break;
    default:
        break;
    }
}

void NetEndpoint::HandleData(Peer& p, const std::vector<u8>& data) {
    ByteReader r({data.data() + 1, data.size() - 1});
    const u8 flags = r.U8();
    const u16 seq = r.U16();
    const u16 ack = r.U16();
    const u32 ack_bits = r.U32();
    const u8 count = r.U8();
    if (!r.Ok()) return;

    // Track what arrived, and drop datagrams already seen (duplicates) or too old to record.
    if (!p.has_remote) {
        p.has_remote = true;
        p.remote_seq = seq;
        p.recv_bits = 0;
    } else if (SeqGreater(seq, p.remote_seq)) {
        const u32 shift = static_cast<u16>(seq - p.remote_seq);
        p.recv_bits = shift >= 32 ? 0 : p.recv_bits << shift;
        if (shift <= 32) p.recv_bits |= 1u << (shift - 1);
        p.remote_seq = seq;
    } else {
        const u32 diff = static_cast<u16>(p.remote_seq - seq);
        if (diff == 0 || diff > 32 || (p.recv_bits & (1u << (diff - 1))) != 0) return;
        p.recv_bits |= 1u << (diff - 1);
    }
    p.last_receive = now_;
    p.ack_pending = true;
    ++p.stats.packets_received;
    p.stats.bytes_received += data.size();
    if (flags & kHasAck) ProcessAck(p, ack, ack_bits);

    for (u8 i = 0; i < count; ++i) {
        const u8 channel = r.U8();
        u16 id = 0;
        if (channel == static_cast<u8>(Channel::ReliableOrdered)) id = r.U16();
        const u16 length = r.U16();
        const std::span<const u8> body = r.Bytes(length);
        if (!r.Ok()) return;

        if (channel == static_cast<u8>(Channel::Unreliable)) {
            NetEvent e;
            e.peer = p.address;
            e.channel = Channel::Unreliable;
            e.data.assign(body.begin(), body.end());
            Emit(std::move(e));
        } else if (channel == static_cast<u8>(Channel::ReliableOrdered)) {
            const u16 ahead = static_cast<u16>(id - p.next_deliver_id);
            if (ahead >= 32768 || ahead >= kReliableWindow) continue; // already delivered, or absurdly far ahead
            if (ahead > 0) {
                p.reliable_in.emplace(id, std::vector<u8>(body.begin(), body.end()));
                continue;
            }
            std::vector<u8> next(body.begin(), body.end());
            for (;;) {
                NetEvent e;
                e.peer = p.address;
                e.channel = Channel::ReliableOrdered;
                e.data = std::move(next);
                Emit(std::move(e));
                ++p.next_deliver_id;
                auto buffered = p.reliable_in.find(p.next_deliver_id);
                if (buffered == p.reliable_in.end()) break;
                next = std::move(buffered->second);
                p.reliable_in.erase(buffered);
            }
        }
    }
}

void NetEndpoint::ProcessAck(Peer& p, u16 ack, u32 bits) {
    AckPacket(p, ack);
    for (u32 i = 0; i < 32; ++i) {
        if (bits & (1u << i)) AckPacket(p, static_cast<u16>(ack - 1 - i));
    }
}

void NetEndpoint::AckPacket(Peer& p, u16 seq) {
    SentPacket& s = p.sent[seq % kSentRing];
    if (!s.valid || s.seq != seq || s.acked) return;
    s.acked = true;
    const f64 sample = now_ - s.time;
    p.srtt = p.has_rtt ? p.srtt * 0.875 + sample * 0.125 : sample;
    p.has_rtt = true;
    for (u16 id : s.reliable_ids) {
        auto it = std::find_if(p.reliable_out.begin(), p.reliable_out.end(), [id](const Reliable& r) { return r.id == id; });
        if (it != p.reliable_out.end()) p.reliable_out.erase(it);
    }
    s.reliable_ids.clear();
}

void NetEndpoint::Flush(Peer& p) {
    const usize max_size = transport_.MaxDatagramSize();
    const f64 rto = Rto(p);

    for (usize packet = 0; packet < kMaxPacketsPerFlush; ++packet) {
        ByteWriter body;
        std::vector<u16> ids;
        u8 count = 0;
        usize size = kHeaderBytes;
        bool more = false;

        for (Reliable& r : p.reliable_out) {
            const bool due = !r.ever_sent || now_ - r.last_sent >= rto;
            if (!due || std::find(ids.begin(), ids.end(), r.id) != ids.end()) continue;
            if (size + kMessageOverhead + r.data.size() > max_size) {
                more = true;
                continue;
            }
            body.U8(static_cast<u8>(Channel::ReliableOrdered));
            body.U16(r.id);
            body.U16(static_cast<u16>(r.data.size()));
            body.Bytes(r.data);
            size += kMessageOverhead + r.data.size();
            ids.push_back(r.id);
            ++count;
        }
        usize taken = 0;
        while (taken < p.unreliable_out.size()) {
            const std::vector<u8>& m = p.unreliable_out[taken];
            if (size + kMessageOverhead + m.size() > max_size) {
                more = true;
                break;
            }
            body.U8(static_cast<u8>(Channel::Unreliable));
            body.U16(static_cast<u16>(m.size()));
            body.Bytes(m);
            size += kMessageOverhead + m.size() - 2; // no reliable id
            ++count;
            ++taken;
        }
        const bool idle_ack = count == 0 && (p.ack_pending || now_ - p.last_send >= config_.keepalive);
        if (count == 0 && !idle_ack) break;

        const u16 seq = p.local_seq++;
        ByteWriter w;
        w.U8(kData);
        w.U8(p.has_remote ? kHasAck : 0);
        w.U16(seq);
        w.U16(p.remote_seq);
        w.U32(p.recv_bits);
        w.U8(count);
        w.Bytes(body.Data());
        if (!transport_.Send(p.address, w.Data())) break;

        SentPacket& slot = p.sent[seq % kSentRing];
        if (slot.valid && !slot.acked) ++p.stats.packets_lost; // slot reused unacknowledged
        slot.seq = seq;
        slot.time = now_;
        slot.valid = true;
        slot.acked = false;
        slot.reliable_ids = ids;
        for (Reliable& r : p.reliable_out) {
            if (std::find(ids.begin(), ids.end(), r.id) == ids.end()) continue;
            if (r.ever_sent) ++p.stats.reliable_resends;
            r.ever_sent = true;
            r.last_sent = now_;
        }
        p.unreliable_out.erase(p.unreliable_out.begin(), p.unreliable_out.begin() + static_cast<std::ptrdiff_t>(taken));
        p.last_send = now_;
        p.ack_pending = false;
        ++p.stats.packets_sent;
        p.stats.bytes_sent += w.Size();
        if (!more) break;
    }
}

} // namespace aether::net
