#pragma once

#include "aether/net/transport.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace aether::net {

// A real UDP socket (IPv4) as a Transport. Works on Windows (Winsock) and
// POSIX. Non-blocking: Send never waits and Receive returns false when
// nothing has arrived, so it fits the poll-from-the-game-loop model.
//
// A NetAddress is a small handle for an (IP, port) pair, because a socket
// address doesn't fit in 32 bits. Handles are made by AddressFor (to connect
// somewhere) or when a datagram arrives from a new place, and stay valid
// while the pair is in use. The table is bounded: when it fills, the pair
// that has been quiet the longest is dropped, so spoofed source addresses
// can't grow memory without limit and live connections (which are never
// quiet for long) are not affected.

struct UdpConfig {
    u16 port = 0;                 // 0 = any free port
    bool allow_broadcast = false; // needed for Broadcast (LAN discovery)
    usize max_addresses = 1024;
};

class UdpTransport final : public Transport {
public:
    // Opens and binds a socket; null (and `error`) if that fails.
    static std::unique_ptr<UdpTransport> Create(const UdpConfig& config = {}, std::string* error = nullptr);
    ~UdpTransport() override;
    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    // This socket has no handle for itself (it is never a destination): a fixed marker.
    NetAddress LocalAddress() const override { return kSelf; }
    usize MaxDatagramSize() const override { return 1200; }
    bool Send(NetAddress to, std::span<const u8> data) override;
    bool Receive(Datagram& out) override;
    bool Broadcast(u16 port, std::span<const u8> data) override;

    u16 Port() const { return port_; }
    // A handle for "host:port" (a dotted IPv4 address or a name); 0 if it doesn't resolve.
    NetAddress AddressFor(const std::string& host, u16 port);
    // The same machine as `address`, on another port (a host's game port from its discovery reply).
    NetAddress AddressWithPort(NetAddress address, u16 port);
    // "a.b.c.d" and the port a handle stands for; false if it is not (or no longer) known.
    bool Describe(NetAddress address, std::string& host, u16& port) const;
    usize KnownAddresses() const { return by_id_.size(); }

    static constexpr NetAddress kSelf = 0xFFFFFFFEu;

private:
    UdpTransport() = default;
    struct Entry {
        u32 ip = 0; // network byte order
        u16 port = 0;
        u64 last_used = 0;
    };
    NetAddress Handle(u32 ip, u16 port);
    void Evict();

    std::intptr_t socket_ = -1;
    u16 port_ = 0;
    usize max_addresses_ = 1024;
    u64 clock_ = 0; // a counter, not time: only the order of use matters
    NetAddress next_id_ = 1;
    std::unordered_map<NetAddress, Entry> by_id_;
    std::unordered_map<u64, NetAddress> by_key_;
};

} // namespace aether::net
