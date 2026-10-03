#pragma once

#include "aether/core/base.h"

#include <map>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace aether::net {

// An IPv4 endpoint.
struct Address {
    u32 ip = 0; // host order: 127.0.0.1 is 0x7F000001
    u16 port = 0;
    static Address Loopback(u16 port) { return {0x7F000001u, port}; }
    static std::optional<Address> Parse(const std::string& text); // "a.b.c.d:port"
    std::string ToString() const;
    bool operator==(const Address&) const = default;
    auto operator<=>(const Address&) const = default;
};

// Datagrams, unreliable and unordered (Phase 22 step 1,
// docs/design/PHASE_SPECS.md §22.1). Never blocks.
class DatagramSocket {
public:
    virtual ~DatagramSocket() = default;
    virtual bool Send(const Address& to, std::span<const u8> data) = 0;
    // The next datagram that has arrived; false when there's none.
    virtual bool Receive(Address& from, std::vector<u8>& data) = 0;
    virtual Address LocalAddress() const = 0;
};

// A UDP socket (non-blocking), bound to a port (0: any free one).
class UdpSocket final : public DatagramSocket {
public:
    UdpSocket() = default;
    ~UdpSocket() override;
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    bool Open(u16 port = 0, std::string* error = nullptr);
    void Close();
    bool IsOpen() const { return handle_ != kInvalid; }
    bool Send(const Address& to, std::span<const u8> data) override;
    bool Receive(Address& from, std::vector<u8>& data) override;
    Address LocalAddress() const override;

private:
    static constexpr i64 kInvalid = -1;
    i64 handle_ = kInvalid;
    u16 port_ = 0;
};

// What a simulated link does to datagrams.
struct LinkConditions {
    f64 latency = 0.0;   // seconds, one way
    f64 jitter = 0.0;    // up to this much more, at random (so packets can reorder)
    f32 loss = 0.0f;     // 0..1
    f32 duplicate = 0.0f; // 0..1: delivered twice
};

// An in-memory network of sockets (one process): datagrams are delivered
// at `now` + latency (and jitter), or lost, or duplicated, by seeded
// chance, so a run is repeatable. Time moves only with Advance().
class LoopbackNetwork {
public:
    explicit LoopbackNetwork(u64 seed = 1) : rng_(seed) {}
    // A socket at 127.0.0.1:port (0: the next free port); null if the port is taken.
    std::unique_ptr<DatagramSocket> Open(u16 port = 0);
    LinkConditions conditions;
    void Advance(f64 seconds) { now_ += seconds; }
    f64 Now() const { return now_; }
    usize InFlight() const { return in_flight_.size(); }
    u64 Sent() const { return sent_; }
    u64 Dropped() const { return dropped_; }

private:
    friend class LoopbackSocket;
    struct Datagram {
        f64 deliver_at;
        u64 order;
        Address from, to;
        std::vector<u8> data;
    };
    void Post(const Address& from, const Address& to, std::span<const u8> data);
    bool Take(const Address& to, Address& from, std::vector<u8>& data);
    void Close(u16 port);

    std::mt19937_64 rng_;
    f64 now_ = 0.0;
    u64 order_ = 0, sent_ = 0, dropped_ = 0;
    u16 next_port_ = 40000;
    std::map<u16, bool> open_;
    std::vector<Datagram> in_flight_;
};

} // namespace aether::net
