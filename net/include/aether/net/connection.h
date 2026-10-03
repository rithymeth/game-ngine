#pragma once

#include "aether/core/base.h"

#include <array>
#include <deque>
#include <map>
#include <span>
#include <vector>

namespace aether::net {

// How a channel's messages travel.
enum class ChannelType : u8 {
    ReliableOrdered,     // every message, once, in order (resent until acked; large ones split)
    Unreliable,          // maybe, maybe out of order (at most one packet's worth)
    UnreliableSequenced, // maybe, never older than one already delivered (state updates)
};

struct ConnectionConfig {
    std::vector<ChannelType> channels = {ChannelType::ReliableOrdered, ChannelType::Unreliable, ChannelType::UnreliableSequenced};
    usize max_message = 256 * 1024; // bytes, for a reliable message
    usize max_queued = 4096;        // messages waiting per channel
};

struct Message {
    u8 channel = 0;
    std::vector<u8> data;
};

struct ConnectionStats {
    f64 rtt = 0.0;          // seconds, smoothed
    f32 packet_loss = 0.0f; // 0..1, smoothed
    u64 packets_sent = 0, packets_received = 0, packets_acked = 0, packets_lost = 0;
    u64 bytes_sent = 0, bytes_received = 0;
    u64 messages_resent = 0;
};

// One end of a connection over datagrams (Phase 22 step 1,
// docs/design/PHASE_SPECS.md §22.1): packets carry a sequence number and
// acks for the last 33 packets received; reliable messages ride packets
// until one carrying them is acked (resent after a round trip and a bit),
// and are delivered in order; large ones are split into fragments. Time
// is passed in (seconds), so a run is repeatable.
class Connection {
public:
    static constexpr usize kMaxPacket = 1200;  // bytes on the wire for a data packet's payload
    static constexpr usize kMaxFragment = 1024; // bytes of a message per packet

    explicit Connection(ConnectionConfig config = {});

    // Queues a message; false when the channel doesn't exist, the queue is full, or it's too big
    // (more than kMaxFragment for unreliable channels).
    bool Send(u8 channel, std::span<const u8> data);
    // A packet from the other end (the host's header already taken off).
    void ReceivePacket(std::span<const u8> packet, f64 now);
    // Packets to send now (queued messages, due resends, and acks owed); with `keepalive`,
    // at least one even when there's nothing else.
    std::vector<std::vector<u8>> WritePackets(f64 now, bool keepalive = false);
    // The next delivered message; false when there's none.
    bool Receive(Message& out);

    const ConnectionStats& Stats() const { return stats_; }
    usize ChannelCount() const { return config_.channels.size(); }
    // Reliable messages not yet acked (all channels).
    usize Unacked() const;

private:
    struct Outgoing {
        u16 id = 0;
        u8 flags = 0; // bit 0: more fragments follow
        std::vector<u8> data;
        f64 last_sent = -1e300;
        bool acked = false;
    };
    struct Channel {
        ChannelType type;
        // Sending.
        u16 next_id = 0;
        std::deque<Outgoing> queue; // reliable: until acked (oldest first); unreliable: until sent
        // Receiving.
        u16 expected = 0; // reliable: the next id to deliver
        std::map<u16, std::pair<u8, std::vector<u8>>> buffer; // reliable: arrived early
        std::vector<u8> fragments;  // reliable: a message being reassembled
        bool any_received = false;  // sequenced
        u16 last_received = 0;      // sequenced
    };
    struct SentPacket {
        u16 seq = 0;
        f64 time = 0.0;
        bool valid = false, acked = false;
        std::vector<std::pair<u8, u16>> reliable; // (channel, message id)
    };
    void Acked(u16 seq, f64 now);
    void Deliver(u8 channel, u8 flags, u16 id, std::vector<u8> data);
    void AgeLosses(f64 now);

    ConnectionConfig config_;
    std::vector<Channel> channels_;
    u16 next_seq_ = 0;
    std::array<SentPacket, 1024> sent_{};
    // What we've received: the newest sequence and bits for the 32 before it.
    bool any_packet_ = false;
    u16 remote_seq_ = 0;
    u32 remote_bits_ = 0;
    bool ack_owed_ = false;
    std::deque<Message> inbox_;
    ConnectionStats stats_;
    bool rtt_known_ = false;
};

// Sequence numbers that wrap: whether a is after b.
inline bool SeqGreater(u16 a, u16 b) { return a != b && static_cast<u16>(a - b) < 32768; }

} // namespace aether::net
