#include "aether/net/connection.h"

#include <algorithm>
#include <cstring>

namespace aether::net {

namespace {
constexpr usize kHeader = 2 + 2 + 4;        // seq, ack, ack bits
constexpr usize kMessageHeader = 1 + 1 + 2 + 2; // channel, flags, id, length
constexpr u16 kWindow = 256;                // reliable ids in flight per channel

void PutU16(std::vector<u8>& b, u16 v) { b.push_back(static_cast<u8>(v)), b.push_back(static_cast<u8>(v >> 8)); }
void PutU32(std::vector<u8>& b, u32 v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(v >> (8 * i)));
}
u16 GetU16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
u32 GetU32(const u8* p) { return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24); }
} // namespace

Connection::Connection(ConnectionConfig config) : config_(std::move(config)) {
    for (ChannelType t : config_.channels) {
        Channel c;
        c.type = t;
        channels_.push_back(std::move(c));
    }
}

bool Connection::Send(u8 channel, std::span<const u8> data) {
    if (channel >= channels_.size()) return false;
    Channel& c = channels_[channel];
    if (c.type == ChannelType::ReliableOrdered) {
        if (data.size() > config_.max_message) return false;
        const usize pieces = std::max<usize>(1, (data.size() + kMaxFragment - 1) / kMaxFragment);
        if (c.queue.size() + pieces > config_.max_queued) return false;
        for (usize i = 0; i < pieces; ++i) {
            const usize from = i * kMaxFragment, to = std::min(data.size(), from + kMaxFragment);
            Outgoing m;
            m.id = c.next_id++;
            m.flags = i + 1 < pieces ? 1 : 0;
            m.data.assign(data.begin() + static_cast<std::ptrdiff_t>(from), data.begin() + static_cast<std::ptrdiff_t>(to));
            c.queue.push_back(std::move(m));
        }
        return true;
    }
    if (data.size() > kMaxFragment || c.queue.size() >= config_.max_queued) return false;
    Outgoing m;
    m.id = c.next_id++;
    m.data.assign(data.begin(), data.end());
    c.queue.push_back(std::move(m));
    return true;
}

usize Connection::Unacked() const {
    usize n = 0;
    for (const Channel& c : channels_)
        if (c.type == ChannelType::ReliableOrdered) n += c.queue.size();
    return n;
}

void Connection::Acked(u16 seq, f64 now) {
    SentPacket& p = sent_[seq % sent_.size()];
    if (!p.valid || p.seq != seq || p.acked) return;
    p.acked = true;
    ++stats_.packets_acked;
    const f64 sample = now - p.time;
    stats_.rtt = rtt_known_ ? stats_.rtt + (sample - stats_.rtt) * 0.1 : sample;
    rtt_known_ = true;
    stats_.packet_loss *= 0.99f;
    for (const auto& [ch, id] : p.reliable) {
        Channel& c = channels_[ch];
        for (Outgoing& m : c.queue)
            if (m.id == id) m.acked = true;
    }
    for (Channel& c : channels_)
        if (c.type == ChannelType::ReliableOrdered)
            while (!c.queue.empty() && c.queue.front().acked) c.queue.pop_front();
}

void Connection::AgeLosses(f64 now) {
    // A packet unacked for a good while (well past a round trip) counts as lost.
    const f64 limit = std::max(1.0, stats_.rtt * 4.0);
    for (SentPacket& p : sent_) {
        if (!p.valid || p.acked || now - p.time < limit) continue;
        p.valid = false;
        ++stats_.packets_lost;
        stats_.packet_loss = stats_.packet_loss * 0.99f + 0.01f;
    }
}

void Connection::Deliver(u8 channel, u8 flags, u16 id, std::vector<u8> data) {
    Channel& c = channels_[channel];
    switch (c.type) {
    case ChannelType::Unreliable:
        inbox_.push_back({channel, std::move(data)});
        return;
    case ChannelType::UnreliableSequenced:
        if (c.any_received && !SeqGreater(id, c.last_received)) return; // older than what we have
        c.any_received = true;
        c.last_received = id;
        inbox_.push_back({channel, std::move(data)});
        return;
    case ChannelType::ReliableOrdered:
        // Already delivered, or too far ahead to buffer: drop (it'll come again).
        if (id != c.expected && !SeqGreater(id, c.expected)) return;
        if (static_cast<u16>(id - c.expected) >= kWindow) return;
        c.buffer.emplace(id, std::make_pair(flags, std::move(data)));
        // Deliver what's now in order, joining fragments.
        for (auto it = c.buffer.find(c.expected); it != c.buffer.end(); it = c.buffer.find(c.expected)) {
            auto [f, bytes] = std::move(it->second);
            c.buffer.erase(it);
            ++c.expected;
            c.fragments.insert(c.fragments.end(), bytes.begin(), bytes.end());
            if ((f & 1) == 0) {
                inbox_.push_back({channel, std::move(c.fragments)});
                c.fragments.clear();
            }
        }
        return;
    }
}

void Connection::ReceivePacket(std::span<const u8> packet, f64 now) {
    if (packet.size() < kHeader) return;
    const u8* p = packet.data();
    const u16 seq = GetU16(p), ack = GetU16(p + 2);
    const u32 bits = GetU32(p + 4);
    // Note it (and drop duplicates).
    if (!any_packet_) {
        any_packet_ = true;
        remote_seq_ = seq;
        remote_bits_ = 0;
    } else if (SeqGreater(seq, remote_seq_)) {
        const u16 diff = static_cast<u16>(seq - remote_seq_);
        remote_bits_ = diff >= 32 ? 0 : (remote_bits_ << diff);
        if (diff <= 32) remote_bits_ |= 1u << (diff - 1);
        remote_seq_ = seq;
    } else {
        const u16 diff = static_cast<u16>(remote_seq_ - seq);
        if (diff == 0 || diff > 32 || (remote_bits_ & (1u << (diff - 1))) != 0) return; // a duplicate (or ancient)
        remote_bits_ |= 1u << (diff - 1);
    }
    ack_owed_ = true;
    ++stats_.packets_received;
    stats_.bytes_received += packet.size();
    // Their acks of ours.
    Acked(ack, now);
    for (u16 i = 0; i < 32; ++i)
        if ((bits >> i) & 1u) Acked(static_cast<u16>(ack - 1 - i), now);
    // The messages.
    usize at = kHeader;
    while (at + kMessageHeader <= packet.size()) {
        const u8 channel = p[at], flags = p[at + 1];
        const u16 id = GetU16(p + at + 2), len = GetU16(p + at + 4);
        at += kMessageHeader;
        if (at + len > packet.size() || channel >= channels_.size()) return; // malformed
        Deliver(channel, flags, id, std::vector<u8>(p + at, p + at + len));
        at += len;
    }
}

std::vector<std::vector<u8>> Connection::WritePackets(f64 now, bool keepalive) {
    AgeLosses(now);
    std::vector<std::vector<u8>> out;
    const f64 resend_after = std::max(0.1, stats_.rtt * 1.25 + 0.02);
    while (true) {
        std::vector<u8> packet;
        const u16 seq = next_seq_;
        PutU16(packet, seq);
        PutU16(packet, any_packet_ ? remote_seq_ : static_cast<u16>(0xFFFF));
        PutU32(packet, any_packet_ ? remote_bits_ : 0u);
        SentPacket record;
        record.seq = seq;
        record.time = now;
        record.valid = true;
        bool any = false;
        const auto add = [&](u8 ch, const Outgoing& m) {
            if (packet.size() + kMessageHeader + m.data.size() > kMaxPacket) return false;
            packet.push_back(ch);
            packet.push_back(m.flags);
            PutU16(packet, m.id);
            PutU16(packet, static_cast<u16>(m.data.size()));
            packet.insert(packet.end(), m.data.begin(), m.data.end());
            any = true;
            return true;
        };
        for (u8 ch = 0; ch < channels_.size(); ++ch) {
            Channel& c = channels_[ch];
            if (c.type == ChannelType::ReliableOrdered) {
                // Unacked messages within the window that are new or due again.
                const u16 base = c.queue.empty() ? 0 : c.queue.front().id;
                for (Outgoing& m : c.queue) {
                    if (static_cast<u16>(m.id - base) >= kWindow) break;
                    if (m.acked || now - m.last_sent < resend_after) continue;
                    if (!add(ch, m)) break;
                    if (m.last_sent > -1e299) ++stats_.messages_resent;
                    m.last_sent = now;
                    record.reliable.push_back({ch, m.id});
                }
            } else {
                while (!c.queue.empty() && add(ch, c.queue.front())) c.queue.pop_front();
            }
        }
        if (!any && !ack_owed_ && !(keepalive && out.empty())) break;
        sent_[seq % sent_.size()] = std::move(record);
        ++next_seq_;
        ++stats_.packets_sent;
        stats_.bytes_sent += packet.size();
        ack_owed_ = false;
        out.push_back(std::move(packet));
        if (!any) break; // an ack or keepalive alone
    }
    return out;
}

bool Connection::Receive(Message& out) {
    if (inbox_.empty()) return false;
    out = std::move(inbox_.front());
    inbox_.pop_front();
    return true;
}

} // namespace aether::net
