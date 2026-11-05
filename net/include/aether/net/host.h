#pragma once

#include "aether/net/connection.h"
#include "aether/net/siphash.h"
#include "aether/net/socket.h"

#include <memory>
#include <optional>
#include <unordered_map>

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

    // Security (M8.9, docs/manual/15-dedicated-server.md "Security").
    // The key for handshake cookies and session keys; all zero: random per host (the default).
    SipKey secret{};
    f64 cookie_lifetime = 10.0; // seconds a challenge cookie stays valid
    // Per source address token buckets (0 rate: unlimited). Excess datagrams are dropped and counted.
    f64 handshakes_per_second = 4.0, handshake_burst = 8.0;    // ConnectRequest + ConnectResponse
    f64 packets_per_second = 1000.0, packet_burst = 2000.0;    // every datagram
    f64 bytes_per_second = 512.0 * 1024, byte_burst = 1024.0 * 1024;
    usize max_tracked_addresses = 4096; // the rate table's bound (least recently seen evicted)
};

// What the host dropped and why (all counters, since it was made).
struct HostSecurityStats {
    u64 rate_limited = 0;       // over a per-address budget
    u64 challenges_sent = 0;    // stateless cookies handed out
    u64 bad_cookies = 0;        // a ConnectResponse whose cookie is forged, for another address, or expired
    u64 replayed_handshakes = 0; // a ConnectResponse whose cookie was already used
    u64 bad_macs = 0;           // Data/Disconnect that failed the session MAC (forged, spoofed or corrupted)
    u64 malformed = 0;          // not ours, truncated or an unknown kind
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
//
// M8.9 hardening: connecting is a stateless challenge-response (the server
// answers a padded ConnectRequest with a cookie MACed over the source
// address, the client's nonce and the time, and keeps no state until the
// client echoes a valid, unused cookie; no reply is larger than the request
// it answers). The Accept carries per-direction session keys and every Data
// and Disconnect datagram then carries a SipHash-2-4 tag, so an off-path
// attacker spoofing a peer's address can neither join as it nor inject into
// or tear down its connection. Not encrypted: an on-path observer can read
// traffic and the keys in the Accept (see the manual).
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
    const HostSecurityStats& SecurityStats() const { return security_; }
    usize TrackedAddresses() const { return buckets_.size(); } // rate-limit table entries

    static constexpr usize kConnectRequestSize = 5 + 64; // padded, so a server reply never amplifies
    static constexpr usize kTagSize = 8;

private:
    enum class Kind : u8 { ConnectRequest = 1, Accept = 2, Deny = 3, Data = 4, Disconnect = 5, Challenge = 6, ConnectResponse = 7 };
    struct Keys {
        SipKey send{}, receive{};
    };
    struct Bucket {
        f64 handshakes = 0, packets = 0, bytes = 0, last = 0;
    };
    struct PeerState_ {
        PeerId id = kNoPeer;
        Address address;
        PeerState state = PeerState::Connecting;
        u64 nonce = 0;
        u32 remote_id = 0;
        f64 started = 0.0, last_received = 0.0, last_sent = -1e300, last_attempt = -1e300;
        bool challenged = false;   // a client that has the server's cookie
        u64 cookie_time = 0, cookie = 0;
        Keys keys;
        std::unique_ptr<Connection> connection;
    };
    void SendRaw(const Address& to, Kind kind, std::span<const u8> payload = {});
    void SendSigned(const PeerState_& peer, Kind kind, std::span<const u8> payload = {});
    bool Verify(const PeerState_& peer, std::span<const u8> datagram) const;
    u64 Cookie(const Address& from, u64 nonce, u64 time_ms) const;
    bool Allow(const Address& from, usize bytes, bool handshake, f64 now);
    PeerState_* ByAddress(const Address& a);
    void Drop(PeerId peer, DisconnectReason reason);
    void Handle(const Address& from, std::span<const u8> datagram, f64 now);

    DatagramSocket& socket_;
    HostConfig config_;
    bool listening_ = false;
    PeerId next_id_ = 1;
    u64 nonce_seed_;
    std::unordered_map<u64, Bucket> buckets_;
    std::unordered_map<u64, u64> used_cookies_; // cookie -> its time (ms), until it expires
    HostSecurityStats security_;
    std::vector<std::unique_ptr<PeerState_>> peers_;
    std::vector<NetEvent> events_;
};

} // namespace aether::net
