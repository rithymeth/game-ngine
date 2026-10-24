#include "aether/reflection/serialize.h"
#include "aether/scene/components.h"
#include "aether/server/dedicated_server.h"
#include "test_framework.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

// M8.9: the dedicated server's config (server.json and the command line) and
// a short in-process run over real UDP on localhost: a server on a free port,
// two clients that join with the password and build id, receive replicated
// state and get their server RPC acknowledged, and two that are refused.

using namespace aether;
using namespace aether::server;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

AETHER_TEST(DedicatedServer_ConfigJson) {
    ServerConfig c;
    std::string error;
    CHECK(ParseServerConfigJson(R"({"name":"EU 1","port":27015,"max_players":16,"password":"pw","build_id":89,
        "scene":"Scenes/Arena.ascene","paks":["Paks"],"tick_rate":60,"log_file":"logs/server.log","log_level":"warn",
        "replicate_scene":false,"max_runtime":30,"quit_when_empty":true,"status_interval":0,
        "token_secret":"0123456789abcdef-issuer","require_token":false})", c, &error));
    CHECK(c.token_secret == "0123456789abcdef-issuer" && !c.require_token);
    CHECK(c.name == "EU 1" && c.port == 27015 && c.max_players == 16 && c.password == "pw" && c.build_id == 89);
    CHECK(c.scene == "Scenes/Arena.ascene" && c.paks.size() == 1 && c.tick_rate == 60.0 && c.log_file == "logs/server.log");
    CHECK(c.log_level == "warn" && !c.replicate_scene && c.max_runtime == 30.0 && c.quit_when_empty && c.status_interval == 0.0);

    ServerConfig lan;
    CHECK(lan.lan_advertise && lan.lan_port == 7778 && lan.profile.empty());
    CHECK(ParseServerConfigJson(R"({"lan_advertise":false,"lan_port":27016,"profile":"logs/p.jsonl","profile_interval":0.5})", lan, &error));
    CHECK(!lan.lan_advertise && lan.lan_port == 27016 && lan.profile == "logs/p.jsonl" && lan.profile_interval == 0.5);
    CHECK(ParseServerArgs({"--lan-advertise", "--lan-port", "0", "--profile", "x.jsonl", "--profile-interval", "2"}, lan, &error));
    CHECK(lan.lan_advertise && lan.lan_port == 0 && lan.profile == "x.jsonl" && lan.profile_interval == 2.0);
    CHECK(ParseServerArgs({"--no-lan-advertise"}, lan, &error) && !lan.lan_advertise);
    CHECK(!ParseServerArgs({"--profile-interval", "0"}, lan, &error));
    // Errors leave the config as it was.
    ServerConfig d;
    CHECK(!ParseServerConfigJson(R"({"prot":7777})", d, &error) && error.find("prot") != std::string::npos);
    CHECK(!ParseServerConfigJson(R"({"port":70000})", d, &error));
    CHECK(!ParseServerConfigJson(R"({"port":"7777"})", d, &error));
    CHECK(!ParseServerConfigJson(R"({"max_players":0})", d, &error));
    CHECK(!ParseServerConfigJson(R"({"tick_rate":1000})", d, &error));
    CHECK(!ParseServerConfigJson(R"({"log_level":"loud"})", d, &error));
    CHECK(!ParseServerConfigJson(R"({"paks":"Game.apak"})", d, &error));
    CHECK(!ParseServerConfigJson(R"([1,2])", d, &error));
    CHECK(!ParseServerConfigJson(R"({"token_secret":"short"})", d, &error)); // at least 16 characters
    CHECK(!ParseServerConfigJson(R"({"require_token":1})", d, &error));
    CHECK(!ParseServerConfigJson("{not json", d, &error));
    CHECK(d.port == 7777 && d.max_players == 8);
    // Comments are allowed (it's a hand-edited file).
    CHECK(ParseServerConfigJson("{\n// the game port\n\"port\": 7000}", d, &error) && d.port == 7000);
}

AETHER_TEST(DedicatedServer_CommandLineOverridesFile) {
    const auto dir = std::filesystem::temp_directory_path() / "aether_server_config_test";
    std::filesystem::create_directories(dir);
    const auto file = dir / "server.json";
    std::ofstream(file) << R"({"port":7100,"max_players":4,"password":"file","paks":["a.apak","b.apak"],"tick_rate":20})";
    ServerConfig c;
    std::string error;
    bool help = false;
    // --config is applied first even when it comes last.
    CHECK(ParseServerArgs({"--port", "7200", "--pak", "c.apak", "--quit-when-empty", "--max-runtime", "12.5", "--config", file.string()},
                          c, &error, &help));
    CHECK(c.port == 7200 && c.max_players == 4 && c.tick_rate == 20.0 && c.quit_when_empty && c.max_runtime == 12.5 && !help);
    CHECK(c.paks.size() == 1 && c.paks[0] == "c.apak");
    CHECK(c.password == "file" || std::getenv("AETHER_SERVER_PASSWORD") != nullptr);

    ServerConfig e;
    CHECK(!ParseServerArgs({"--port", "x"}, e, &error));
    CHECK(!ParseServerArgs({"--port"}, e, &error));
    CHECK(!ParseServerArgs({"--max-players", "0"}, e, &error));
    CHECK(!ParseServerArgs({"--bogus"}, e, &error) && error.find("--bogus") != std::string::npos);
    CHECK(!ParseServerArgs({"--config", (dir / "missing.json").string()}, e, &error));
    CHECK(ParseServerArgs({"--help"}, e, &error, &help) && help);
    ServerConfig t;
    CHECK(ParseServerArgs({"--token-secret", "0123456789abcdef-cli", "--optional-token"}, t, &error));
    CHECK((t.token_secret == "0123456789abcdef-cli" || std::getenv("AETHER_SERVER_TOKEN_SECRET") != nullptr) && !t.require_token);
    CHECK(!ParseServerArgs({"--token-secret"}, t, &error));
    std::filesystem::remove_all(dir);
}

AETHER_TEST(DedicatedServer_RealUdpJoinReplicateRpc) {
    RegisterServerComponents();
    World world;
    for (int i = 0; i < 5; ++i) world.CreateEntity(Transform{Vec3{static_cast<f32>(i), 0, 0}, Quaternion{}});
    ServerConfig config;
    config.port = 0;
    config.max_players = 2;
    config.password = "hunter2";
    config.build_id = 89;
    DedicatedServer server;
    std::string error;
    if (!server.Start(config, world, &error)) {
        std::printf("    (no UDP here: %s)\n", error.c_str());
        return;
    }
    CHECK(server.Port() != 0);
    CHECK(server.Stats().replicated_entities == 5);

    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const auto now = [&] { return std::chrono::duration<f64>(Clock::now() - start).count(); };
    auto make = [&](const char* name, const char* password, u32 build, f64 duration) {
        NetClientConfig cc;
        cc.server = net::Address::Loopback(server.Port());
        cc.name = name;
        cc.password = password;
        cc.build_id = build;
        cc.duration = duration;
        cc.join_timeout = 3.0;
        auto c = std::make_unique<NetClient>();
        CHECK(c->Start(cc, now(), &error));
        return c;
    };
    std::vector<std::unique_ptr<NetClient>> good;
    good.push_back(make("ada", "hunter2", 89, 1.5));
    good.push_back(make("ada", "hunter2", 89, 1.5)); // same name: the server makes it unique
    auto wrong_password = make("eve", "guess", 89, 1.0);
    auto wrong_build = make("old", "hunter2", 88, 1.0);
    auto all_done = [&] {
        for (auto& c : good)
            if (!c->Done()) return false;
        return wrong_password->Done() && wrong_build->Done();
    };
    while (!all_done() && now() < 8.0) {
        server.Tick(now());
        for (auto& c : good) c->Tick(now());
        wrong_password->Tick(now());
        wrong_build->Tick(now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(all_done());
    for (auto& c : good) {
        const NetClientResult& r = c->Result();
        std::printf("    [m8.9] %s\n", r.ToJson().c_str());
        CHECK(r.joined && r.pawn_seen && r.rpc_ack && r.rpcs_sent == 1);
        CHECK(r.entities_seen >= 6); // the 5 scene entities and at least our pawn
        CHECK(r.bytes_received > 0 && r.bytes_sent > 0 && r.snapshots > 0);
        CHECK(r.Passed(true));
    }
    CHECK(good[0]->Result().player_name != good[1]->Result().player_name);
    CHECK(!wrong_password->Result().joined && wrong_password->Result().join_result == "WrongPassword");
    CHECK(!wrong_build->Result().joined && wrong_build->Result().join_result == "BuildMismatch");
    CHECK(!wrong_password->Result().Passed(true));
    CHECK(wrong_password->Result().entities_seen == 0 && wrong_build->Result().entities_seen == 0); // no state before joining

    // The good clients said goodbye: the server sees the room empty again.
    for (int i = 0; i < 200 && !server.EmptyAfterPlay(); ++i) {
        server.Tick(now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const ServerStats s = server.Stats();
    CHECK(server.EmptyAfterPlay());
    CHECK(s.joins == 2 && s.leaves == 2 && s.refused == 2 && s.peak_players == 2 && s.pings == 2);
    CHECK(s.replicated_entities == 5); // the pawns went with their players
    server.Stop();
    CHECK(!server.Running());
}

AETHER_TEST(DedicatedServer_RealUdpJoinTokens) {
    RegisterServerComponents();
    World world;
    world.CreateEntity(Transform{Vec3{1, 0, 0}, Quaternion{}});
    ServerConfig config;
    config.port = 0;
    config.max_players = 4;
    config.build_id = 89;
    config.token_secret = "dedicated-server-test-issuer-secret";
    DedicatedServer server;
    std::string error;
    if (!server.Start(config, world, &error)) {
        std::printf("    (no UDP here: %s)\n", error.c_str());
        return;
    }
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const auto now = [&] { return std::chrono::duration<f64>(Clock::now() - start).count(); };
    const u64 unix_now = net::UnixNow();
    auto make = [&](const char* name, const std::string& token) {
        NetClientConfig cc;
        cc.server = net::Address::Loopback(server.Port());
        cc.name = name;
        cc.token = token;
        cc.build_id = 89;
        cc.duration = 1.0;
        cc.join_timeout = 3.0;
        auto c = std::make_unique<NetClient>();
        CHECK(c->Start(cc, now(), &error));
        return c;
    };
    std::vector<std::unique_ptr<NetClient>> clients;
    clients.push_back(make("good", net::IssueJoinToken(config.token_secret, {"acct-good", unix_now + 60, 89})));
    clients.push_back(make("expired", net::IssueJoinToken(config.token_secret, {"acct-old", unix_now - 5, 89})));
    clients.push_back(make("forged", net::IssueJoinToken("some-other-issuer-secret", {"acct-x", unix_now + 60, 89})));
    clients.push_back(make("build", net::IssueJoinToken(config.token_secret, {"acct-b", unix_now + 60, 88})));
    clients.push_back(make("none", ""));
    auto all_done = [&] { return std::all_of(clients.begin(), clients.end(), [](auto& c) { return c->Done(); }); };
    while (!all_done() && now() < 8.0) {
        server.Tick(now());
        for (auto& c : clients) c->Tick(now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(all_done());
    CHECK(clients[0]->Result().joined && clients[0]->Result().rpc_ack);
    CHECK(clients[1]->Result().join_result == "TokenExpired");
    CHECK(clients[2]->Result().join_result == "BadToken");
    CHECK(clients[3]->Result().join_result == "BadToken");
    CHECK(clients[4]->Result().join_result == "BadToken");
    for (usize i = 1; i < clients.size(); ++i) CHECK(!clients[i]->Result().joined && clients[i]->Result().entities_seen == 0);
    server.Stop();
}

// M8.9: the dedicated server answers LAN discovery over real UDP (broadcast plus a 127.0.0.1 query, since a
// container may have no broadcast route), the advert carries name/players/max/build/port/requires_token, a
// client joins what it found, and the profile capture records per-peer and per-channel traffic.
AETHER_TEST(DedicatedServer_RealUdpLanDiscoveryAndProfile) {
    RegisterServerComponents();
    World world;
    for (int i = 0; i < 3; ++i) world.CreateEntity(Transform{Vec3{static_cast<f32>(i), 0, 0}, Quaternion{}});
    ServerConfig config;
    config.port = 0;
    config.lan_port = 0; // any free port
    config.name = "LAN test";
    config.max_players = 3;
    config.build_id = 89;
    config.token_secret = "lan-discovery-test-secret-0123";
    config.require_token = false;
    DedicatedServer server;
    std::string error;
    if (!server.Start(config, world, &error)) {
        std::printf("    (no UDP here: %s)\n", error.c_str());
        return;
    }
    CHECK(server.LanPort() != 0 && server.Advert() != nullptr);
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const auto now = [&] { return std::chrono::duration<f64>(Clock::now() - start).count(); };
    DiscoverConfig dc;
    dc.build_id = 89;
    dc.lan_port = server.LanPort();
    dc.timeout = 0.6;
    std::vector<net::LanSession> found = DiscoverServers(dc, &error, [&] { server.Tick(now()); });
    std::printf("    [m8.9] AETHER_DISCOVER %s\n", DiscoveryJson(found).c_str());
    auto mine = std::find_if(found.begin(), found.end(), [&](const net::LanSession& s) { return s.info.port == server.Port() && s.info.name == "LAN test"; });
    CHECK(mine != found.end());
    if (mine == found.end()) return;
    CHECK(mine->compatible && mine->info.build == 89 && mine->info.max_players == 3 && mine->info.players == 0);
    CHECK(mine->info.Property("requires_token") && *mine->info.Property("requires_token") == "0");
    CHECK(DiscoveryJson(found).find("\"requires_token\":false") != std::string::npos);
    CHECK(server.Stats().lan_answered >= 1);
    const net::Address found_address = mine->address;
    // Another build sees it as incompatible.
    dc.build_id = 88;
    found = DiscoverServers(dc, &error, [&] { server.Tick(now()); });
    CHECK(!found.empty() && !found[0].compatible);

    // Join what was found and play; the profile sees the peer.
    NetClientConfig cc;
    cc.server = net::Address::Loopback(found_address.port);
    cc.build_id = 89;
    cc.duration = 1.0;
    NetClient client;
    CHECK(client.Start(cc, now(), &error));
    std::string record;
    f64 last = 0;
    u32 advert_players = 0;
    while (!client.Done() && now() < 8.0) {
        server.Tick(now());
        client.Tick(now());
        if (now() - last > 0.3 && server.Stats().players == 1) last = now(), record = server.ProfileRecord(now());
        if (server.Stats().players == 1) advert_players = std::max(advert_players, server.Advert()->players); // the advert follows
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::printf("    [m8.9] client %s\n", client.Result().ToJson().c_str());
    CHECK(client.Result().Passed(true) && advert_players == 1);
    std::printf("    [m8.9] profile record %s\n", record.c_str());
    const reflect::Json j = reflect::Json::parse(record, nullptr, false);
    CHECK(!j.is_discarded() && j["players"] == 1 && j["peers"].size() == 1);
    if (!j.is_discarded() && j["peers"].size() == 1) {
        const reflect::Json& p = j["peers"][0];
        CHECK(p["bytes_sent"].get<u64>() > 0 && p["bytes_received"].get<u64>() > 0 && p["channels"].size() == 3);
        u64 channel_sent = 0;
        for (const reflect::Json& c : p["channels"]) channel_sent += c["bytes_sent"].get<u64>();
        CHECK(channel_sent > 0 && channel_sent <= p["bytes_sent"].get<u64>());
        CHECK(j["interval"]["snapshots"].get<u64>() > 0 && j["interval"]["snapshot_bytes"].get<u64>() > 0);
        CHECK(j["security"].contains("bad_macs") && j["lan"]["answered"].get<u64>() >= 1);
    }
    const std::string summary = server.ProfileSummaryJson();
    std::printf("    [m8.9] profile summary %s\n", summary.c_str());
    CHECK(summary.find("\"peak_peers\":1") != std::string::npos);
    server.Stop();
}
