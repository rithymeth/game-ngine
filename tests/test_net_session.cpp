#include "aether/net/bytes.h"
#include "aether/net/session.h"
#include "test_framework.h"

#include <algorithm>

// Phase 22 step 8: sessions - joining and being refused, the lobby, ready-up,
// starting a game, kicks, leaving, advertising, and a hostile or broken peer.

using namespace aether;
using namespace aether::net;

namespace {

struct Lobby {
    LoopbackNetwork net;
    LoopbackTransport& host_link;
    NetEndpoint host_ep;
    SessionHost host;
    struct Member {
        LoopbackTransport* link = nullptr;
        std::unique_ptr<NetEndpoint> endpoint;
        std::unique_ptr<SessionClient> client;
        std::vector<SessionEvent> events;
        std::vector<NetEvent> passed; // events the session left for the game
    };
    std::vector<std::unique_ptr<Member>> members;
    std::vector<NetEvent> host_passed;
    f64 t = 0.0;

    explicit Lobby(const SessionConfig& cfg = {}, u64 seed = 1)
        : net(seed), host_link(net.CreateEndpoint()), host_ep(host_link), host(host_ep, cfg) {}

    Member& Add(const std::string& name, u32 version = 1, const std::string& password = "", bool join = true) {
        auto m = std::make_unique<Member>();
        m->link = &net.CreateEndpoint();
        m->endpoint = std::make_unique<NetEndpoint>(*m->link);
        m->client = std::make_unique<SessionClient>(*m->endpoint);
        if (join) m->client->Join(host_link.LocalAddress(), name, version, password);
        members.push_back(std::move(m));
        return *members.back();
    }

    void Step(f64 dt = 0.01) {
        t += dt;
        net.Advance(dt);
        host_ep.Update(t);
        host.Update(t);
        NetEvent e;
        while (host_ep.Poll(e)) {
            if (!host.HandleEvent(e, t)) host_passed.push_back(e);
        }
        for (auto& m : members) {
            m->endpoint->Update(t);
            while (m->endpoint->Poll(e)) {
                if (!m->client->HandleEvent(e)) m->passed.push_back(e);
            }
            SessionEvent se;
            while (m->client->Poll(se)) m->events.push_back(se);
        }
    }
    void Run(f64 seconds) {
        for (f64 s = 0; s < seconds; s += 0.01) Step();
    }
};

usize CountType(const std::vector<SessionEvent>& v, SessionEvent::Type type) {
    return static_cast<usize>(std::count_if(v.begin(), v.end(), [&](const SessionEvent& e) { return e.type == type; }));
}

const SessionEvent* Last(const std::vector<SessionEvent>& v, SessionEvent::Type type) {
    for (auto it = v.rbegin(); it != v.rend(); ++it) {
        if (it->type == type) return &*it;
    }
    return nullptr;
}

} // namespace

AETHER_TEST(NetSession_JoinShowsEveryoneTheLobby) {
    Lobby l;
    auto& a = l.Add("Alice");
    auto& b = l.Add("Bob");
    l.Run(1.0);
    AETHER_CHECK(a.client->Joined() && b.client->Joined() && a.client->LocalPlayer() != b.client->LocalPlayer());
    AETHER_CHECK(CountType(a.events, SessionEvent::Type::Joined) == 1 && Last(a.events, SessionEvent::Type::Joined)->player_id == a.client->LocalPlayer());
    AETHER_CHECK(l.host.Lobby().players.size() == 2);
    AETHER_CHECK(a.client->Lobby() == l.host.Lobby() && b.client->Lobby() == l.host.Lobby());
    AETHER_CHECK(a.client->Lobby().Find(b.client->LocalPlayer())->name == "Bob");
    AETHER_CHECK(l.host.PlayerOf(a.link->LocalAddress()) == a.client->LocalPlayer() && l.host.AddressOf(b.client->LocalPlayer()) == b.link->LocalAddress());
    AETHER_CHECK(a.client->Lobby().phase == SessionPhase::Lobby && a.client->Lobby().map == "default");
    // Connection events still reach the game.
    AETHER_CHECK(std::any_of(l.host_passed.begin(), l.host_passed.end(), [](const NetEvent& e) { return e.type == NetEventType::Connected; }));
    AETHER_CHECK(std::any_of(a.passed.begin(), a.passed.end(), [](const NetEvent& e) { return e.type == NetEventType::Connected; }));
}

AETHER_TEST(NetSession_JoinSurvivesALossyLink) {
    Lobby l({}, 9);
    l.net.conditions.latency = 0.03;
    l.net.conditions.jitter = 0.05;
    l.net.conditions.loss = 0.3;
    l.net.conditions.duplicate = 0.1;
    auto& a = l.Add("Alice");
    auto& b = l.Add("Bob");
    l.Run(8.0);
    AETHER_CHECK(a.client->Joined() && b.client->Joined());
    AETHER_CHECK(a.client->Lobby() == l.host.Lobby() && l.host.Lobby().players.size() == 2);
    AETHER_CHECK(CountType(a.events, SessionEvent::Type::Joined) == 1); // once, despite duplicates
}

AETHER_TEST(NetSession_ReadyAndStart) {
    Lobby l;
    auto& a = l.Add("Alice");
    auto& b = l.Add("Bob");
    l.Run(1.0);
    AETHER_CHECK(a.client->SetReady(true)); // joined: the call is accepted
    l.Run(0.3);
    AETHER_CHECK(l.host.Lobby().Find(a.client->LocalPlayer())->ready && !l.host.Lobby().Find(b.client->LocalPlayer())->ready);
    AETHER_CHECK(b.client->Lobby().Find(a.client->LocalPlayer())->ready); // everyone sees it

    // Not everyone is ready: the host can't start without forcing.
    AETHER_CHECK(!l.host.StartGame());
    AETHER_CHECK(l.host.Lobby().phase == SessionPhase::Lobby);
    b.client->SetReady(true);
    l.Run(0.3);
    AETHER_CHECK(l.host.SetMap("canyon"));
    l.Run(0.3);
    AETHER_CHECK(a.client->Lobby().map == "canyon");
    // Changing the map isn't a ready-reset in this lobby; the host starts it.
    AETHER_CHECK(l.host.StartGame());
    l.Run(0.5);
    AETHER_CHECK(l.host.Lobby().phase == SessionPhase::InGame && !l.host.StartGame() && !l.host.SetMap("other"));
    const SessionEvent* sa = Last(a.events, SessionEvent::Type::GameStarting);
    const SessionEvent* sb = Last(b.events, SessionEvent::Type::GameStarting);
    AETHER_CHECK(sa != nullptr && sb != nullptr);
    AETHER_CHECK(sa->map == "canyon" && sb->map == "canyon" && sa->seed == sb->seed && sa->seed == l.host.Seed());
    AETHER_CHECK(a.client->Lobby().phase == SessionPhase::InGame);

    // Un-readying after the start does nothing.
    a.client->SetReady(false);
    l.Run(0.3);
    AETHER_CHECK(l.host.Lobby().Find(a.client->LocalPlayer())->ready);
    Lobby empty;
    AETHER_CHECK(!empty.host.StartGame() && !empty.host.StartGame(true)); // an empty lobby can't start
}

AETHER_TEST(NetSession_ForceStartAndSeedsDiffer) {
    Lobby one;
    auto& a = one.Add("Alice");
    one.Run(1.0);
    AETHER_CHECK(!one.host.StartGame()); // Alice isn't ready
    AETHER_CHECK(one.host.StartGame(true));
    one.Run(0.5);
    AETHER_CHECK(Last(a.events, SessionEvent::Type::GameStarting) != nullptr);

    Lobby two;
    auto& x = two.Add("Xavier");
    auto& y = two.Add("Yvonne");
    two.Run(1.0);
    two.host.StartGame(true);
    two.Run(0.5);
    AETHER_CHECK(Last(x.events, SessionEvent::Type::GameStarting)->seed != Last(a.events, SessionEvent::Type::GameStarting)->seed);
    (void)y;
}

AETHER_TEST(NetSession_RefusesWhatItShould) {
    SessionConfig cfg;
    cfg.max_players = 2;
    cfg.password = "hunter2";
    cfg.game_version = 7;
    Lobby l(cfg);
    auto& good = l.Add("Alice", 7, "hunter2");
    auto& wrong_version = l.Add("Bob", 6, "hunter2");
    auto& wrong_password = l.Add("Carol", 7, "nope");
    auto& no_password = l.Add("Dave", 7, "");
    auto& empty_name = l.Add("", 7, "hunter2");
    auto& long_name = l.Add(std::string(kMaxPlayerName + 1, 'x'), 7, "hunter2");
    auto& control_name = l.Add(std::string("bad\nname"), 7, "hunter2");
    l.Run(2.0);
    AETHER_CHECK(good.client->Joined());
    auto reason = [](Lobby::Member& m) { return m.events.empty() ? JoinResult::Accepted : m.events.back().reason; };
    AETHER_CHECK(Last(wrong_version.events, SessionEvent::Type::Rejected) != nullptr && reason(wrong_version) == JoinResult::VersionMismatch);
    AETHER_CHECK(reason(wrong_password) == JoinResult::BadPassword && reason(no_password) == JoinResult::BadPassword);
    AETHER_CHECK(reason(empty_name) == JoinResult::BadName && reason(long_name) == JoinResult::BadName && reason(control_name) == JoinResult::BadName);
    AETHER_CHECK(l.host.Rejected() == 6 && l.host.Lobby().players.size() == 1);
    AETHER_CHECK(!wrong_version.client->Joined() && wrong_version.client->Server() == 0);

    // Room for one more, then full.
    auto& second = l.Add("Eve", 7, "hunter2");
    auto& third = l.Add("Frank", 7, "hunter2");
    l.Run(2.0);
    const bool second_in = second.client->Joined(), third_in = third.client->Joined();
    AETHER_CHECK(second_in != third_in); // exactly one got the last seat
    Lobby::Member& loser = second_in ? third : second;
    AETHER_CHECK(reason(loser) == JoinResult::Full && l.host.Lobby().players.size() == 2);
}

AETHER_TEST(NetSession_DuplicateNamesGetASuffix) {
    Lobby l;
    auto& a = l.Add("Sam");
    l.Run(0.5);
    auto& b = l.Add("Sam");
    auto& c = l.Add("Sam");
    l.Run(1.0);
    AETHER_CHECK(a.client->Joined() && b.client->Joined() && c.client->Joined());
    std::vector<std::string> names;
    for (const SessionPlayer& p : l.host.Lobby().players) names.push_back(p.name);
    std::sort(names.begin(), names.end());
    AETHER_CHECK((names == std::vector<std::string>{"Sam", "Sam (2)", "Sam (3)"}));
    // A name at the length limit still gets a suffix inside the limit.
    auto& d = l.Add(std::string(kMaxPlayerName, 'z'));
    auto& e = l.Add(std::string(kMaxPlayerName, 'z'));
    l.Run(1.0);
    AETHER_CHECK(d.client->Joined() && e.client->Joined());
    for (const SessionPlayer& p : l.host.Lobby().players) AETHER_CHECK(p.name.size() <= kMaxPlayerName);
}

AETHER_TEST(NetSession_JoiningAGameInProgress) {
    Lobby closed;
    auto& a = closed.Add("Alice");
    closed.Run(1.0);
    closed.host.StartGame(true);
    closed.Run(0.5);
    auto& late = closed.Add("Late");
    closed.Run(1.0);
    AETHER_CHECK(!late.client->Joined() && Last(late.events, SessionEvent::Type::Rejected)->reason == JoinResult::InProgress);
    (void)a;

    SessionConfig cfg;
    cfg.allow_join_in_progress = true;
    Lobby open(cfg);
    auto& first = open.Add("Alice");
    open.Run(1.0);
    open.host.StartGame(true);
    open.Run(0.5);
    auto& joiner = open.Add("Late");
    open.Run(1.5);
    AETHER_CHECK(joiner.client->Joined());
    // It is told to start right away, with the same seed.
    const SessionEvent* start = Last(joiner.events, SessionEvent::Type::GameStarting);
    AETHER_CHECK(start != nullptr && start->seed == open.host.Seed() && start->map == "default");
    AETHER_CHECK(joiner.client->Lobby().phase == SessionPhase::InGame && open.host.Lobby().Find(joiner.client->LocalPlayer())->ready);
    (void)first;
}

AETHER_TEST(NetSession_KickAndLeave) {
    Lobby l;
    auto& a = l.Add("Alice");
    auto& b = l.Add("Bob");
    auto& c = l.Add("Carol");
    l.Run(1.0);
    const u16 bob = b.client->LocalPlayer(), alice = a.client->LocalPlayer(), carol = c.client->LocalPlayer();

    AETHER_CHECK(!l.host.Kick(999));
    AETHER_CHECK(l.host.Kick(bob));
    l.Run(1.0);
    AETHER_CHECK(Last(b.events, SessionEvent::Type::Kicked) != nullptr && !b.client->Joined() && b.client->Server() == 0);
    AETHER_CHECK(l.host.Lobby().players.size() == 2 && l.host.Lobby().Find(bob) == nullptr);
    AETHER_CHECK(a.client->Lobby() == l.host.Lobby() && a.client->Lobby().Find(bob) == nullptr);
    AETHER_CHECK(l.host_ep.ConnectedPeers().size() == 2);

    // Carol leaves of her own accord.
    c.client->Leave();
    l.Run(0.5);
    AETHER_CHECK(l.host.Lobby().players.size() == 1 && a.client->Lobby().players.size() == 1);

    // Ids are never reused: a newcomer doesn't get Bob's or Carol's.
    auto& d = l.Add("Dan");
    l.Run(1.0);
    AETHER_CHECK(d.client->Joined());
    const u16 dan = d.client->LocalPlayer();
    AETHER_CHECK(dan != alice && dan != bob && dan != carol);
}

AETHER_TEST(NetSession_ConnectionsThatNeverJoinAreDropped) {
    SessionConfig cfg;
    cfg.join_timeout = 1.0;
    Lobby l(cfg);
    // A raw connection that never says who it is.
    auto& idle = l.Add("ignored", 1, "", false);
    idle.endpoint->Connect(l.host_link.LocalAddress());
    l.Run(0.5);
    AETHER_CHECK(l.host_ep.ConnectedPeers().size() == 1 && l.host.Lobby().players.empty());
    l.Run(1.5);
    AETHER_CHECK(l.host_ep.ConnectedPeers().empty());

    // It also can't do anything useful meanwhile: a ready message before joining is ignored.
    auto& sneaky = l.Add("sneaky", 1, "", false);
    sneaky.endpoint->Connect(l.host_link.LocalAddress());
    l.Run(0.3);
    const std::vector<u8> ready = {kSessionMessage, 5, 1};
    sneaky.endpoint->Send(l.host_link.LocalAddress(), Channel::ReliableOrdered, ready);
    l.Run(0.3);
    AETHER_CHECK(l.host.Lobby().players.empty());
}

AETHER_TEST(NetSession_ClientsCannotActAsTheHost) {
    Lobby l;
    auto& a = l.Add("Alice");
    auto& b = l.Add("Bob");
    l.Run(1.0);
    const LobbyState before = l.host.Lobby();
    const NetAddress host = l.host_link.LocalAddress();
    // Forged host messages from a client: start, kick, lobby overwrite, junk.
    ByteWriter start;
    start.U8(kSessionMessage), start.U8(6), start.String("evil"), start.U32(1);
    a.endpoint->Send(host, Channel::ReliableOrdered, start.Data());
    const std::vector<u8> kick = {kSessionMessage, 7};
    a.endpoint->Send(host, Channel::ReliableOrdered, kick);
    const std::vector<u8> lobby = {kSessionMessage, 4, 0, 0, 0};
    a.endpoint->Send(host, Channel::ReliableOrdered, lobby);
    const std::vector<u8> junk = {kSessionMessage, 99, 1, 2, 3};
    a.endpoint->Send(host, Channel::ReliableOrdered, junk);
    const std::vector<u8> bad_ready = {kSessionMessage, 5, 1, 1};
    a.endpoint->Send(host, Channel::ReliableOrdered, bad_ready);
    l.Run(1.0);
    AETHER_CHECK(l.host.Lobby() == before && l.host.Lobby().phase == SessionPhase::Lobby);
    AETHER_CHECK(l.host.Malformed() >= 4);
    AETHER_CHECK(b.client->Joined() && a.client->Joined());

    // A second JoinRequest from a joined player changes nothing.
    ByteWriter again;
    again.U8(kSessionMessage), again.U8(1), again.String("Mallory"), again.U32(1), again.String("");
    a.endpoint->Send(host, Channel::ReliableOrdered, again.Data());
    l.Run(0.3);
    AETHER_CHECK(l.host.Lobby() == before);
}

AETHER_TEST(NetSession_ClientIgnoresMalformedHostMessages) {
    Lobby l;
    auto& a = l.Add("Alice");
    l.Run(1.0);
    const LobbyState before = a.client->Lobby();
    const NetAddress alice = a.link->LocalAddress();
    auto send = [&](const std::vector<u8>& m) { l.host_ep.Send(alice, Channel::ReliableOrdered, m); l.Run(0.2); };
    send({kSessionMessage, 4, 9, 0, 0});                    // a phase that doesn't exist
    send({kSessionMessage, 4, 0, 0, 200});                  // more players than allowed, and truncated
    send({kSessionMessage, 2, 0, 0});                       // JoinAccept with id 0
    send({kSessionMessage, 3, 0});                          // JoinReject with a non-reason
    send({kSessionMessage, 6, 1});                          // StartGame, truncated
    send({kSessionMessage, 55});                            // unknown kind
    ByteWriter bad_name;
    bad_name.U8(kSessionMessage), bad_name.U8(4), bad_name.U8(0), bad_name.String("m"), bad_name.Varint(1);
    bad_name.U16(5), bad_name.String(""), bad_name.Bool(false), bad_name.Bool(false); // an empty player name
    send(bad_name.Data());
    AETHER_CHECK(a.client->Lobby() == before && a.client->Joined());
    AETHER_CHECK(a.client->Malformed() == 7);
    AETHER_CHECK(CountType(a.events, SessionEvent::Type::GameStarting) == 0);
}

AETHER_TEST(NetSession_HostDisconnectShowsUpAsAnEvent) {
    Lobby l;
    auto& a = l.Add("Alice");
    l.Run(1.0);
    l.host_ep.DisconnectAll();
    l.Run(0.5);
    AETHER_CHECK(CountType(a.events, SessionEvent::Type::Disconnected) == 1 && !a.client->Joined());
    // A client can rejoin afterwards.
    AETHER_CHECK(a.client->Join(l.host_link.LocalAddress(), "Alice", 1));
    l.Run(1.0);
    AETHER_CHECK(a.client->Joined());
    AETHER_CHECK(!a.client->Join(l.host_link.LocalAddress(), "Again", 1)); // already in
}

AETHER_TEST(NetSession_ListenServerHostPlaysToo) {
    SessionConfig cfg;
    cfg.host_player_name = "Hosty";
    cfg.max_players = 2;
    Lobby l(cfg);
    AETHER_CHECK(l.host.Lobby().players.size() == 1 && l.host.Lobby().players[0].is_host && l.host.Lobby().players[0].ready);
    auto& a = l.Add("Alice");
    auto& b = l.Add("Bob");
    l.Run(1.5);
    AETHER_CHECK(a.client->Joined() != b.client->Joined()); // the host holds one of the two seats
    AETHER_CHECK(l.host.Lobby().players.size() == 2);
    AETHER_CHECK(l.host.AddressOf(l.host.Lobby().players[0].id) == 0);
    AETHER_CHECK(l.host.SetHostReady(false));
    l.Run(0.3);
    AETHER_CHECK(!l.host.StartGame()); // the host itself isn't ready
    l.host.SetHostReady(true);
    Lobby::Member& joined = a.client->Joined() ? a : b;
    joined.client->SetReady(true);
    l.Run(0.3);
    AETHER_CHECK(l.host.StartGame());
}

AETHER_TEST(NetSession_AdvertisementTracksTheLobby) {
    SessionConfig cfg;
    cfg.name = "Friday night";
    cfg.map = "dunes";
    cfg.game_version = 4;
    cfg.max_players = 6;
    Lobby l(cfg);
    SessionInfo info = l.host.Advertisement(7777);
    AETHER_CHECK(info.name == "Friday night" && info.map == "dunes" && info.game_version == 4);
    AETHER_CHECK(info.game_port == 7777 && info.players == 0 && info.max_players == 6);

    // A browser on the same network sees the player count change as people join.
    LoopbackTransport& host_discovery = l.net.CreateEndpoint();
    LoopbackTransport& browser_link = l.net.CreateEndpoint();
    LanHost advert(host_discovery, "test-game");
    LanBrowser browser(browser_link, "test-game");
    auto refresh = [&] {
        advert.SetSession(l.host.Advertisement(7777));
        browser.Search(l.t);
        for (int i = 0; i < 20; ++i) {
            l.Step();
            advert.Update(l.t);
            browser.Update(l.t);
        }
    };
    refresh();
    AETHER_CHECK(browser.Sessions().size() == 1 && browser.Sessions()[0].info.players == 0);
    l.Add("Alice", 4);
    l.Add("Bob", 4);
    l.Run(1.0);
    l.host.SetMap("canyon");
    refresh();
    AETHER_CHECK(browser.Sessions().size() == 1);
    AETHER_CHECK(browser.Sessions()[0].info.players == 2);
    AETHER_CHECK(browser.Sessions()[0].info.map == "canyon");
    AETHER_CHECK(l.host.Lobby().players.size() == 2 && l.host.Lobby().map == "canyon");
}
