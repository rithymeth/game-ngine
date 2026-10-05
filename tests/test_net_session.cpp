#include "aether/net/session.h"
#include "test_framework.h"

#include <algorithm>
#include <cstring>

// Phase 22 step 5: sessions - simulated hosts and broadcast, session info,
// LAN discovery, hosting and joining (passwords, builds, full sessions,
// silent peers, kicks, unique names), and the LAN lobby service.

using namespace aether;
using namespace aether::net;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

constexpr f64 kStep = 1.0 / 60.0;
constexpr u32 kHostA = Address::IPv4(10, 0, 0, 1), kHostB = Address::IPv4(10, 0, 0, 2), kPlayer = Address::IPv4(10, 0, 0, 9);

std::vector<u8> Bytes(const char* s) { return std::vector<u8>(s, s + std::strlen(s)); }

SessionInfo Info(const std::string& name, u16 port, u32 build = 7) {
    SessionInfo i;
    i.name = name, i.map = "Docks", i.mode = "CTF", i.port = port, i.max_players = 8, i.build = build;
    i.properties = {{"region", "eu"}, {"ranked", "no"}};
    return i;
}

// A hosted session and joining clients on the simulated network.
struct Lan {
    LoopbackNetwork net;
    std::unique_ptr<DatagramSocket> game_socket;
    std::unique_ptr<NetHost> host;
    std::unique_ptr<SessionHost> session;
    struct Joiner {
        std::unique_ptr<DatagramSocket> socket;
        std::unique_ptr<NetHost> host;
        std::unique_ptr<SessionClient> session;
    };
    std::vector<std::unique_ptr<Joiner>> joiners;
    std::vector<SessionEvent> events;

    explicit Lan(SessionInfo info, std::string password = {}, HostConfig config = {}) {
        game_socket = net.OpenAt(kHostA, 7777);
        host = std::make_unique<NetHost>(*game_socket, config);
        host->Listen();
        session = std::make_unique<SessionHost>(*host, std::move(info), std::move(password));
    }
    SessionClient& Join(const std::string& name, const std::string& password, u32 build = 7) {
        auto j = std::make_unique<Joiner>();
        j->socket = net.OpenAt(kPlayer);
        j->host = std::make_unique<NetHost>(*j->socket);
        j->session = std::make_unique<SessionClient>(*j->host);
        j->session->Join(Address{kHostA, 7777}, name, password, build, net.Now());
        joiners.push_back(std::move(j));
        return *joiners.back()->session;
    }
    void Step(int frames = 1) {
        for (int i = 0; i < frames; ++i) {
            net.Advance(kStep);
            host->Update(net.Now());
            for (const NetEvent& e : host->TakeEvents()) session->HandleEvent(e, net.Now());
            session->Update(net.Now());
            for (SessionEvent& e : session->TakeEvents()) events.push_back(std::move(e));
            for (auto& j : joiners) {
                j->host->Update(net.Now());
                for (const NetEvent& e : j->host->TakeEvents()) j->session->HandleEvent(e);
            }
        }
    }
    usize Count(SessionEventType type) const {
        return static_cast<usize>(std::count_if(events.begin(), events.end(), [&](const SessionEvent& e) { return e.type == type; }));
    }
};

} // namespace

AETHER_TEST(Session_SimulatedHostsAndBroadcast) {
    LoopbackNetwork net;
    auto a = net.OpenAt(kHostA, 7778), b = net.OpenAt(kHostB, 7778), other_port = net.OpenAt(kHostB, 9000);
    auto me = net.OpenAt(kPlayer);
    CHECK(a && b && other_port && me && net.OpenAt(kHostA, 7778) == nullptr);
    CHECK(net.OpenAt(kHostB, 7777) != nullptr); // same port, another host: fine
    CHECK(a->LocalAddress() == (Address{kHostA, 7778}) && Address::Parse("10.0.0.1:7778") == a->LocalAddress());
    me->Send(Address::Broadcast(7778), Bytes("hello"));
    me->Send(Address{kHostB, 7778}, Bytes("just b"));
    a->Send(Address::Broadcast(7778), Bytes("from a")); // to b, not back to itself
    net.Advance(0.01);
    Address from;
    std::vector<u8> data;
    CHECK(a->Receive(from, data) && data == Bytes("hello") && from == me->LocalAddress() && !a->Receive(from, data));
    CHECK(b->Receive(from, data) && data == Bytes("hello"));
    CHECK(b->Receive(from, data) && data == Bytes("just b"));
    CHECK(b->Receive(from, data) && data == Bytes("from a") && from == a->LocalAddress());
    CHECK(!other_port->Receive(from, data));
    // Real UDP sockets can broadcast too (the option is set; delivery depends on the machine).
    UdpSocket udp;
    CHECK(udp.Open(0));
}

AETHER_TEST(Session_InfoRoundTrips) {
    SessionInfo info = Info("Ada's game", 7777);
    info.players = 3, info.password = true;
    const std::vector<u8> bytes = EncodeSessionInfo(info);
    SessionInfo back;
    CHECK(DecodeSessionInfo(bytes, back));
    CHECK(back.name == "Ada's game" && back.map == "Docks" && back.mode == "CTF" && back.port == 7777);
    CHECK(back.players == 3 && back.max_players == 8 && back.build == 7 && back.password);
    CHECK(back.Property("region") && *back.Property("region") == "eu" && !back.Property("nope"));
    CHECK(!DecodeSessionInfo(std::span<const u8>(bytes.data(), bytes.size() - 1), back)); // cut short
    info.name = std::string(400, 'x'); // long strings are cut to 255
    CHECK(DecodeSessionInfo(EncodeSessionInfo(info), back) && back.name.size() == 255);
}

AETHER_TEST(Session_LanDiscovery) {
    LoopbackNetwork net;
    net.conditions.latency = 0.002;
    auto beacon_socket_a = net.OpenAt(kHostA, kDefaultDiscoveryPort);
    auto beacon_socket_b = net.OpenAt(kHostB, kDefaultDiscoveryPort);
    auto browser_socket = net.OpenAt(kPlayer);
    auto beacon_a = std::make_unique<LanBeacon>(*beacon_socket_a);
    LanBeacon beacon_b(*beacon_socket_b);
    beacon_a->info = Info("Alpha", 7777);
    beacon_b.info = Info("Bravo", 7777, 8); // another build
    LanBrowser browser(*browser_socket, 7);
    auto run = [&](int frames) {
        for (int i = 0; i < frames; ++i) {
            net.Advance(kStep);
            if (beacon_a) beacon_a->Update();
            beacon_b.Update();
            browser.Update(net.Now());
        }
    };
    browser.Search(net.Now());
    run(5);
    std::vector<LanSession> found = browser.Results();
    CHECK(found.size() == 2);
    auto alpha = std::find_if(found.begin(), found.end(), [](const LanSession& s) { return s.info.name == "Alpha"; });
    auto bravo = std::find_if(found.begin(), found.end(), [](const LanSession& s) { return s.info.name == "Bravo"; });
    CHECK(alpha != found.end() && bravo != found.end());
    CHECK(alpha->address == (Address{kHostA, 7777}) && alpha->compatible && !bravo->compatible);
    CHECK(alpha->ping > 0.0 && alpha->ping < 0.1 && alpha->info.map == "Docks");
    CHECK(found[0].ping <= found[1].ping);
    // A changed player count shows on the next search.
    beacon_a->info.players = 5;
    browser.Search(net.Now());
    run(5);
    found = browser.Results();
    CHECK(std::any_of(found.begin(), found.end(), [](const LanSession& s) { return s.info.name == "Alpha" && s.info.players == 5; }));
    CHECK(browser.Results().size() == 2);
    // Another protocol's query gets no answer; unasked answers are ignored.
    auto stranger_socket = net.OpenAt(kPlayer, 5000);
    LanBrowser stranger(*stranger_socket, 7, kDefaultDiscoveryPort, 0x12345678);
    const u32 before = beacon_b.Answered();
    stranger.Search(net.Now());
    run(5);
    CHECK(beacon_b.Answered() == before && stranger.Results().empty());
    // A host that goes away expires; auto-search keeps the rest fresh.
    beacon_a.reset();
    beacon_socket_a.reset();
    browser.auto_search = 1.0;
    browser.expiry = 2.0;
    run(240);
    found = browser.Results();
    CHECK(found.size() == 1 && found[0].info.name == "Bravo");
    beacon_b.enabled = false;
    run(240);
    CHECK(browser.Results().empty());
}

AETHER_TEST(Session_HostingAndJoining) {
    SessionInfo info = Info("Ada's game", 7777);
    info.max_players = 2;
    Lan lan(info, "hunter2");
    CHECK(lan.session->Info().password && lan.session->Info().players == 0);
    SessionClient& ada = lan.Join("Ada", "hunter2");
    SessionClient& wrong = lan.Join("Mallory", "guess");
    SessionClient& old = lan.Join("Bob", "hunter2", 6);
    lan.Step(30);
    CHECK(ada.GetState() == SessionClient::State::Joined && ada.PlayerName() == "Ada");
    CHECK(ada.Info().name == "Ada's game" && ada.Info().players == 1 && ada.Info().password);
    CHECK(wrong.GetState() == SessionClient::State::Failed && wrong.Result() == JoinResult::WrongPassword);
    CHECK(old.GetState() == SessionClient::State::Failed && old.Result() == JoinResult::BuildMismatch);
    CHECK(lan.session->Players().size() == 1 && lan.session->Info().players == 1);
    CHECK(lan.Count(SessionEventType::PlayerJoined) == 1 && lan.Count(SessionEventType::JoinRefused) == 2);
    CHECK(lan.host->PeerCount() == 1); // the refused were dropped once told

    // The same name twice, then full.
    SessionClient& ada2 = lan.Join("Ada", "hunter2");
    lan.Step(30);
    CHECK(ada2.GetState() == SessionClient::State::Joined && ada2.PlayerName() == "Ada (2)");
    SessionClient& late = lan.Join("Cy", "hunter2");
    lan.Step(30);
    CHECK(late.GetState() == SessionClient::State::Failed && late.Result() == JoinResult::Full);

    // A peer that connects and never asks is dropped.
    auto raw_socket = lan.net.OpenAt(kPlayer);
    NetHost raw(*raw_socket);
    raw.Connect(Address{kHostA, 7777}, lan.net.Now());
    for (int i = 0; i < 260; ++i) {
        lan.Step();
        raw.Update(lan.net.Now());
        raw.TakeEvents();
    }
    CHECK(raw.PeerCount() == 0 && lan.host->PeerCount() == 2);
    CHECK(std::any_of(lan.events.begin(), lan.events.end(),
                      [](const SessionEvent& e) { return e.type == SessionEventType::JoinRefused && e.reason == JoinResult::NoRequest; }));

    // Leaving and kicking.
    ada.Leave(lan.net.Now());
    lan.Step(30);
    CHECK(ada.GetState() == SessionClient::State::Idle && lan.session->Players().size() == 1);
    CHECK(lan.Count(SessionEventType::PlayerLeft) == 1);
    const PeerId ada2_peer = lan.session->Players()[0].peer;
    CHECK(lan.session->Player(ada2_peer)->name == "Ada (2)");
    lan.session->Kick(ada2_peer, lan.net.Now());
    lan.Step(30);
    CHECK(ada2.GetState() == SessionClient::State::Failed && ada2.Result() == JoinResult::Kicked);
    CHECK(lan.session->Players().empty() && lan.host->PeerCount() == 0 && lan.session->Info().players == 0);
    // Nobody there.
    Lan empty(Info("x", 7777));
    empty.game_socket.reset();
    SessionClient& lost = empty.Join("Dee", "");
    empty.host.reset();
    empty.session.reset();
    for (int i = 0; i < 400; ++i) {
        empty.net.Advance(kStep);
        for (auto& j : empty.joiners) {
            j->host->Update(empty.net.Now());
            for (const NetEvent& e : j->host->TakeEvents()) j->session->HandleEvent(e);
        }
    }
    CHECK(lost.GetState() == SessionClient::State::Failed && lost.Result() == JoinResult::ConnectFailed);
}

AETHER_TEST(Session_LanLobbyService) {
    LoopbackNetwork net;
    auto host_discovery = net.OpenAt(kHostA, kDefaultDiscoveryPort);
    auto host_search = net.OpenAt(kHostA);
    auto player_search = net.OpenAt(kPlayer);
    LanLobbyService hosting(host_discovery.get(), *host_search, 7);
    LanLobbyService finding(nullptr, *player_search, 7);
    LobbyService& lobby = finding;
    CHECK(std::string(lobby.Name()) == "LAN");
    CHECK(!finding.Advertise(Info("nope", 1))); // no discovery socket
    CHECK(hosting.Advertise(Info("Lobby", 7777)) && hosting.Advertising());
    auto run = [&](int frames) {
        for (int i = 0; i < frames; ++i) {
            net.Advance(kStep);
            hosting.Update(net.Now());
            finding.Update(net.Now());
        }
    };
    lobby.Search(net.Now());
    run(5);
    std::vector<LobbyEntry> found = lobby.Results();
    CHECK(found.size() == 1 && found[0].info.name == "Lobby" && found[0].id == "10.0.0.1:7777" && found[0].compatible);
    hosting.StopAdvertising();
    lobby.Search(net.Now());
    run(400); // expired, and no answer to the new search
    CHECK(lobby.Results().empty());
}
