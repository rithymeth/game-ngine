#pragma once

#include "aether/core/base.h"

#include <deque>
#include <memory>
#include <span>
#include <vector>

namespace aether::net {

// A datagram transport: unreliable, unordered, size-limited packets between
// numbered addresses. The connection layer (endpoint.h) builds reliability on
// top, so it runs over a real UDP socket or, as here, a simulated network.

using NetAddress = u32; // 0 is never a valid address

struct Datagram {
    NetAddress from = 0;
    std::vector<u8> data;
};

class Transport {
public:
    virtual ~Transport() = default;
    virtual NetAddress LocalAddress() const = 0;
    virtual usize MaxDatagramSize() const = 0;
    // False if the datagram is too big or the address is unknown; true does not mean delivered.
    virtual bool Send(NetAddress to, std::span<const u8> data) = 0;
    virtual bool Receive(Datagram& out) = 0;
};

// How the simulated link treats datagrams.
struct LinkConditions {
    f64 latency = 0.0;   // seconds, one way
    f64 jitter = 0.0;    // extra random delay in [0, jitter]; enough of it reorders datagrams
    f64 loss = 0.0;      // chance a datagram is dropped, 0..1
    f64 duplicate = 0.0; // chance a datagram is delivered twice
};

class LoopbackNetwork;

class LoopbackTransport final : public Transport {
public:
    NetAddress LocalAddress() const override { return address_; }
    usize MaxDatagramSize() const override { return 1200; }
    bool Send(NetAddress to, std::span<const u8> data) override;
    bool Receive(Datagram& out) override;

private:
    friend class LoopbackNetwork;
    LoopbackTransport(LoopbackNetwork& network, NetAddress address) : network_(network), address_(address) {}
    LoopbackNetwork& network_;
    NetAddress address_;
    std::deque<Datagram> inbox_;
};

// An in-process network with a manual clock and a seeded random generator, so
// latency, loss and reordering are reproducible in tests (and in the editor's
// simulated-latency PIE). Advance() moves time and delivers what is due.
class LoopbackNetwork {
public:
    explicit LoopbackNetwork(u64 seed = 1) : rng_(seed != 0 ? seed : 1) {}

    LoopbackTransport& CreateEndpoint();
    LinkConditions conditions; // applied to every datagram sent

    f64 Now() const { return now_; }
    void Advance(f64 dt);
    usize InFlight() const { return in_flight_.size(); }
    u64 Dropped() const { return dropped_; }

private:
    friend class LoopbackTransport;
    struct Pending {
        f64 deliver_at;
        u64 order; // keeps equal delivery times in send order
        NetAddress to;
        Datagram datagram;
    };
    void Enqueue(NetAddress from, NetAddress to, std::span<const u8> data);
    f64 Random(); // [0, 1)
    void Deliver();

    f64 now_ = 0.0;
    u64 rng_;
    u64 order_ = 0;
    u64 dropped_ = 0;
    std::vector<std::unique_ptr<LoopbackTransport>> endpoints_;
    std::vector<Pending> in_flight_;
};

} // namespace aether::net
