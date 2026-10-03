#include "aether/net/discovery.h"
#include "aether/net/bytes.h"
#include "test_framework.h"

#include <algorithm>

// Phase 22 step 6: LAN discovery - hosts answering queries, a browser listing
// sessions, filtering by game, malformed traffic and the reply rate limit.

using namespace aether;
using namespace aether::net;

namespace {

struct Lan {
    LoopbackNetwork net;
    f64 t = 0.0;
    explicit Lan(f64 latency = 0.01) {
        net.conditions.latency = latency;
    }
    void Run(int steps, std::vector<LanHost*> hosts, LanBrowser* browser) {
        for (int i = 0; i < steps; ++i) {
            t += 0.005;
            net.Advance(0.005);
            for (LanHost* h : hosts) h->Update(t);
            if (browser != nullptr) browser->Update(t);
        }
    }
};

SessionInfo Info(const char* name, u8 players = 1) {
    SessionInfo s;
    s.name = name;
    s.map = "arena";
    s.game_version = 3;
    s.game_port = 7000;
    s.players = players;
    s.max_players = 8;
    return s;
}

} // namespace

AETHER_TEST(NetDiscovery_BrowserListsAHost) {
    Lan lan;
    LoopbackTransport& host_link = lan.net.CreateEndpoint();
    LoopbackTransport& browser_link = lan.net.CreateEndpoint();
    LanHost host(host_link, "aether-test");
    LanBrowser browser(browser_link, "aether-test");

    // Not advertising yet: no answer.
    AETHER_CHECK(browser.Search(lan.t));
    lan.Run(20, {&host}, &browser);
    AETHER_CHECK(browser.Sessions().empty() && host.Answered() == 0);

    host.SetSession(Info("Friday night"));
    AETHER_CHECK(browser.Search(lan.t));
    lan.Run(20, {&host}, &browser);
    AETHER_CHECK(browser.Sessions().size() == 1);
    const DiscoveredSession& s = browser.Sessions()[0];
    AETHER_CHECK(s.host == host_link.LocalAddress());
    AETHER_CHECK(s.info.name == "Friday night" && s.info.map == "arena" && s.info.game_version == 3);
    AETHER_CHECK(s.info.game_port == 7000 && s.info.players == 1 && s.info.max_players == 8);
    AETHER_CHECK(s.round_trip > 0.015 && s.round_trip < 0.05); // two 10 ms hops
    AETHER_CHECK(host.Answered() == 1);

    // Stopping the advert silences the host.
    host.Stop();
    browser.Prune(lan.t + 100.0, 1.0);
    AETHER_CHECK(browser.Sessions().empty());
    browser.Search(lan.t);
    lan.Run(20, {&host}, &browser);
    AETHER_CHECK(browser.Sessions().empty());
}

AETHER_TEST(NetDiscovery_SeveralHostsAndRefresh) {
    Lan lan;
    LoopbackTransport& l1 = lan.net.CreateEndpoint();
    LoopbackTransport& l2 = lan.net.CreateEndpoint();
    LoopbackTransport& lb = lan.net.CreateEndpoint();
    LanHost h1(l1, "g"), h2(l2, "g");
    LanBrowser browser(lb, "g");
    h1.SetSession(Info("one", 2));
    h2.SetSession(Info("two", 5));
    browser.Search(lan.t);
    lan.Run(20, {&h1, &h2}, &browser);
    AETHER_CHECK(browser.Sessions().size() == 2);

    // A refresh updates the entry; it doesn't add a duplicate.
    h1.SetSession(Info("one", 4));
    browser.Search(lan.t);
    lan.Run(20, {&h1, &h2}, &browser);
    AETHER_CHECK(browser.Sessions().size() == 2);
    const auto one = std::find_if(browser.Sessions().begin(), browser.Sessions().end(), [](const DiscoveredSession& s) { return s.info.name == "one"; });
    AETHER_CHECK(one != browser.Sessions().end() && one->info.players == 4);

    // A host that went quiet ages out; one that kept answering stays.
    h2.Stop();
    const f64 before = lan.t;
    for (int i = 0; i < 3; ++i) {
        browser.Search(lan.t);
        lan.Run(20, {&h1, &h2}, &browser);
    }
    browser.Prune(lan.t, lan.t - before - 0.01);
    AETHER_CHECK(browser.Sessions().size() == 1 && browser.Sessions()[0].info.name == "one");
}

AETHER_TEST(NetDiscovery_OtherGamesAreIgnored) {
    Lan lan;
    LoopbackTransport& hl = lan.net.CreateEndpoint();
    LoopbackTransport& bl = lan.net.CreateEndpoint();
    LanHost host(hl, "game-a");
    host.SetSession(Info("a"));
    LanBrowser other(bl, "game-b");
    other.Search(lan.t);
    lan.Run(20, {&host}, &other);
    AETHER_CHECK(other.Sessions().empty() && host.Answered() == 0);
}

AETHER_TEST(NetDiscovery_UnicastQueryReachesOneHost) {
    Lan lan;
    LoopbackTransport& hl = lan.net.CreateEndpoint();
    LoopbackTransport& other_link = lan.net.CreateEndpoint();
    LoopbackTransport& bl = lan.net.CreateEndpoint();
    LanHost host(hl, "g"), other(other_link, "g");
    host.SetSession(Info("target"));
    other.SetSession(Info("bystander"));
    LanBrowser browser(bl, "g");
    AETHER_CHECK(browser.SearchHost(hl.LocalAddress(), lan.t));
    lan.Run(20, {&host, &other}, &browser);
    AETHER_CHECK(browser.Sessions().size() == 1 && browser.Sessions()[0].info.name == "target" && other.Answered() == 0);
    AETHER_CHECK(!browser.SearchHost(99, lan.t)); // no such host
}

AETHER_TEST(NetDiscovery_MalformedTrafficIsIgnored) {
    Lan lan(0.0);
    LoopbackTransport& hl = lan.net.CreateEndpoint();
    LoopbackTransport& bl = lan.net.CreateEndpoint();
    LoopbackTransport& rogue = lan.net.CreateEndpoint();
    LanHost host(hl, "g");
    host.SetSession(Info("real"));
    LanBrowser browser(bl, "g");

    auto query = [&](u32 magic, const std::string& game, bool extra_byte) {
        ByteWriter w;
        w.U8(kDiscoveryQuery);
        w.U32(magic);
        w.String(game);
        w.U32(1);
        if (extra_byte) w.U8(0);
        rogue.Send(hl.LocalAddress(), w.Data());
    };
    query(0xDEADBEEF, "g", false);                       // wrong magic
    query(0x53444541, std::string(500, 'x'), false);     // a name far too long
    query(0x53444541, "g", true);                        // trailing bytes
    const std::vector<u8> truncated = {kDiscoveryQuery, 1, 2};
    rogue.Send(hl.LocalAddress(), truncated);
    const std::vector<u8> unrelated = {0x42, 0, 0, 0};
    rogue.Send(hl.LocalAddress(), unrelated);
    host.Update(lan.t);
    AETHER_CHECK(host.Answered() == 0 && host.Ignored() == 4); // the unrelated datagram isn't counted: it isn't discovery traffic

    // Bad responses to the browser: each is counted and none is listed.
    auto response = [&](u32 magic, u16 port, u8 players, u8 max_players, bool extra) {
        ByteWriter w;
        w.U8(kDiscoveryResponse);
        w.U32(magic);
        w.U32(1);
        w.String("g");
        w.String("name");
        w.String("map");
        w.U32(1);
        w.U16(port);
        w.U8(players);
        w.U8(max_players);
        if (extra) w.U8(9);
        rogue.Send(bl.LocalAddress(), w.Data());
    };
    response(0xDEADBEEF, 7000, 1, 8, false);
    response(0x53444541, 0, 1, 8, false);     // no game port
    response(0x53444541, 7000, 9, 8, false);  // more players than slots
    response(0x53444541, 7000, 1, 8, true);   // trailing bytes
    const std::vector<u8> cut = {kDiscoveryResponse, 1, 2, 3};
    rogue.Send(bl.LocalAddress(), cut);
    browser.Update(lan.t);
    AETHER_CHECK(browser.Sessions().empty() && browser.Malformed() == 5);

    // The real host still works afterwards.
    browser.Search(lan.t);
    host.Update(lan.t);
    browser.Update(lan.t);
    AETHER_CHECK(browser.Sessions().size() == 1);
}

AETHER_TEST(NetDiscovery_HostLimitsItsReplyRate) {
    Lan lan(0.0);
    LoopbackTransport& hl = lan.net.CreateEndpoint();
    LoopbackTransport& bl = lan.net.CreateEndpoint();
    LanHost host(hl, "g");
    host.max_replies_per_second = 10;
    host.SetSession(Info("busy"));
    LanBrowser browser(bl, "g");
    for (int i = 0; i < 100; ++i) browser.SearchHost(hl.LocalAddress(), lan.t);
    host.Update(lan.t);
    AETHER_CHECK(host.Answered() == 10 && host.Ignored() == 90);
    // The next second starts fresh.
    lan.t += 1.5;
    browser.SearchHost(hl.LocalAddress(), lan.t);
    host.Update(lan.t);
    AETHER_CHECK(host.Answered() == 11);
}

AETHER_TEST(NetDiscovery_LongNamesAreTruncatedByTheHost) {
    Lan lan(0.0);
    LoopbackTransport& hl = lan.net.CreateEndpoint();
    LoopbackTransport& bl = lan.net.CreateEndpoint();
    LanHost host(hl, "g");
    SessionInfo info = Info(std::string(300, 'n').c_str());
    host.SetSession(info);
    LanBrowser browser(bl, "g");
    browser.Search(lan.t);
    host.Update(lan.t);
    browser.Update(lan.t);
    AETHER_CHECK(browser.Sessions().size() == 1 && browser.Sessions()[0].info.name.size() == kMaxDiscoveryString);
}
