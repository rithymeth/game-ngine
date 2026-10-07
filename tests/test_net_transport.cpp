#include "aether/net/host.h"
#include "test_framework.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

// Phase 22 step 1: the network transport - addresses, the simulated
// network, connections (acks, reliable ordering under loss, duplication
// and reordering, fragments, sequenced and unreliable channels, RTT), and
// hosts (handshakes, refusals, goodbyes, timeouts, keepalives), plus real
// UDP on localhost.

using namespace aether;
using namespace aether::net;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

constexpr f64 kStep = 1.0 / 60.0;

std::vector<u8> Bytes(u32 v) { return {static_cast<u8>(v), static_cast<u8>(v >> 8), static_cast<u8>(v >> 16), static_cast<u8>(v >> 24)}; }
u32 Value(const std::vector<u8>& b) { return b.size() < 4 ? 0xFFFFFFFFu : b[0] | (b[1] << 8) | (b[2] << 16) | (static_cast<u32>(b[3]) << 24); }

// A server and clients on a simulated network.
struct Sim {
    LoopbackNetwork net;
    std::unique_ptr<DatagramSocket> server_socket;
    std::unique_ptr<NetHost> server;
    std::vector<std::unique_ptr<DatagramSocket>> client_sockets;
    std::vector<std::unique_ptr<NetHost>> clients;
    std::vector<NetEvent> server_events;
    std::vector<std::vector<NetEvent>> client_events;

    explicit Sim(HostConfig config = {}, u64 seed = 1) : net(seed) {
        server_socket = net.Open(7777);
        server = std::make_unique<NetHost>(*server_socket, config);
        server->Listen();
    }
    usize AddClient(HostConfig config = {}) {
        client_sockets.push_back(net.Open());
        clients.push_back(std::make_unique<NetHost>(*client_sockets.back(), config));
        client_events.emplace_back();
        clients.back()->Connect(Address::Loopback(7777), net.Now());
        return clients.size() - 1;
    }
    void Step(int frames = 1, bool update_server = true) {
        for (int i = 0; i < frames; ++i) {
            net.Advance(kStep);
            if (update_server) {
                server->Update(net.Now());
                for (NetEvent& e : server->TakeEvents()) server_events.push_back(std::move(e));
            }
            for (usize c = 0; c < clients.size(); ++c) {
                clients[c]->Update(net.Now());
                for (NetEvent& e : clients[c]->TakeEvents()) client_events[c].push_back(std::move(e));
            }
        }
    }
    static usize Count(const std::vector<NetEvent>& events, NetEventType type) {
        return static_cast<usize>(std::count_if(events.begin(), events.end(), [&](const NetEvent& e) { return e.type == type; }));
    }
    static std::vector<NetEvent> Messages(const std::vector<NetEvent>& events, u8 channel) {
        std::vector<NetEvent> out;
        for (const NetEvent& e : events)
            if (e.type == NetEventType::Message && e.channel == channel) out.push_back(e);
        return out;
    }
};

} // namespace

AETHER_TEST(Net_Addresses) {
    const auto a = Address::Parse("192.168.1.20:7777");
    CHECK(a.has_value() && a->ip == 0xC0A80114u && a->port == 7777);
    CHECK(a->ToString() == "192.168.1.20:7777");
    CHECK(Address::Loopback(80).ToString() == "127.0.0.1:80");
    CHECK(!Address::Parse("300.1.1.1:5") && !Address::Parse("1.2.3.4") && !Address::Parse("1.2.3.4:70000") && !Address::Parse("1.2.3.4:5x"));
    CHECK(SeqGreater(1, 0) && SeqGreater(0, 65535) && !SeqGreater(65535, 0) && !SeqGreater(7, 7));
}

AETHER_TEST(Net_LoopbackNetwork) {
    LoopbackNetwork net(3);
    auto a = net.Open(1000);
    auto b = net.Open(1001);
    CHECK(net.Open(1000) == nullptr); // taken
    net.conditions.latency = 0.05;
    const std::vector<u8> hello{1, 2, 3};
    CHECK(a->Send(b->LocalAddress(), hello));
    Address from;
    std::vector<u8> got;
    CHECK(!b->Receive(from, got)); // not yet
    net.Advance(0.049);
    CHECK(!b->Receive(from, got));
    net.Advance(0.002);
    CHECK(b->Receive(from, got) && got == hello && from == a->LocalAddress());
    // Nobody there: dropped.
    a->Send(Address::Loopback(1234), hello);
    CHECK(net.InFlight() == 0);
    // Loss is by seeded chance: about the rate.
    net.conditions.latency = 0.0;
    net.conditions.loss = 0.25f;
    const u64 dropped_before = net.Dropped();
    for (int i = 0; i < 2000; ++i) a->Send(b->LocalAddress(), hello);
    const u64 lost = net.Dropped() - dropped_before;
    CHECK(lost > 400 && lost < 600);
    usize received = 0;
    while (b->Receive(from, got)) ++received;
    CHECK(received + lost == 2000);
}

AETHER_TEST(Net_LoopbackNetworkAppliesJitterAndDuplication) {
    LoopbackNetwork net(17);
    auto sender = net.Open(1200);
    auto receiver = net.Open(1201);
    net.conditions = {0.1, 0.2, 0.0f, 1.0f};
    const std::vector<u8> payload{4, 2};
    CHECK(sender->Send(receiver->LocalAddress(), payload));
    CHECK(net.InFlight() == 2);

    Address from;
    std::vector<u8> got;
    net.Advance(0.099);
    CHECK(!receiver->Receive(from, got));
    net.Advance(0.201); // the base latency plus the maximum jitter
    CHECK(receiver->Receive(from, got) && got == payload && from == sender->LocalAddress());
    CHECK(receiver->Receive(from, got) && got == payload && from == sender->LocalAddress());
    CHECK(!receiver->Receive(from, got) && net.InFlight() == 0);
}

AETHER_TEST(Net_HandshakeAndGoodbye) {
    Sim sim;
    const usize c = sim.AddClient();
    sim.Step(5);
    CHECK(Sim::Count(sim.server_events, NetEventType::Connected) == 1);
    CHECK(Sim::Count(sim.client_events[c], NetEventType::Connected) == 1);
    CHECK(sim.server->PeerCount() == 1 && sim.clients[c]->PeerCount() == 1);
    const PeerId to_server = sim.clients[c]->Peers()[0];
    const auto info = sim.clients[c]->Peer(to_server);
    CHECK(info && info->state == PeerState::Connected && info->address == Address::Loopback(7777) && info->remote_id == sim.server->Peers()[0]);
    // A message each way.
    CHECK(sim.clients[c]->Send(to_server, 0, Bytes(42)));
    CHECK(sim.server->Broadcast(0, Bytes(7)) == 1);
    sim.Step(3);
    auto got = Sim::Messages(sim.server_events, 0);
    CHECK(got.size() == 1 && Value(got[0].data) == 42);
    got = Sim::Messages(sim.client_events[c], 0);
    CHECK(got.size() == 1 && Value(got[0].data) == 7);
    // Goodbye: the client says so; both sides hear.
    sim.clients[c]->Disconnect(to_server, sim.net.Now());
    sim.Step(2);
    CHECK(sim.server->PeerCount() == 0);
    const auto bye = std::find_if(sim.server_events.begin(), sim.server_events.end(), [](const NetEvent& e) { return e.type == NetEventType::Disconnected; });
    CHECK(bye != sim.server_events.end() && bye->reason == DisconnectReason::Remote);
    CHECK(sim.client_events[c].back().type == NetEventType::Disconnected && sim.client_events[c].back().reason == DisconnectReason::Local);
    CHECK(!sim.clients[c]->Send(to_server, 0, Bytes(1)));
    CHECK(std::string(DisconnectReasonName(DisconnectReason::Timeout)) == "Timeout");
}

AETHER_TEST(Net_ReliableUnderBadConditions) {
    Sim sim({}, 11);
    sim.net.conditions = {0.05, 0.03, 0.2f, 0.05f}; // 50-80 ms, 20% loss, 5% duplicated
    const usize c = sim.AddClient();
    sim.Step(60);
    CHECK(sim.server->PeerCount() == 1);
    const PeerId peer = sim.server->Peers()[0];
    // 500 reliable messages, five a frame for 100 frames, plus an unreliable and a sequenced one each frame.
    u32 sent = 0;
    for (int frame = 0; frame < 120; ++frame) {
        for (int k = 0; k < 5 && sent < 500; ++k) CHECK(sim.server->Send(peer, 0, Bytes(sent++)));
        sim.server->Send(peer, 1, Bytes(static_cast<u32>(frame)));
        sim.server->Send(peer, 2, Bytes(static_cast<u32>(frame)));
        sim.Step();
    }
    CHECK(sent == 500);
    sim.Step(300); // let resends finish
    const auto reliable = Sim::Messages(sim.client_events[c], 0);
    CHECK(reliable.size() == 500);
    bool in_order = true;
    for (usize i = 0; i < reliable.size(); ++i) in_order = in_order && Value(reliable[i].data) == i;
    CHECK(in_order);
    // Unreliable: most, not all.
    const auto unreliable = Sim::Messages(sim.client_events[c], 1);
    CHECK(unreliable.size() > 70 && unreliable.size() < 120 + 20);
    // Sequenced: only ever newer.
    const auto sequenced = Sim::Messages(sim.client_events[c], 2);
    CHECK(sequenced.size() > 60 && sequenced.size() <= 120);
    bool rising = true;
    for (usize i = 1; i < sequenced.size(); ++i) rising = rising && Value(sequenced[i].data) > Value(sequenced[i - 1].data);
    CHECK(rising);
    const PeerId to_server = sim.clients[c]->Peers()[0];
    const ConnectionStats& stats = sim.server->Peer(peer)->connection->Stats();
    CHECK(stats.messages_resent > 0 && stats.packets_lost > 0);
    CHECK(sim.server->Peer(peer)->connection->Unacked() == 0);
    // RTT: about two one-way trips (100-160 ms), plus up to a frame each way.
    CHECK(stats.rtt > 0.09 && stats.rtt < 0.25);
    CHECK(sim.clients[c]->Peer(to_server)->connection->Stats().packets_received > 0);
}

AETHER_TEST(Net_LargeMessagesAndLimits) {
    Sim sim({}, 5);
    sim.net.conditions = {0.02, 0.0, 0.1f, 0.0f};
    const usize c = sim.AddClient();
    sim.Step(30);
    const PeerId peer = sim.server->Peers()[0];
    std::vector<u8> big(100000);
    for (usize i = 0; i < big.size(); ++i) big[i] = static_cast<u8>(i * 7 + 3);
    CHECK(sim.server->Send(peer, 0, big));
    CHECK(sim.server->Send(peer, 0, Bytes(99))); // after it, in order
    sim.Step(600);
    const auto got = Sim::Messages(sim.client_events[c], 0);
    CHECK(got.size() == 2);
    CHECK(!got.empty() && got[0].data == big);
    CHECK(got.size() == 2 && Value(got[1].data) == 99);
    // Limits: unreliable messages fit one packet; channels exist; queues are bounded.
    Connection conn;
    CHECK(!conn.Send(1, std::vector<u8>(Connection::kMaxFragment + 1)));
    CHECK(conn.Send(1, std::vector<u8>(Connection::kMaxFragment)));
    CHECK(!conn.Send(9, Bytes(1)));
    CHECK(!conn.Send(0, std::vector<u8>(ConnectionConfig{}.max_message + 1)));
    ConnectionConfig tight;
    tight.max_queued = 3;
    Connection small(tight);
    CHECK(small.Send(0, Bytes(1)) && small.Send(0, Bytes(2)) && small.Send(0, Bytes(3)) && !small.Send(0, Bytes(4)));
}

AETHER_TEST(Net_FullServerAndFailures) {
    HostConfig one;
    one.max_peers = 1;
    Sim sim(one);
    const usize a = sim.AddClient();
    sim.Step(5);
    const usize b = sim.AddClient();
    sim.Step(5);
    CHECK(sim.server->PeerCount() == 1);
    CHECK(sim.client_events[b].size() == 1 && sim.client_events[b][0].type == NetEventType::Disconnected &&
          sim.client_events[b][0].reason == DisconnectReason::Denied);
    CHECK(sim.clients[a]->PeerCount() == 1);
    // No server there: the client gives up after connect_timeout.
    Sim lonely;
    lonely.server_socket.reset(); // closes port 7777
    lonely.server.reset();
    lonely.client_sockets.push_back(lonely.net.Open());
    lonely.clients.push_back(std::make_unique<NetHost>(*lonely.client_sockets.back()));
    lonely.client_events.emplace_back();
    lonely.clients[0]->Connect(Address::Loopback(7777), lonely.net.Now());
    for (int i = 0; i < 60 * 6; ++i) {
        lonely.net.Advance(kStep);
        lonely.clients[0]->Update(lonely.net.Now());
        for (NetEvent& e : lonely.clients[0]->TakeEvents()) lonely.client_events[0].push_back(e);
    }
    CHECK(lonely.client_events[0].size() == 1 && lonely.client_events[0][0].reason == DisconnectReason::ConnectFailed);
}

AETHER_TEST(Net_KeepaliveAndTimeout) {
    Sim sim;
    const usize c = sim.AddClient();
    sim.Step(5);
    // Idle for 20 s: keepalives hold it.
    sim.Step(60 * 20);
    CHECK(sim.server->PeerCount() == 1 && sim.clients[c]->PeerCount() == 1);
    // The server goes silent (stops updating): the client times out after 5 s.
    sim.Step(60 * 4, false);
    CHECK(sim.clients[c]->PeerCount() == 1);
    sim.Step(60 * 2, false);
    CHECK(sim.clients[c]->PeerCount() == 0);
    CHECK(sim.client_events[c].back().reason == DisconnectReason::Timeout);
    // Packets from another game (protocol id) are ignored.
    HostConfig other;
    other.protocol_id = 0x12345678;
    sim.clients.push_back(std::make_unique<NetHost>(*sim.client_sockets[c], other));
    sim.client_events.emplace_back();
    sim.clients.back()->Connect(Address::Loopback(7777), sim.net.Now());
    sim.Step(30);
    CHECK(Sim::Count(sim.server_events, NetEventType::Connected) == 1); // only the first one, long ago
}

AETHER_TEST(Net_RealUdpLocalhost) {
    UdpSocket server_socket, client_socket;
    std::string error;
    if (!server_socket.Open(0, &error) || !client_socket.Open(0, &error)) {
        std::printf("    (no UDP here: %s)\n", error.c_str());
        return;
    }
    CHECK(server_socket.LocalAddress().port != 0);
    NetHost server(server_socket), client(client_socket);
    server.Listen();
    const auto start = std::chrono::steady_clock::now();
    const auto now = [&] { return std::chrono::duration<f64>(std::chrono::steady_clock::now() - start).count(); };
    client.Connect(server_socket.LocalAddress(), now());
    std::vector<NetEvent> got;
    bool sent = false;
    while (now() < 3.0) {
        server.Update(now());
        client.Update(now());
        for (NetEvent& e : server.TakeEvents()) got.push_back(e);
        client.TakeEvents();
        if (!sent && client.PeerCount() == 1) sent = client.Send(client.Peers()[0], 0, Bytes(1234));
        if (std::any_of(got.begin(), got.end(), [](const NetEvent& e) { return e.type == NetEventType::Message; })) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto msg = std::find_if(got.begin(), got.end(), [](const NetEvent& e) { return e.type == NetEventType::Message; });
    CHECK(msg != got.end() && Value(msg->data) == 1234);
}
