#include "aether/net/endpoint.h"
#include "test_framework.h"

#include <algorithm>
#include <cstring>

// Phase 22 step 1: the connection layer - handshake, reliable ordered and
// unreliable channels, resends under loss, RTT, timeouts and disconnects.

using namespace aether;
using namespace aether::net;

namespace {

std::vector<u8> Bytes(u32 v) {
    std::vector<u8> b(4);
    std::memcpy(b.data(), &v, 4);
    return b;
}
u32 Value(const std::vector<u8>& b) {
    u32 v = 0;
    if (b.size() >= 4) std::memcpy(&v, b.data(), 4);
    return v;
}

struct Pair {
    LoopbackNetwork net;
    LoopbackTransport& server_link;
    LoopbackTransport& client_link;
    NetEndpoint server;
    NetEndpoint client;
    f64 t = 0.0;
    std::vector<NetEvent> server_events, client_events;

    explicit Pair(u64 seed = 1, const NetConfig& server_config = {}, const NetConfig& client_config = {})
        : net(seed),
          server_link(net.CreateEndpoint()),
          client_link(net.CreateEndpoint()),
          server(server_link, server_config),
          client(client_link, client_config) {
        server.Listen();
    }

    void Step(f64 dt) {
        t += dt;
        net.Advance(dt);
        server.Update(t);
        client.Update(t);
        NetEvent e;
        while (server.Poll(e)) server_events.push_back(std::move(e));
        while (client.Poll(e)) client_events.push_back(std::move(e));
    }
    void Run(f64 seconds, f64 dt = 0.01) {
        const int steps = static_cast<int>(seconds / dt);
        for (int i = 0; i < steps; ++i) Step(dt);
    }
    bool ConnectClient() {
        client.Connect(server_link.LocalAddress());
        Run(1.0);
        return client.IsConnected(server_link.LocalAddress());
    }
};

usize Count(const std::vector<NetEvent>& events, NetEventType type) {
    return static_cast<usize>(std::count_if(events.begin(), events.end(), [&](const NetEvent& e) { return e.type == type; }));
}

std::vector<u32> Messages(const std::vector<NetEvent>& events, Channel channel) {
    std::vector<u32> out;
    for (const NetEvent& e : events) {
        if (e.type == NetEventType::Message && e.channel == channel) out.push_back(Value(e.data));
    }
    return out;
}

} // namespace

AETHER_TEST(NetEndpoint_Handshake) {
    Pair p;
    AETHER_CHECK(p.ConnectClient());
    AETHER_CHECK(Count(p.server_events, NetEventType::Connected) == 1 && Count(p.client_events, NetEventType::Connected) == 1);
    AETHER_CHECK(p.server_events[0].peer == p.client_link.LocalAddress());
    AETHER_CHECK(p.client_events[0].peer == p.server_link.LocalAddress());
    AETHER_CHECK(p.server.ConnectedPeers().size() == 1 && p.server.IsConnected(p.client_link.LocalAddress()));

    // Connecting twice to the same server is refused.
    AETHER_CHECK(!p.client.Connect(p.server_link.LocalAddress()));
    // Connection survives idle time through keepalives.
    p.Run(20.0);
    AETHER_CHECK(p.client.IsConnected(p.server_link.LocalAddress()) && p.server.IsConnected(p.client_link.LocalAddress()));
    AETHER_CHECK(Count(p.server_events, NetEventType::Disconnected) == 0);
}

AETHER_TEST(NetEndpoint_HandshakeSurvivesLoss) {
    Pair p(5);
    p.net.conditions.loss = 0.6;
    p.net.conditions.latency = 0.02;
    p.client.Connect(p.server_link.LocalAddress());
    p.Run(4.0);
    AETHER_CHECK(p.client.IsConnected(p.server_link.LocalAddress()) && p.server.IsConnected(p.client_link.LocalAddress()));
    AETHER_CHECK(Count(p.server_events, NetEventType::Connected) == 1 && Count(p.client_events, NetEventType::Connected) == 1);
}

AETHER_TEST(NetEndpoint_VersionMismatchAndFullAreRejected) {
    NetConfig server_config;
    server_config.protocol_version = 2;
    Pair mismatch(1, server_config);
    mismatch.client.Connect(mismatch.server_link.LocalAddress());
    mismatch.Run(1.0);
    AETHER_CHECK(!mismatch.client.IsConnected(mismatch.server_link.LocalAddress()));
    AETHER_CHECK(mismatch.client_events.size() == 1 && mismatch.client_events[0].type == NetEventType::Disconnected &&
                 mismatch.client_events[0].reason == DisconnectReason::VersionMismatch);
    AETHER_CHECK(mismatch.server.ConnectedPeers().empty());

    NetConfig one;
    one.max_clients = 1;
    LoopbackNetwork net;
    LoopbackTransport& s = net.CreateEndpoint();
    LoopbackTransport& c1 = net.CreateEndpoint();
    LoopbackTransport& c2 = net.CreateEndpoint();
    NetEndpoint server(s, one), a(c1), b(c2);
    server.Listen();
    a.Connect(s.LocalAddress());
    f64 t = 0.0;
    for (int i = 0; i < 50; ++i) {
        t += 0.01;
        server.Update(t), a.Update(t);
    }
    b.Connect(s.LocalAddress());
    for (int i = 0; i < 50; ++i) {
        t += 0.01;
        server.Update(t), a.Update(t), b.Update(t);
    }
    AETHER_CHECK(a.IsConnected(s.LocalAddress()) && !b.IsConnected(s.LocalAddress()));
    NetEvent e;
    bool full = false;
    while (b.Poll(e)) full = full || (e.type == NetEventType::Disconnected && e.reason == DisconnectReason::Full);
    AETHER_CHECK(full);
}

AETHER_TEST(NetEndpoint_ConnectToNobodyTimesOut) {
    NetConfig cfg;
    cfg.connect_timeout = 1.0;
    Pair p(1, {}, cfg);
    p.client.Connect(77); // no such address: the datagrams go nowhere
    p.Run(2.0);
    AETHER_CHECK(p.client_events.size() == 1 && p.client_events[0].reason == DisconnectReason::Timeout);
    AETHER_CHECK(!p.client.IsConnected(77));
}

AETHER_TEST(NetEndpoint_MessagesBothChannelsBothWays) {
    Pair p;
    AETHER_CHECK(p.ConnectClient());
    const NetAddress server = p.server_link.LocalAddress(), client = p.client_link.LocalAddress();
    p.server_events.clear(), p.client_events.clear();

    for (u32 i = 0; i < 5; ++i) {
        AETHER_CHECK(p.client.Send(server, Channel::ReliableOrdered, Bytes(i)));
        AETHER_CHECK(p.client.Send(server, Channel::Unreliable, Bytes(100 + i)));
        AETHER_CHECK(p.server.Send(client, Channel::ReliableOrdered, Bytes(200 + i)));
    }
    p.Run(0.5);
    AETHER_CHECK((Messages(p.server_events, Channel::ReliableOrdered) == std::vector<u32>{0, 1, 2, 3, 4}));
    AETHER_CHECK((Messages(p.server_events, Channel::Unreliable) == std::vector<u32>{100, 101, 102, 103, 104}));
    AETHER_CHECK((Messages(p.client_events, Channel::ReliableOrdered) == std::vector<u32>{200, 201, 202, 203, 204}));

    // Broadcast reaches every connected peer.
    p.server_events.clear(), p.client_events.clear();
    p.server.Broadcast(Channel::ReliableOrdered, Bytes(9));
    p.Run(0.3);
    AETHER_CHECK((Messages(p.client_events, Channel::ReliableOrdered) == std::vector<u32>{9}));
}

AETHER_TEST(NetEndpoint_SendRules) {
    Pair p;
    const NetAddress server = p.server_link.LocalAddress();
    AETHER_CHECK(!p.client.Send(server, Channel::Unreliable, Bytes(1))); // not connected yet
    AETHER_CHECK(p.ConnectClient());
    AETHER_CHECK(!p.client.Send(99, Channel::Unreliable, Bytes(1)));     // not a peer
    const std::vector<u8> huge(p.client.Config().max_message_size + 1, 0);
    AETHER_CHECK(!p.client.Send(server, Channel::ReliableOrdered, huge));
    const std::vector<u8> max(p.client.Config().max_message_size, 7);
    AETHER_CHECK(p.client.Send(server, Channel::ReliableOrdered, max));
    p.server_events.clear();
    p.Run(0.3);
    AETHER_CHECK(Count(p.server_events, NetEventType::Message) == 1 && p.server_events[0].data == max);

    // A message limit that leaves room for the packet header.
    AETHER_CHECK(p.client.Config().max_message_size + 24 <= p.client_link.MaxDatagramSize());
}

AETHER_TEST(NetEndpoint_ReliableSurvivesLossReorderAndDuplicates) {
    Pair p(11);
    AETHER_CHECK(p.ConnectClient());
    p.net.conditions.latency = 0.03;
    p.net.conditions.jitter = 0.08; // reorders
    p.net.conditions.loss = 0.3;
    p.net.conditions.duplicate = 0.1;
    const NetAddress server = p.server_link.LocalAddress(), client = p.client_link.LocalAddress();
    p.server_events.clear(), p.client_events.clear();

    constexpr u32 kCount = 300;
    for (u32 i = 0; i < kCount; ++i) {
        // Spread the sends out so the window never overflows.
        while (!p.client.Send(server, Channel::ReliableOrdered, Bytes(i))) p.Step(0.01);
        p.server.Send(client, Channel::ReliableOrdered, Bytes(1000 + i));
        if (i % 3 == 0) p.Step(0.01);
    }
    p.Run(20.0);

    std::vector<u32> up = Messages(p.server_events, Channel::ReliableOrdered);
    std::vector<u32> down = Messages(p.client_events, Channel::ReliableOrdered);
    AETHER_CHECK(up.size() == kCount && down.size() == kCount); // every one, exactly once
    bool ordered = true;
    for (u32 i = 0; i < up.size() && ordered; ++i) ordered = up[i] == i;
    for (u32 i = 0; i < down.size() && ordered; ++i) ordered = down[i] == 1000 + i;
    AETHER_CHECK(ordered);
    PeerStats stats;
    AETHER_CHECK(p.client.Stats(server, stats) && stats.reliable_resends > 0 && stats.reliable_pending == 0);
    AETHER_CHECK(p.client.IsConnected(server)); // still alive after all that
}

AETHER_TEST(NetEndpoint_UnreliableIsLossyButNotDuplicated) {
    Pair p(21);
    AETHER_CHECK(p.ConnectClient());
    p.net.conditions.loss = 0.4;
    p.server_events.clear();
    const NetAddress server = p.server_link.LocalAddress();
    for (u32 i = 0; i < 200; ++i) {
        p.client.Send(server, Channel::Unreliable, Bytes(i));
        p.Step(0.01);
    }
    p.Run(1.0);
    const std::vector<u32> got = Messages(p.server_events, Channel::Unreliable);
    AETHER_CHECK(got.size() > 60 && got.size() < 180);
    AETHER_CHECK(std::is_sorted(got.begin(), got.end()));
    AETHER_CHECK(std::adjacent_find(got.begin(), got.end()) == got.end());
}

AETHER_TEST(NetEndpoint_DuplicatedDatagramsAreDroppedOnce) {
    Pair p(2);
    AETHER_CHECK(p.ConnectClient());
    p.net.conditions.duplicate = 1.0; // every datagram twice
    p.server_events.clear();
    const NetAddress server = p.server_link.LocalAddress();
    for (u32 i = 0; i < 20; ++i) {
        p.client.Send(server, Channel::Unreliable, Bytes(i));
        p.client.Send(server, Channel::ReliableOrdered, Bytes(i));
        p.Step(0.02);
    }
    p.Run(1.0);
    AETHER_CHECK(Messages(p.server_events, Channel::Unreliable).size() == 20);
    AETHER_CHECK(Messages(p.server_events, Channel::ReliableOrdered).size() == 20);
}

AETHER_TEST(NetEndpoint_LargeBurstSpansPackets) {
    Pair p(4);
    AETHER_CHECK(p.ConnectClient());
    p.server_events.clear();
    const NetAddress server = p.server_link.LocalAddress();
    // 120 messages of 900 bytes in one frame: far more than a datagram holds.
    for (u32 i = 0; i < 120; ++i) {
        std::vector<u8> m(900, static_cast<u8>(i));
        if (!p.client.Send(server, Channel::ReliableOrdered, m)) break; // the pending cap is 256, so all fit
    }
    p.Run(3.0);
    const usize got = Count(p.server_events, NetEventType::Message);
    AETHER_CHECK(got == 120);
    for (usize i = 0; i < got; ++i) AETHER_CHECK(p.server_events[i].data.size() == 900 && p.server_events[i].data[0] == static_cast<u8>(i));
}

AETHER_TEST(NetEndpoint_BackpressureWhenPeerIsGone) {
    NetConfig cfg;
    cfg.max_reliable_pending = 8;
    Pair p(1, {}, cfg);
    AETHER_CHECK(p.ConnectClient());
    p.net.conditions.loss = 1.0; // nothing gets acknowledged
    const NetAddress server = p.server_link.LocalAddress();
    for (u32 i = 0; i < 8; ++i) AETHER_CHECK(p.client.Send(server, Channel::ReliableOrdered, Bytes(i)));
    AETHER_CHECK(!p.client.Send(server, Channel::ReliableOrdered, Bytes(8)));
    PeerStats stats;
    AETHER_CHECK(p.client.Stats(server, stats) && stats.reliable_pending == 8);
    AETHER_CHECK(p.client.Send(server, Channel::Unreliable, Bytes(1))); // unreliable is never held back
}

AETHER_TEST(NetEndpoint_RttFollowsLatency) {
    Pair p;
    AETHER_CHECK(p.ConnectClient());
    p.net.conditions.latency = 0.05; // 100 ms round trip
    const NetAddress server = p.server_link.LocalAddress();
    for (u32 i = 0; i < 100; ++i) {
        p.client.Send(server, Channel::ReliableOrdered, Bytes(i));
        p.Step(0.02);
    }
    p.Run(1.0);
    PeerStats stats;
    AETHER_CHECK(p.client.Stats(server, stats));
    AETHER_CHECK(stats.rtt > 0.09 && stats.rtt < 0.16);
    AETHER_CHECK(stats.packets_sent > 0 && stats.packets_received > 0 && stats.bytes_sent > 0 && stats.bytes_received > 0);
    AETHER_CHECK(!p.client.Stats(99, stats));
}

AETHER_TEST(NetEndpoint_SilentPeerTimesOut) {
    NetConfig cfg;
    cfg.timeout = 2.0;
    Pair p(1, cfg, cfg);
    AETHER_CHECK(p.ConnectClient());
    p.server_events.clear(), p.client_events.clear();
    // The server stops updating (a crash): the client notices.
    for (int i = 0; i < 400; ++i) {
        p.t += 0.01;
        p.net.Advance(0.01);
        p.client.Update(p.t);
    }
    NetEvent e;
    std::vector<NetEvent> events;
    while (p.client.Poll(e)) events.push_back(e);
    AETHER_CHECK(events.size() == 1 && events[0].type == NetEventType::Disconnected && events[0].reason == DisconnectReason::Timeout);
    AETHER_CHECK(!p.client.IsConnected(p.server_link.LocalAddress()));
}

AETHER_TEST(NetEndpoint_GracefulDisconnect) {
    Pair p;
    AETHER_CHECK(p.ConnectClient());
    p.server_events.clear(), p.client_events.clear();
    p.client.Disconnect(p.server_link.LocalAddress());
    p.Run(0.2);
    AETHER_CHECK(p.client_events.size() == 1 && p.client_events[0].reason == DisconnectReason::Local);
    AETHER_CHECK(p.server_events.size() == 1 && p.server_events[0].reason == DisconnectReason::Remote);
    AETHER_CHECK(p.server.ConnectedPeers().empty() && !p.client.IsConnected(p.server_link.LocalAddress()));
    // Messages to a gone peer are refused, and it can connect again.
    AETHER_CHECK(!p.server.Send(p.client_link.LocalAddress(), Channel::Unreliable, Bytes(1)));
    AETHER_CHECK(p.ConnectClient());
}

AETHER_TEST(NetEndpoint_ServerDisconnectsAllPeers) {
    LoopbackNetwork net;
    LoopbackTransport& s = net.CreateEndpoint();
    LoopbackTransport& c1 = net.CreateEndpoint();
    LoopbackTransport& c2 = net.CreateEndpoint();
    NetEndpoint server(s), a(c1), b(c2);
    server.Listen();
    a.Connect(s.LocalAddress());
    b.Connect(s.LocalAddress());
    f64 t = 0.0;
    auto pump = [&](int n) {
        for (int i = 0; i < n; ++i) {
            t += 0.01;
            server.Update(t), a.Update(t), b.Update(t);
        }
    };
    pump(50);
    AETHER_CHECK(server.ConnectedPeers().size() == 2);
    server.DisconnectAll();
    pump(20);
    AETHER_CHECK(server.ConnectedPeers().empty() && !a.IsConnected(s.LocalAddress()) && !b.IsConnected(s.LocalAddress()));
}

AETHER_TEST(NetEndpoint_IgnoresStrangersAndGarbage) {
    Pair p;
    LoopbackTransport& stranger = p.net.CreateEndpoint();
    const std::vector<u8> garbage = {4, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}; // Data from a non-peer
    stranger.Send(p.server_link.LocalAddress(), garbage);
    const std::vector<u8> bad_magic = {1, 0, 0, 0, 0, 1, 0, 0, 0};            // Connect, wrong magic
    stranger.Send(p.server_link.LocalAddress(), bad_magic);
    const std::vector<u8> empty;
    stranger.Send(p.server_link.LocalAddress(), empty);
    const std::vector<u8> truncated = {1, 0x41};
    stranger.Send(p.server_link.LocalAddress(), truncated);
    p.Run(0.2);
    AETHER_CHECK(p.server.ConnectedPeers().empty() && p.server_events.empty());

    // Garbage from a real peer doesn't break the connection or deliver anything.
    AETHER_CHECK(p.ConnectClient());
    p.server_events.clear();
    const std::vector<u8> junk = {4, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 9, 200, 200};
    p.client_link.Send(p.server_link.LocalAddress(), junk);
    p.Run(0.3);
    AETHER_CHECK(Count(p.server_events, NetEventType::Message) == 0 && p.server.IsConnected(p.client_link.LocalAddress()));
}

AETHER_TEST(NetEndpoint_SequenceNumbersWrap) {
    Pair p;
    AETHER_CHECK(p.ConnectClient());
    p.server_events.clear();
    const NetAddress server = p.server_link.LocalAddress();
    // More than 65536 packets and reliable ids: both counters wrap.
    constexpr u32 kTotal = 70000;
    u32 sent = 0;
    for (int guard = 0; sent < kTotal && guard < 400000; ++guard) {
        if (p.client.Send(server, Channel::ReliableOrdered, Bytes(sent))) ++sent;
        else p.Step(0.001);
        if (sent % 100 == 0) p.Step(0.001);
    }
    p.Run(2.0, 0.001);
    const std::vector<u32> got = Messages(p.server_events, Channel::ReliableOrdered);
    AETHER_CHECK(got.size() == kTotal);
    bool ordered = true;
    for (u32 i = 0; i < got.size() && ordered; ++i) ordered = got[i] == i;
    AETHER_CHECK(ordered);
}
