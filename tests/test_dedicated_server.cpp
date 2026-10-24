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
