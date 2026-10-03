#pragma once

#include "aether/net/transport.h"

#include <deque>
#include <map>
#include <span>
#include <vector>

namespace aether::net {

// The connection layer (Phase 22 step 1): a handshake, then per-peer packet
// sequencing with acknowledgements, a reliable ordered channel (resent until
// the packet carrying it is acknowledged, delivered once and in order) and an
// unreliable channel. It runs over any Transport and takes time from the
// caller, so it is deterministic under a LoopbackNetwork.
//
// One NetEndpoint is either a server (Listen: accepts many peers) or a client
// (Connect: one server). A peer is identified by its transport address.

enum class Channel : u8 { Unreliable = 0, ReliableOrdered = 1 };

enum class DisconnectReason : u8 {
    None,
    Local,           // we called Disconnect
    Remote,          // the other side disconnected
    Timeout,         // nothing heard for NetConfig::timeout, or the connect attempt timed out
    Full,            // the server had no room
    VersionMismatch, // the protocol versions differ
};

enum class NetEventType : u8 { Connected, Disconnected, Message };

struct NetEvent {
    NetEventType type = NetEventType::Message;
    NetAddress peer = 0;
    Channel channel = Channel::Unreliable; // Message only
    std::vector<u8> data;                  // Message only
    DisconnectReason reason = DisconnectReason::None; // Disconnected only
};

struct NetConfig {
    u32 protocol_version = 1;
    usize max_clients = 32;
    f64 timeout = 5.0;          // seconds of silence before a connection is dropped
    f64 connect_timeout = 5.0;  // how long a client keeps trying to connect
    f64 connect_retry = 0.25;
    f64 keepalive = 0.25;       // an idle connection still sends an ack this often
    usize max_message_size = 1000;
    usize max_reliable_pending = 256; // unacknowledged reliable messages per peer; Send fails beyond it
};

struct PeerStats {
    f64 rtt = 0.0; // smoothed round trip, seconds
    u64 packets_sent = 0;
    u64 packets_received = 0;
    u64 packets_lost = 0; // sent packets never acknowledged
    u64 bytes_sent = 0;
    u64 bytes_received = 0;
    u64 reliable_resends = 0;
    usize reliable_pending = 0;
};

class NetEndpoint {
public:
    explicit NetEndpoint(Transport& transport, const NetConfig& config = {});

    void Listen();
    // Starts connecting to a server; completes with a Connected event.
    bool Connect(NetAddress server);

    // Queues a message for the next Update. False if the peer isn't connected,
    // the message is too big, or (reliable) too many are still unacknowledged.
    bool Send(NetAddress peer, Channel channel, std::span<const u8> data);
    void Broadcast(Channel channel, std::span<const u8> data);

    void Disconnect(NetAddress peer);
    void DisconnectAll();

    // Receives datagrams, resends, times out connections and sends what is queued.
    void Update(f64 now);
    bool Poll(NetEvent& out);

    bool IsConnected(NetAddress peer) const;
    std::vector<NetAddress> ConnectedPeers() const;
    bool Stats(NetAddress peer, PeerStats& out) const;
    const NetConfig& Config() const { return config_; }

private:
    enum class State : u8 { Connecting, Connected };
    struct Reliable {
        u16 id = 0;
        std::vector<u8> data;
        f64 last_sent = -1.0;
        bool ever_sent = false;
    };
    struct SentPacket {
        u16 seq = 0;
        f64 time = 0.0;
        bool valid = false;
        bool acked = false;
        std::vector<u16> reliable_ids;
    };
    static constexpr usize kSentRing = 256;
    struct Peer {
        NetAddress address = 0;
        State state = State::Connecting;
        f64 started = 0.0;
        f64 last_receive = 0.0;
        f64 last_send = -1.0e9;
        bool ack_pending = false;

        u16 local_seq = 0;
        bool has_remote = false;
        u16 remote_seq = 0;
        u32 recv_bits = 0; // bit i: packet remote_seq - 1 - i arrived
        SentPacket sent[kSentRing];

        std::deque<Reliable> reliable_out;
        u16 next_reliable_id = 0;
        std::vector<std::vector<u8>> unreliable_out;
        u16 next_deliver_id = 0;
        std::map<u16, std::vector<u8>> reliable_in;

        f64 srtt = 0.1;
        bool has_rtt = false;
        PeerStats stats;
    };

    void HandleDatagram(const Datagram& d);
    void HandleData(Peer& peer, const std::vector<u8>& data);
    void ProcessAck(Peer& peer, u16 ack, u32 bits);
    void AckPacket(Peer& peer, u16 seq);
    void Flush(Peer& peer);
    void SendControl(NetAddress to, u8 kind, DisconnectReason reason = DisconnectReason::None);
    void Drop(NetAddress address, DisconnectReason reason);
    void Emit(NetEvent event) { events_.push_back(std::move(event)); }
    f64 Rto(const Peer& peer) const;

    Transport& transport_;
    NetConfig config_;
    bool listening_ = false;
    f64 now_ = 0.0;
    std::map<NetAddress, Peer> peers_;
    std::deque<NetEvent> events_;
};

} // namespace aether::net
