#pragma once

#include "aether/net/connection.h"
#include "aether/net/socket.h"

#include <memory>
#include <optional>

namespace aether::net {

using PeerId = u32;
constexpr PeerId kNoPeer = 0;

enum class DisconnectReason : u8 {
    Local,         // we disconnected it
    Remote,        // it said goodbye
    Timeout,       // nothing heard for HostConfig::timeout
    Denied,        // the server refused (full)
    ConnectFailed, // no answer within HostConfig::connect_timeout
};
const char* DisconnectReasonName(DisconnectReason reason);

enum class NetEventType : u8 { Connected, Disconnected, Message };

struct NetEvent {
    NetEventType type = NetEventType::Message;
    PeerId peer = kNoPeer;
    DisconnectReason reason = DisconnectReason::Local;
    u8 channel = 0;
    std::vector<u8> data;
};

struct HostConfig {
    u32 protocol_id = 0x41455448; // packets with another id are ignored ("AETH")
    u32 max_peers = 32;
    f64 timeout = 5.0;          // seconds of silence before a peer is dropped
    f64 keepalive = 0.25;       // send at least this often
    f64 connect_retry = 0.25;   // resend a connect request this often
    f64 connect_timeout = 5.0;  // give up connecting after this long
    ConnectionConfig connection;
};

enum class PeerState : u8 { Connecting, Connected };

struct PeerInfo {
    Address address;
    PeerState state = PeerState::Connecting;
    u32 remote_id = 0; // the server's id for us (on a client)
    const Connection* connection = nullptr;
};

// Hosts connections over a datagram socket (Phase 22 step 1,
// docs/design/PHASE_SPECS.md §22.1): a server listens and accepts peers up
// to max_peers (refusing the rest); a client connects (retrying until
// accepted, refused or out of time). Connected peers exchange messages on
// the Connection's channels, keep alive, time out, and say goodbye.
// Everything happens in Update(now); what happened comes back as events.
class NetHost {
public:
    NetHost(DatagramSocket& socket, HostConfig config = {});
    ~NetHost();

    void Listen(bool accept = true) { listening_ = accept; }
    PeerId Connect(const Address& server, f64 now);
    // Says goodbye (a few times, unacknowledged) and drops it; a Disconnected(Local) event follows.
    void Disconnect(PeerId peer, f64 now);
    void DisconnectAll(f64 now);

    bool Send(PeerId peer, u8 channel, std::span<const u8> data);
    // To every connected peer but `except`; how many it was queued for.
    usize Broadcast(u8 channel, std::span<const u8> data, PeerId except = kNoPeer);

    void Update(f64 now);
    std::vector<NetEvent> TakeEvents();

    std::optional<PeerInfo> Peer(PeerId peer) const;
    std::vector<PeerId> Peers() const; // connected ones
    usize PeerCount() const;           // connected ones
    const HostConfig& Config() const { return config_; }

private:
    enum class Kind : u8 { ConnectRequest = 1, Accept = 2, Deny = 3, Data = 4, Disconnect = 5 };
    struct PeerState_ {
        PeerId id = kNoPeer;
        Address address;
        PeerState state = PeerState::Connecting;
        u64 nonce = 0;
        u32 remote_id = 0;
        f64 started = 0.0, last_received = 0.0, last_sent = -1e300, last_attempt = -1e300;
        std::unique_ptr<Connection> connection;
    };
    void SendRaw(const Address& to, Kind kind, std::span<const u8> payload = {});
    PeerState_* ByAddress(const Address& a);
    void Drop(PeerId peer, DisconnectReason reason);
    void Handle(const Address& from, std::span<const u8> datagram, f64 now);

    DatagramSocket& socket_;
    HostConfig config_;
    bool listening_ = false;
    PeerId next_id_ = 1;
    u64 nonce_seed_;
    std::vector<std::unique_ptr<PeerState_>> peers_;
    std::vector<NetEvent> events_;
};

} // namespace aether::net
