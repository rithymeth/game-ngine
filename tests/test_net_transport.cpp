#include "aether/net/bytes.h"
#include "aether/net/transport.h"
#include "test_framework.h"

#include <algorithm>
#include <limits>

// Phase 22 step 1: byte serialization and the simulated network.

using namespace aether;
using namespace aether::net;

AETHER_TEST(NetBytes_RoundTrip) {
    ByteWriter w;
    w.U8(200);
    w.U16(60000);
    w.U32(4000000000u);
    w.U64(0x0123456789ABCDEFull);
    w.I32(-12345);
    w.F32(3.5f);
    w.F64(-2.25);
    w.Bool(true);
    w.String("hello, world");
    w.String("");
    for (u64 v : {0ull, 1ull, 127ull, 128ull, 300ull, 16383ull, 16384ull, std::numeric_limits<u64>::max()}) w.Varint(v);

    ByteReader r(w.Data());
    AETHER_CHECK(r.U8() == 200 && r.U16() == 60000 && r.U32() == 4000000000u);
    AETHER_CHECK(r.U64() == 0x0123456789ABCDEFull && r.I32() == -12345);
    AETHER_CHECK(r.F32() == 3.5f && r.F64() == -2.25 && r.Bool());
    AETHER_CHECK(r.String() == "hello, world" && r.String().empty());
    for (u64 v : {0ull, 1ull, 127ull, 128ull, 300ull, 16383ull, 16384ull, std::numeric_limits<u64>::max()}) AETHER_CHECK(r.Varint() == v);
    AETHER_CHECK(r.Ok() && r.Remaining() == 0);
}

AETHER_TEST(NetBytes_VarintSizes) {
    auto size = [](u64 v) {
        ByteWriter w;
        w.Varint(v);
        return w.Size();
    };
    AETHER_CHECK(size(0) == 1 && size(127) == 1 && size(128) == 2 && size(16383) == 2 && size(16384) == 3);
    AETHER_CHECK(size(std::numeric_limits<u64>::max()) == 10);
}

AETHER_TEST(NetBytes_ReaderFailsSafely) {
    const std::vector<u8> three = {1, 2, 3};
    ByteReader r(three);
    AETHER_CHECK(r.U16() == 0x0201);
    AETHER_CHECK(r.Ok());
    AETHER_CHECK(r.U32() == 0 && !r.Ok()); // only one byte left
    AETHER_CHECK(r.U8() == 0);             // stays failed; reads nothing more

    // A string whose length runs past the buffer.
    ByteWriter w;
    w.Varint(1000);
    w.U8(1);
    ByteReader s(w.Data());
    AETHER_CHECK(s.String().empty() && !s.Ok());

    // Bytes past the end.
    ByteReader b(three);
    AETHER_CHECK(b.Bytes(5).empty() && !b.Ok());

    // A varint that never ends, or overflows 64 bits.
    const std::vector<u8> endless(12, 0xFF);
    ByteReader e(endless);
    e.Varint();
    AETHER_CHECK(!e.Ok());
    std::vector<u8> overflow(9, 0xFF);
    overflow.push_back(0x7F);
    ByteReader o(overflow);
    o.Varint();
    AETHER_CHECK(!o.Ok());
}

AETHER_TEST(Loopback_DeliversAfterLatency) {
    LoopbackNetwork net;
    net.conditions.latency = 0.05;
    LoopbackTransport& a = net.CreateEndpoint();
    LoopbackTransport& b = net.CreateEndpoint();
    AETHER_CHECK(a.LocalAddress() == 1 && b.LocalAddress() == 2);

    const std::vector<u8> payload = {9, 8, 7};
    AETHER_CHECK(a.Send(b.LocalAddress(), payload));
    Datagram d;
    AETHER_CHECK(!b.Receive(d) && net.InFlight() == 1);
    net.Advance(0.04);
    AETHER_CHECK(!b.Receive(d));
    net.Advance(0.02);
    AETHER_CHECK(b.Receive(d) && d.from == a.LocalAddress() && d.data == payload);
    AETHER_CHECK(!b.Receive(d) && net.InFlight() == 0);
}

AETHER_TEST(Loopback_NoLatencyDeliversImmediately) {
    LoopbackNetwork net;
    LoopbackTransport& a = net.CreateEndpoint();
    LoopbackTransport& b = net.CreateEndpoint();
    const std::vector<u8> payload = {1};
    AETHER_CHECK(a.Send(b.LocalAddress(), payload));
    Datagram d;
    AETHER_CHECK(b.Receive(d) && d.data == payload);
}

AETHER_TEST(Loopback_RejectsBadSends) {
    LoopbackNetwork net;
    LoopbackTransport& a = net.CreateEndpoint();
    net.CreateEndpoint();
    const std::vector<u8> ok(10, 0);
    AETHER_CHECK(!a.Send(0, ok));
    AETHER_CHECK(!a.Send(77, ok)); // no such address
    const std::vector<u8> huge(a.MaxDatagramSize() + 1, 0);
    AETHER_CHECK(!a.Send(2, huge));
    const std::vector<u8> max(a.MaxDatagramSize(), 0);
    AETHER_CHECK(a.Send(2, max));
}

AETHER_TEST(Loopback_LossIsSeededAndCounted) {
    auto run = [](u64 seed) {
        LoopbackNetwork net(seed);
        net.conditions.loss = 0.5;
        LoopbackTransport& a = net.CreateEndpoint();
        LoopbackTransport& b = net.CreateEndpoint();
        std::vector<u8> pattern;
        for (u8 i = 0; i < 200; ++i) {
            const std::vector<u8> m = {i};
            a.Send(b.LocalAddress(), m);
        }
        Datagram d;
        while (b.Receive(d)) pattern.push_back(d.data[0]);
        AETHER_CHECK(pattern.size() + net.Dropped() == 200);
        return pattern;
    };
    const auto first = run(42);
    AETHER_CHECK(first == run(42));  // same seed, same losses
    AETHER_CHECK(first != run(43));
    AETHER_CHECK(first.size() > 60 && first.size() < 140);

    LoopbackNetwork all;
    all.conditions.loss = 1.0;
    LoopbackTransport& x = all.CreateEndpoint();
    LoopbackTransport& y = all.CreateEndpoint();
    const std::vector<u8> m = {1};
    x.Send(y.LocalAddress(), m);
    Datagram d;
    AETHER_CHECK(!y.Receive(d) && all.Dropped() == 1);
}

AETHER_TEST(Loopback_JitterReordersAndDuplicates) {
    LoopbackNetwork net(7);
    net.conditions.latency = 0.01;
    net.conditions.jitter = 0.2;
    LoopbackTransport& a = net.CreateEndpoint();
    LoopbackTransport& b = net.CreateEndpoint();
    for (u8 i = 0; i < 50; ++i) {
        const std::vector<u8> m = {i};
        a.Send(b.LocalAddress(), m);
    }
    net.Advance(1.0);
    std::vector<u8> got;
    Datagram d;
    while (b.Receive(d)) got.push_back(d.data[0]);
    AETHER_CHECK(got.size() == 50);
    AETHER_CHECK(!std::is_sorted(got.begin(), got.end())); // arrived out of order
    std::sort(got.begin(), got.end());
    for (u8 i = 0; i < 50; ++i) AETHER_CHECK(got[i] == i);

    LoopbackNetwork dup(3);
    dup.conditions.duplicate = 1.0;
    LoopbackTransport& x = dup.CreateEndpoint();
    LoopbackTransport& y = dup.CreateEndpoint();
    const std::vector<u8> m = {5};
    x.Send(y.LocalAddress(), m);
    int copies = 0;
    while (y.Receive(d)) ++copies;
    AETHER_CHECK(copies == 2);
}
