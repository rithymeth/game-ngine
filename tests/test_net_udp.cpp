#include "aether/net/endpoint.h"
#include "aether/net/discovery.h"
#include "aether/net/udp.h"
#include "test_framework.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <thread>

// Phase 22 step 6: the UDP socket transport, on this machine's loopback
// interface - datagrams, address handles, limits - and a full connection and
// LAN-discovery exchange over real sockets.

using namespace aether;
using namespace aether::net;

namespace {

using Clock = std::chrono::steady_clock;

f64 Seconds(Clock::time_point from) { return std::chrono::duration<f64>(Clock::now() - from).count(); }

// Polls `step` (about every millisecond) until `done` or the timeout.
bool PumpUntil(f64 timeout, const std::function<void()>& step, const std::function<bool()>& done) {
    const auto start = Clock::now();
    while (Seconds(start) < timeout) {
        step();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool ReceiveWithin(Transport& t, Datagram& out, f64 timeout = 1.0) {
    return PumpUntil(timeout, [] {}, [&] { return t.Receive(out); });
}

} // namespace

AETHER_TEST(NetUdp_DatagramsFlowBothWays) {
    std::string error;
    auto a = UdpTransport::Create({}, &error);
    auto b = UdpTransport::Create({}, &error);
    AETHER_CHECK(a != nullptr && b != nullptr);
    if (a == nullptr || b == nullptr) return;
    AETHER_CHECK(a->Port() != 0 && b->Port() != 0 && a->Port() != b->Port());

    const NetAddress to_b = a->AddressFor("127.0.0.1", b->Port());
    AETHER_CHECK(to_b != 0);
    const std::vector<u8> hello = {1, 2, 3, 4, 5};
    AETHER_CHECK(a->Send(to_b, hello));
    Datagram d;
    AETHER_CHECK(ReceiveWithin(*b, d) && d.data == hello);

    // The sender's handle on b names a's address and port, and replying through it works.
    std::string host;
    u16 port = 0;
    AETHER_CHECK(b->Describe(d.from, host, port) && host == "127.0.0.1" && port == a->Port());
    const std::vector<u8> reply = {9, 9};
    AETHER_CHECK(b->Send(d.from, reply));
    Datagram back;
    AETHER_CHECK(ReceiveWithin(*a, back) && back.data == reply && back.from == to_b); // the same handle a already had

    // The same peer keeps one handle however many datagrams arrive.
    a->Send(to_b, hello);
    a->Send(to_b, hello);
    Datagram d2, d3;
    AETHER_CHECK(ReceiveWithin(*b, d2) && ReceiveWithin(*b, d3) && d2.from == d.from && d3.from == d.from);
    AETHER_CHECK(!b->Receive(d)); // nothing more: non-blocking
}

AETHER_TEST(NetUdp_RejectsWhatItCannotSend) {
    auto a = UdpTransport::Create();
    AETHER_CHECK(a != nullptr);
    if (a == nullptr) return;
    const NetAddress to = a->AddressFor("127.0.0.1", a->Port()); // itself
    const std::vector<u8> huge(a->MaxDatagramSize() + 1, 0);
    AETHER_CHECK(!a->Send(to, huge));
    const std::vector<u8> max(a->MaxDatagramSize(), 7);
    AETHER_CHECK(a->Send(to, max));
    Datagram d;
    AETHER_CHECK(ReceiveWithin(*a, d) && d.data == max);
    AETHER_CHECK(!a->Send(0, max));
    AETHER_CHECK(!a->Send(4242, max)); // a handle it never made
    AETHER_CHECK(a->LocalAddress() == UdpTransport::kSelf);
    AETHER_CHECK(!a->Broadcast(0, max) && !a->Broadcast(9, huge));
}

AETHER_TEST(NetUdp_AddressLookup) {
    auto a = UdpTransport::Create();
    AETHER_CHECK(a != nullptr);
    if (a == nullptr) return;
    AETHER_CHECK(a->AddressFor("127.0.0.1", 0) == 0);        // no port
    AETHER_CHECK(a->AddressFor("999.1.1.1", 5000) == 0);     // not an address, not a name
    AETHER_CHECK(a->AddressFor("", 5000) == 0);
    AETHER_CHECK(a->AddressFor("localhost", 5000) != 0);
    const NetAddress x = a->AddressFor("10.1.2.3", 5000);
    AETHER_CHECK(x != 0 && a->AddressFor("10.1.2.3", 5000) == x);   // stable
    AETHER_CHECK(a->AddressFor("10.1.2.3", 5001) != x);             // another port, another handle
    const NetAddress sibling = a->AddressWithPort(x, 6000);
    std::string host;
    u16 port = 0;
    AETHER_CHECK(sibling != 0 && a->Describe(sibling, host, port) && host == "10.1.2.3" && port == 6000);
    AETHER_CHECK(a->AddressWithPort(x, 0) == 0 && a->AddressWithPort(9999, 6000) == 0);
    AETHER_CHECK(!a->Describe(9999, host, port));
}

AETHER_TEST(NetUdp_BindingATakenPortFails) {
    auto a = UdpTransport::Create();
    AETHER_CHECK(a != nullptr);
    if (a == nullptr) return;
    UdpConfig cfg;
    cfg.port = a->Port();
    std::string error;
    auto clash = UdpTransport::Create(cfg, &error);
    AETHER_CHECK(clash == nullptr && !error.empty());
}

AETHER_TEST(NetUdp_AddressTableIsBounded) {
    UdpConfig cfg;
    cfg.max_addresses = 8;
    auto a = UdpTransport::Create(cfg);
    AETHER_CHECK(a != nullptr);
    if (a == nullptr) return;
    std::vector<NetAddress> handles;
    for (u16 i = 0; i < 100; ++i) handles.push_back(a->AddressFor("10.0.0.1", static_cast<u16>(2000 + i)));
    AETHER_CHECK(a->KnownAddresses() == 8);
    std::string host;
    u16 port = 0;
    AETHER_CHECK(!a->Describe(handles.front(), host, port));            // the quietest were dropped
    AETHER_CHECK(a->Describe(handles.back(), host, port) && port == 2099);

    // A peer in use is not the one dropped.
    const NetAddress keep = a->AddressFor("10.0.0.2", 1);
    const std::vector<u8> m = {1};
    for (u16 i = 0; i < 50; ++i) {
        a->Send(keep, m); // keeps it recent (to nowhere; the send itself may or may not succeed)
        a->AddressFor("10.0.0.3", static_cast<u16>(3000 + i));
    }
    AETHER_CHECK(a->Describe(keep, host, port) && port == 1);
}

AETHER_TEST(NetUdp_ConnectionsAndMessagesOverRealSockets) {
    auto server_socket = UdpTransport::Create();
    auto client_socket = UdpTransport::Create();
    AETHER_CHECK(server_socket != nullptr && client_socket != nullptr);
    if (server_socket == nullptr || client_socket == nullptr) return;
    NetEndpoint server(*server_socket), client(*client_socket);
    server.Listen();
    const NetAddress server_handle = client_socket->AddressFor("127.0.0.1", server_socket->Port());
    AETHER_CHECK(client.Connect(server_handle));

    const auto start = Clock::now();
    std::vector<NetEvent> server_events, client_events;
    auto step = [&] {
        const f64 now = Seconds(start);
        server.Update(now);
        client.Update(now);
        NetEvent e;
        while (server.Poll(e)) server_events.push_back(std::move(e));
        while (client.Poll(e)) client_events.push_back(std::move(e));
    };
    AETHER_CHECK(PumpUntil(3.0, step, [&] { return client.IsConnected(server_handle) && !server.ConnectedPeers().empty(); }));
    const NetAddress client_on_server = server.ConnectedPeers().empty() ? 0 : server.ConnectedPeers()[0];

    // Reliable messages both ways, in order, including a big one.
    for (u8 i = 0; i < 20; ++i) {
        const std::vector<u8> m(i == 10 ? 900 : 4, i);
        AETHER_CHECK(client.Send(server_handle, Channel::ReliableOrdered, m));
        AETHER_CHECK(server.Send(client_on_server, Channel::ReliableOrdered, m));
    }
    auto count = [](const std::vector<NetEvent>& v) {
        return std::count_if(v.begin(), v.end(), [](const NetEvent& e) { return e.type == NetEventType::Message; });
    };
    AETHER_CHECK(PumpUntil(3.0, step, [&] { return count(server_events) == 20 && count(client_events) == 20; }));
    u8 expect = 0;
    bool ordered = true;
    for (const NetEvent& e : server_events) {
        if (e.type != NetEventType::Message) continue;
        ordered = ordered && e.data[0] == expect && e.data.size() == (expect == 10 ? 900u : 4u);
        ++expect;
    }
    AETHER_CHECK(ordered);

    // A clean disconnect reaches the other side.
    client.Disconnect(server_handle);
    AETHER_CHECK(PumpUntil(2.0, step, [&] { return server.ConnectedPeers().empty(); }));
}

AETHER_TEST(NetUdp_DiscoveryOverRealSockets) {
    auto host_socket = UdpTransport::Create();
    auto browser_socket = UdpTransport::Create();
    AETHER_CHECK(host_socket != nullptr && browser_socket != nullptr);
    if (host_socket == nullptr || browser_socket == nullptr) return;
    LanHost host(*host_socket, "udp-test");
    SessionInfo info;
    info.name = "Real sockets";
    info.map = "dunes";
    info.game_port = 7777;
    info.players = 2;
    info.max_players = 4;
    host.SetSession(info);
    LanBrowser browser(*browser_socket, "udp-test");

    const NetAddress host_handle = browser_socket->AddressFor("127.0.0.1", host_socket->Port());
    AETHER_CHECK(browser.SearchHost(host_handle, 0.0));
    const auto start = Clock::now();
    AETHER_CHECK(PumpUntil(2.0, [&] { host.Update(Seconds(start)); browser.Update(Seconds(start)); }, [&] { return !browser.Sessions().empty(); }));
    AETHER_CHECK(browser.Sessions().size() == 1 && browser.Sessions()[0].info.name == "Real sockets");
    AETHER_CHECK(browser.Sessions()[0].info.game_port == 7777 && browser.Sessions()[0].host == host_handle);

    // From the reply, the browser can name the host's game socket: same machine, the game's port.
    const NetAddress game = browser_socket->AddressWithPort(browser.Sessions()[0].host, browser.Sessions()[0].info.game_port);
    std::string ip;
    u16 port = 0;
    AETHER_CHECK(game != 0 && browser_socket->Describe(game, ip, port) && ip == "127.0.0.1" && port == 7777);

    // Broadcasting needs the option; with it the call is accepted (whether anyone answers depends on the network).
    UdpConfig cfg;
    cfg.allow_broadcast = true;
    auto lan = UdpTransport::Create(cfg);
    AETHER_CHECK(lan != nullptr);
    if (lan != nullptr) {
        LanBrowser b2(*lan, "udp-test");
        b2.Search(0.0, kDefaultDiscoveryPort); // must not crash or block; the result depends on the machine's network
    }
}
