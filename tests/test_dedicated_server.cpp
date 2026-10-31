#include "aether/reflection/serialize.h"
#include "aether/scene/components.h"
#include "aether/server/dedicated_server.h"
#include "test_framework.h"

#include <algorithm>
#include <array>
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

// A shutdown with an active player must retain acknowledged RPC history,
// just as a normal PlayerLeft event does, without destroying game-owned scene data.
AETHER_TEST(DedicatedServer_StopPreservesActiveRpcHistory) {
    RegisterServerComponents();
    World world;
    // Active shutdown exercises a different path than completed player leaves.
    const Entity scene = world.CreateEntity(Transform{Vec3{7, 2, -3}, Quaternion{0, 0, 0.6f, 0.8f}, Vec3{2, 3, 4}});
    ServerConfig config;
    config.port = 0;
    config.lan_advertise = false;
    config.max_players = 1;
    config.build_id = 96;
    DedicatedServer server;
    std::string error;
    if (!server.Start(config, world, &error)) {
        std::printf("    (no UDP here: %s)\n", error.c_str());
        return;
    }
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const auto now = [&] { return std::chrono::duration<f64>(Clock::now() - start).count(); };
    NetClientConfig cc;
    cc.server = net::Address::Loopback(server.Port());
    cc.name = "active-rpc";
    cc.build_id = 96;
    cc.duration = 10.0; // Still connected when Stop() is called.
    cc.join_timeout = 3.0;
    NetClient client;
    CHECK(client.Start(cc, now(), &error));
    const f64 deadline = now() + 5.0;
    while (!client.Result().rpc_ack && now() < deadline) {
        server.Tick(now());
        client.Tick(now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(client.Result().joined && client.Result().rpc_ack && !client.Done());
    CHECK(server.Stats().players == 1 && server.Stats().pings == 1);
    const auto* before_stop = world.GetComponent<net::NetIdentity>(scene);
    CHECK(before_stop != nullptr);
    const u32 scene_net_id = before_stop ? before_stop->net_id : 0;
    server.Stop();
    CHECK(!server.Running());
    CHECK(server.Stats().players == 0 && server.Stats().pings == 1 &&
          server.Stats().joins == 1 && server.Stats().leaves == 0);
    CHECK(world.EntityCount() == 1 && world.IsAlive(scene));
    const auto* after_stop = world.GetComponent<net::NetIdentity>(scene);
    CHECK(after_stop != nullptr);
    if (after_stop) CHECK(after_stop->net_id == scene_net_id && after_stop->owner == net::kNoPeer);
    const Transform* preserved = world.GetComponent<Transform>(scene);
    CHECK(preserved != nullptr);
    if (preserved) {
        CHECK(preserved->position.x == 7.0f && preserved->position.y == 2.0f && preserved->position.z == -3.0f);
        CHECK(preserved->rotation.x == 0.0f && preserved->rotation.y == 0.0f &&
              preserved->rotation.z == 0.6f && preserved->rotation.w == 0.8f);
        CHECK(preserved->scale.x == 2.0f && preserved->scale.y == 3.0f && preserved->scale.z == 4.0f);
    }
    // Stop may be called again by the destructor; it must not double-count RPCs
    // or mutate the game-owned scene on a second shutdown.
    server.Stop();
    CHECK(!server.Running() && server.Stats().players == 0 && server.Stats().pings == 1);
    CHECK(server.Stats().joins == 1 && server.Stats().leaves == 0);
    CHECK(world.EntityCount() == 1 && world.IsAlive(scene));
    const auto* twice_stopped = world.GetComponent<net::NetIdentity>(scene);
    CHECK(twice_stopped && twice_stopped->net_id == scene_net_id && twice_stopped->owner == net::kNoPeer);
    const Transform* twice_preserved = world.GetComponent<Transform>(scene);
    CHECK(twice_preserved && twice_preserved->position.x == 7.0f &&
          twice_preserved->position.y == 2.0f && twice_preserved->position.z == -3.0f);
    if (twice_preserved) {
        CHECK(twice_preserved->rotation.x == 0.0f && twice_preserved->rotation.y == 0.0f &&
              twice_preserved->rotation.z == 0.6f && twice_preserved->rotation.w == 0.8f);
        CHECK(twice_preserved->scale.x == 2.0f && twice_preserved->scale.y == 3.0f && twice_preserved->scale.z == 4.0f);
    }
}

AETHER_TEST(DedicatedServer_RealUdpJoinReplicateRpc) {
    RegisterServerComponents();
    World world;
    std::array<Entity, 5> scenes{};
    for (usize i = 0; i < scenes.size(); ++i) {
        // A non-default orientation and scale catch shutdown mutations that
        // an identity-only or position-only scene check would miss.
        const Quaternion rotation = i == 3 ? Quaternion{0, 0, 0.6f, 0.8f} : Quaternion{};
        const Vec3 scale = i == 3 ? Vec3{2, 3, 4} : Vec3{1, 1, 1};
        scenes[i] = world.CreateEntity(Transform{Vec3{static_cast<f32>(i), 0, 0}, rotation, scale});
    }
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
    World shared_client_world; // same World a game host can give its PhysicsScene
    auto make = [&](const char* name, const char* password, u32 build, f64 duration, World* external = nullptr) {
        NetClientConfig cc;
        cc.server = net::Address::Loopback(server.Port());
        cc.name = name;
        cc.password = password;
        cc.build_id = build;
        cc.duration = duration;
        cc.join_timeout = 3.0;
        auto c = std::make_unique<NetClient>();
        if (external) CHECK(c->UseWorld(*external));
        CHECK(c->Start(cc, now(), &error));
        return c;
    };
    std::vector<std::unique_ptr<NetClient>> good;
    good.push_back(make("ada", "hunter2", 89, 1.5, &shared_client_world));
    good.push_back(make("ada", "hunter2", 89, 1.5)); // same name: the server makes it unique
    CHECK(&good[0]->GetWorld() == &shared_client_world);
    CHECK(&good[1]->GetWorld() != &shared_client_world);
    CHECK(!good[0]->UseWorld(shared_client_world)); // no rebinding after Start
    // A second Start must not overwrite the live session's config or World.
    NetClientConfig restart_attempt;
    restart_attempt.server = net::Address::Loopback(server.Port());
    restart_attempt.build_id = 89;
    error.clear();
    CHECK(!good[0]->Start(restart_attempt, now(), &error));
    CHECK(error.find("already started") != std::string::npos);
    CHECK(&good[0]->GetWorld() == &shared_client_world);
    u32 accepted_snapshots = 0;
    u32 populated_snapshots = 0;
    good[0]->OnSnapshotApplied([&](World& snapshot_world) {
        CHECK(&snapshot_world == &shared_client_world);
        ++accepted_snapshots;
        // A valid initial snapshot can precede the first replicated spawn.
        if (snapshot_world.EntityCount() > 0) ++populated_snapshots;
    });
    // A refused join must not touch a game-owned World, including its pre-existing entities.
    World refused_client_world;
    const Entity preserved = refused_client_world.CreateEntity(Transform{Vec3{99, 4, 2}, Quaternion{}});
    auto wrong_password = make("eve", "guess", 89, 1.0, &refused_client_world);
    auto wrong_build = make("old", "hunter2", 88, 1.0);
    u32 refused_snapshots = 0;
    wrong_password->OnSnapshotApplied([&](World&) { ++refused_snapshots; });
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
    CHECK(accepted_snapshots > 0 && populated_snapshots > 0 && refused_snapshots == 0);
    CHECK(shared_client_world.EntityCount() >= 6); // actual UDP spawns in external World
    CHECK(good[0]->Result().player_name != good[1]->Result().player_name);
    CHECK(!wrong_password->Result().joined && wrong_password->Result().join_result == "WrongPassword");
    CHECK(!wrong_build->Result().joined && wrong_build->Result().join_result == "BuildMismatch");
    CHECK(!wrong_password->Result().Passed(true));
    CHECK(wrong_password->Result().entities_seen == 0 && wrong_build->Result().entities_seen == 0); // no state before joining
    CHECK(&wrong_password->GetWorld() == &refused_client_world);
    CHECK(refused_client_world.EntityCount() == 1 && refused_client_world.IsAlive(preserved));
    const Transform* preserved_transform = refused_client_world.GetComponent<Transform>(preserved);
    CHECK(preserved_transform != nullptr);
    if (preserved_transform) CHECK(preserved_transform->position.x == 99.0f && preserved_transform->position.y == 4.0f &&
                                   preserved_transform->position.z == 2.0f);
    // A refused session is terminal for this NetClient too: a new attempt needs a fresh object.
    error.clear();
    CHECK(!wrong_password->Start(restart_attempt, now(), &error));
    CHECK(error.find("fresh client") != std::string::npos);
    CHECK(!wrong_password->UseWorld(shared_client_world));
    CHECK(refused_snapshots == 0 && refused_client_world.EntityCount() == 1);
    // Even after Leave/Done, the same object cannot silently reuse stale
    // replication and physics state; rejoin uses a fresh NetClient.
    error.clear();
    CHECK(!good[0]->Start(restart_attempt, now(), &error));
    CHECK(error.find("fresh client") != std::string::npos);
    CHECK(good[0]->Result().joined && good[0]->Result().snapshots > 0);

    // The good clients said goodbye: the server sees the room empty again.
    for (int i = 0; i < 200 && !server.EmptyAfterPlay(); ++i) {
        server.Tick(now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const ServerStats s = server.Stats();
    CHECK(server.EmptyAfterPlay());
    CHECK(s.joins == 2 && s.leaves == 2 && s.refused == 2 && s.peak_players == 2 && s.pings == 2);
    CHECK(s.replicated_entities == 5); // the pawns went with their players
    // Snapshot every scene identity before shutdown: an unchanged entity count
    // alone cannot detect an accidental identity or ownership rewrite.
    std::array<u32, 5> scene_net_ids{};
    for (usize i = 0; i < scenes.size(); ++i) {
        const auto* identity = world.GetComponent<net::NetIdentity>(scenes[i]);
        CHECK(identity != nullptr);
        if (identity) {
            scene_net_ids[i] = identity->net_id;
            CHECK(identity->net_id != 0 && identity->owner == net::kNoPeer &&
                  std::string(identity->archetype) == kSceneArchetype);
        }
    }
    server.Stop();
    CHECK(!server.Running());
    // A normal leave already accounted for each RPC. Stopping an empty server
    // must not count the completed sessions again or erase their history.
    const ServerStats stopped = server.Stats();
    CHECK(stopped.players == 0 && stopped.joins == s.joins && stopped.leaves == s.leaves);
    CHECK(stopped.refused == s.refused && stopped.peak_players == s.peak_players && stopped.pings == s.pings);
    CHECK(world.EntityCount() == 5);
    for (usize i = 0; i < scenes.size(); ++i) {
        CHECK(world.IsAlive(scenes[i]));
        const auto* identity = world.GetComponent<net::NetIdentity>(scenes[i]);
        const auto* transform = world.GetComponent<Transform>(scenes[i]);
        CHECK(identity && identity->net_id == scene_net_ids[i] &&
              identity->owner == net::kNoPeer && std::string(identity->archetype) == kSceneArchetype);
        CHECK(transform && transform->position.x == static_cast<f32>(i) &&
              transform->position.y == 0.0f && transform->position.z == 0.0f);
        if (transform) {
            const Quaternion expected = i == 3 ? Quaternion{0, 0, 0.6f, 0.8f} : Quaternion{};
            CHECK(transform->rotation.x == expected.x && transform->rotation.y == expected.y &&
                  transform->rotation.z == expected.z && transform->rotation.w == expected.w);
            CHECK(transform->scale.x == (i == 3 ? 2.0f : 1.0f) &&
                  transform->scale.y == (i == 3 ? 3.0f : 1.0f) &&
                  transform->scale.z == (i == 3 ? 4.0f : 1.0f));
        }
    }
}

// A capacity refusal is a real UDP join failure, not permission to replicate
// into the caller's game-owned World. The already joined player must stay live.
AETHER_TEST(DedicatedServer_RealUdpFullPreservesGameWorld) {
    RegisterServerComponents();
    World server_world;
    server_world.CreateEntity(Transform{Vec3{2, 0, 0}, Quaternion{}});
    ServerConfig config;
    config.port = 0;
    config.lan_advertise = false;
    config.max_players = 1;
    config.build_id = 91;
    DedicatedServer server;
    std::string error;
    if (!server.Start(config, server_world, &error)) {
        std::printf("    (no UDP here: %s)\n", error.c_str());
        return;
    }

    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const auto now = [&] { return std::chrono::duration<f64>(Clock::now() - start).count(); };
    NetClientConfig cc;
    cc.server = net::Address::Loopback(server.Port());
    cc.build_id = 91;
    cc.join_timeout = 3.0;
    cc.duration = 5.0; // keep the first slot occupied throughout the refusal
    cc.send_rpc = false;
    cc.name = "first";
    NetClient first;
    CHECK(first.Start(cc, now(), &error));
    while ((!first.Result().joined || first.Result().snapshots == 0) && now() < 4.0) {
        server.Tick(now());
        first.Tick(now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(first.Result().joined && first.Result().snapshots > 0);
    if (!first.Result().joined) {
        server.Stop();
        return;
    }

    World game_world;
    const Entity preserved = game_world.CreateEntity(Transform{Vec3{31, 7, -4}, Quaternion{}});
    NetClient refused;
    CHECK(refused.UseWorld(game_world));
    u32 applied = 0;
    refused.OnSnapshotApplied([&](World&) { ++applied; });
    cc.name = "second";
    CHECK(refused.Start(cc, now(), &error));
    const f64 deadline = now() + 3.0;
    while (!refused.Done() && now() < deadline) {
        server.Tick(now());
        first.Tick(now());
        refused.Tick(now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(refused.Done());
    CHECK(!refused.Result().joined && refused.Result().join_result == "Full");
    CHECK(refused.Result().entities_seen == 0 && refused.Result().snapshots == 0 && applied == 0);
    CHECK(game_world.EntityCount() == 1 && game_world.IsAlive(preserved));
    const Transform* original = game_world.GetComponent<Transform>(preserved);
    CHECK(original != nullptr);
    if (original) CHECK(original->position.x == 31.0f && original->position.y == 7.0f &&
                        original->position.z == -4.0f);
    CHECK(&refused.GetWorld() == &game_world);
    CHECK(!refused.UseWorld(server_world));
    CHECK(first.Result().joined && !first.Done());
    CHECK(server.Stats().players == 1 && server.Stats().peak_players == 1 &&
          server.Stats().joins == 1 && server.Stats().refused == 1);

    // A refused peer must not consume the only slot permanently. Once the
    // legitimate player leaves, a fresh real UDP client can join this same
    // authoritative server and receive the scene and its own pawn.
    const f64 leave_deadline = now() + 7.0;
    while ((!first.Done() || !server.EmptyAfterPlay()) && now() < leave_deadline) {
        server.Tick(now());
        first.Tick(now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(first.Done() && server.EmptyAfterPlay());
    CHECK(server.Stats().players == 0 && server.Stats().replicated_entities == 1);

    // The refused client must remain terminal even when the server has a
    // free slot. Retrying the same object must not reuse its old UDP host,
    // rebind the external World, or turn the earlier refusal into a join.
    error.clear();
    CHECK(!refused.Start(cc, now(), &error));
    CHECK(error.find("fresh client") != std::string::npos);
    CHECK(refused.Done() && !refused.Result().joined && refused.Result().join_result == "Full");
    CHECK(&refused.GetWorld() == &game_world && game_world.EntityCount() == 1);
    CHECK(game_world.IsAlive(preserved) && refused.Result().snapshots == 0 && applied == 0);

    World next_world;
    NetClient next;
    CHECK(next.UseWorld(next_world));
    cc.name = "after-full";
    cc.duration = 1.0;
    CHECK(next.Start(cc, now(), &error));
    const f64 rejoin_deadline = now() + 4.0;
    while (!next.Done() && now() < rejoin_deadline) {
        server.Tick(now());
        next.Tick(now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(next.Done() && next.Result().joined);
    CHECK(next.Result().snapshots > 0 && next.Result().entities_seen >= 2);
    CHECK(next_world.EntityCount() >= 2);
    for (int i = 0; i < 200 && !server.EmptyAfterPlay(); ++i) {
        server.Tick(now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(server.EmptyAfterPlay());
    CHECK(server.Stats().joins == 2 && server.Stats().leaves == 2 &&
          server.Stats().refused == 1 && server.Stats().replicated_entities == 1);
    server.Stop();
}

// Restarting a dedicated server while a UDP client is still joined must
// destroy only the server-owned pawn. A fresh session on the restarted server
// must replicate the preserved scene and a new pawn, without stale slots.
AETHER_TEST(DedicatedServer_RealUdpRestartCleansLivePawn) {
    RegisterServerComponents();
    World server_world;
    const Entity preserved = server_world.CreateEntity(Transform{Vec3{13, 2, -6}, Quaternion{}});
    // A scene with a sparse, preassigned ID must not collide with the
    // automatically assigned scene or pawn IDs across server restarts.
    net::NetIdentity sparse_identity;
    sparse_identity.net_id = 4096;
    net::SetNetArchetype(sparse_identity, kSceneArchetype);
    const Entity sparse_scene = server_world.CreateEntity(
        sparse_identity, Transform{Vec3{-21, 5, 8}, Quaternion{}});
    ServerConfig config;
    config.port = 0;
    config.lan_advertise = false;
    config.max_players = 1;
    config.build_id = 93;
    DedicatedServer server;
    std::string error;
    if (!server.Start(config, server_world, &error)) {
        std::printf("    (no UDP here: %s)\n", error.c_str());
        return;
    }

    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const auto now = [&] { return std::chrono::duration<f64>(Clock::now() - start).count(); };
    NetClientConfig cc;
    cc.server = net::Address::Loopback(server.Port());
    cc.build_id = 93;
    cc.name = "before-restart";
    cc.join_timeout = 3.0;
    cc.duration = 10.0; // still joined when the server stops
    cc.send_rpc = false;
    NetClient first;
    CHECK(first.Start(cc, now(), &error));
    const f64 first_deadline = now() + 4.0;
    while ((!first.Result().joined || !first.Result().pawn_seen || first.Result().snapshots == 0) &&
           now() < first_deadline) {
        server.Tick(now());
        first.Tick(now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(first.Result().joined && first.Result().pawn_seen && first.Result().snapshots > 0);
    if (!first.Result().joined) {
        server.Stop();
        return;
    }
    CHECK(!first.Done() && server.Stats().players == 1);
    CHECK(server_world.EntityCount() == 3);
    const net::NetIdentity* assigned = server_world.GetComponent<net::NetIdentity>(preserved);
    CHECK(assigned != nullptr && assigned->net_id > sparse_identity.net_id);
    const u32 assigned_id = assigned ? assigned->net_id : 0;

    server.Stop(); // live peer is disconnected; only its pawn may be destroyed
    CHECK(!server.Running());
    CHECK(server_world.EntityCount() == 2 && server_world.IsAlive(preserved) &&
          server_world.IsAlive(sparse_scene));
    const Transform* original = server_world.GetComponent<Transform>(preserved);
    CHECK(original != nullptr);
    if (original) CHECK(original->position.x == 13.0f && original->position.y == 2.0f &&
                        original->position.z == -6.0f);
    const Transform* sparse_transform = server_world.GetComponent<Transform>(sparse_scene);
    CHECK(sparse_transform != nullptr);
    if (sparse_transform) CHECK(sparse_transform->position.x == -21.0f &&
                                sparse_transform->position.y == 5.0f && sparse_transform->position.z == 8.0f);
    const net::NetIdentity* kept = server_world.GetComponent<net::NetIdentity>(preserved);
    CHECK(kept != nullptr && kept->net_id == assigned_id);

    // The old NetClient cannot be recycled even after the server restarts.
    error.clear();
    CHECK(!first.Start(cc, now(), &error));
    CHECK(error.find("fresh client") != std::string::npos);
    CHECK(first.Result().joined && first.Result().snapshots > 0);

    error.clear();
    CHECK(server.Start(config, server_world, &error));
    if (!server.Running()) return;
    CHECK(server.Stats().players == 0 && server.Stats().joins == 0 &&
          server.Stats().leaves == 0 && server.Stats().refused == 0);
    CHECK(server_world.EntityCount() == 2);

    World next_world;
    NetClient next;
    CHECK(next.UseWorld(next_world));
    cc.server = net::Address::Loopback(server.Port());
    cc.name = "after-restart";
    cc.duration = 0.8;
    CHECK(next.Start(cc, now(), &error));
    const f64 next_deadline = now() + 4.0;
    while (!next.Done() && now() < next_deadline) {
        server.Tick(now());
        next.Tick(now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(next.Done() && next.Result().joined);
    CHECK(next.Result().pawn_seen && next.Result().snapshots > 0);
    CHECK(next.Result().entities_seen >= 3 && next_world.EntityCount() >= 3);
    std::vector<u32> received_ids;
    next_world.ForEachArchetype([&](const Archetype& archetype) {
        const ComponentId id = GetComponentId<net::NetIdentity>();
        if (!archetype.Has(id)) return;
        for (usize chunk = 0; chunk < archetype.ChunkCount(); ++chunk) {
            const auto* identities = static_cast<const net::NetIdentity*>(archetype.ComponentArray(chunk, id));
            for (u32 row = 0; row < archetype.ChunkEntityCount(chunk); ++row)
                received_ids.push_back(identities[row].net_id);
        }
    });
    CHECK(received_ids.size() >= 3);
    CHECK(std::find(received_ids.begin(), received_ids.end(), sparse_identity.net_id) != received_ids.end());
    CHECK(std::find(received_ids.begin(), received_ids.end(), assigned_id) != received_ids.end());
    std::sort(received_ids.begin(), received_ids.end());
    CHECK(std::adjacent_find(received_ids.begin(), received_ids.end()) == received_ids.end());
    for (int i = 0; i < 200 && !server.EmptyAfterPlay(); ++i) {
        server.Tick(now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(server.EmptyAfterPlay());
    CHECK(server.Stats().joins == 1 && server.Stats().leaves == 1 &&
          server.Stats().players == 0 && server.Stats().replicated_entities == 2);
    CHECK(server_world.EntityCount() == 2 && server_world.IsAlive(preserved) &&
          server_world.IsAlive(sparse_scene));
    server.Stop();
}

// A live shutdown with two joined UDP peers must clean up *both* server-owned
// pawns without destroying game-owned scene state or poisoning the next session.
AETHER_TEST(DedicatedServer_RealUdpRestartCleansMultipleLivePawns) {
    RegisterServerComponents();
    World server_world;
    const Entity scene = server_world.CreateEntity(Transform{Vec3{7, -3, 11}, Quaternion{}});
    ServerConfig config;
    config.port = 0;
    config.lan_advertise = false;
    config.max_players = 2;
    config.build_id = 94;
    DedicatedServer server;
    std::string error;
    if (!server.Start(config, server_world, &error)) {
        std::printf("    (no UDP here: %s)\n", error.c_str());
        return;
    }

    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const auto now = [&] { return std::chrono::duration<f64>(Clock::now() - start).count(); };
    NetClientConfig cc;
    cc.server = net::Address::Loopback(server.Port());
    cc.build_id = 94;
    cc.join_timeout = 3.0;
    cc.duration = 10.0; // both clients remain joined when the server stops
    cc.send_rpc = true; // both active pawns must contribute acknowledged RPCs on Stop
    cc.name = "first-live";
    NetClient first;
    CHECK(first.Start(cc, now(), &error));
    cc.name = "second-live";
    NetClient second;
    CHECK(second.Start(cc, now(), &error));
    const f64 deadline = now() + 5.0;
    while ((!first.Result().joined || !second.Result().joined ||
            !first.Result().pawn_seen || !second.Result().pawn_seen ||
            first.Result().snapshots == 0 || second.Result().snapshots == 0 ||
            !first.Result().rpc_ack || !second.Result().rpc_ack) && now() < deadline) {
        server.Tick(now());
        first.Tick(now());
        second.Tick(now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(first.Result().joined && second.Result().joined);
    CHECK(first.Result().pawn_seen && second.Result().pawn_seen);
    CHECK(first.Result().snapshots > 0 && second.Result().snapshots > 0);
    CHECK(first.Result().rpc_ack && second.Result().rpc_ack);
    if (!first.Result().joined || !second.Result().joined) {
        server.Stop();
        return;
    }
    CHECK(!first.Done() && !second.Done());
    CHECK(server.Stats().players == 2 && server.Stats().joins == 2);
    CHECK(server_world.EntityCount() == 3 && server.Stats().replicated_entities == 3);
    CHECK(server.Stats().pings == 2);

    server.Stop();
    CHECK(!server.Running());
    // Both live pawn counters must be folded into history exactly once.
    CHECK(server.Stats().players == 0 && server.Stats().pings == 2);
    CHECK(server.Stats().joins == 2 && server.Stats().leaves == 0);
    server.Stop();
    CHECK(server.Stats().players == 0 && server.Stats().pings == 2);
    CHECK(server.Stats().joins == 2 && server.Stats().leaves == 0);
    CHECK(server_world.EntityCount() == 1 && server_world.IsAlive(scene));
    const Transform* preserved = server_world.GetComponent<Transform>(scene);
    CHECK(preserved != nullptr);
    if (preserved) CHECK(preserved->position.x == 7.0f &&
                         preserved->position.y == -3.0f && preserved->position.z == 11.0f);
    error.clear();
    CHECK(!first.Start(cc, now(), &error));
    CHECK(error.find("fresh client") != std::string::npos);
    error.clear();
    CHECK(!second.Start(cc, now(), &error));
    CHECK(error.find("fresh client") != std::string::npos);

    error.clear();
    CHECK(server.Start(config, server_world, &error));
    if (!server.Running()) return;
    CHECK(server.Stats().players == 0 && server.Stats().joins == 0 &&
          server.Stats().leaves == 0 && server.Stats().refused == 0);
    CHECK(server_world.EntityCount() == 1);
    NetClient fresh;
    cc.server = net::Address::Loopback(server.Port());
    cc.name = "fresh-after-two";
    cc.duration = 0.8;
    CHECK(fresh.Start(cc, now(), &error));
    const f64 fresh_deadline = now() + 4.0;
    while (!fresh.Done() && now() < fresh_deadline) {
        server.Tick(now());
        fresh.Tick(now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(fresh.Done() && fresh.Result().joined && fresh.Result().pawn_seen);
    CHECK(fresh.Result().snapshots > 0 && fresh.Result().entities_seen >= 2);
    for (int i = 0; i < 200 && !server.EmptyAfterPlay(); ++i) {
        server.Tick(now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(server.EmptyAfterPlay());
    CHECK(server.Stats().joins == 1 && server.Stats().leaves == 1 &&
          server.Stats().players == 0 && server.Stats().replicated_entities == 1);
    CHECK(server_world.EntityCount() == 1 && server_world.IsAlive(scene));
    server.Stop();
}

// Releasing one live UDP player must free only that player's pawn and slot.
// The remaining player must keep receiving authoritative state while a fresh
// peer fills the vacant slot on the same server without a restart.
AETHER_TEST(DedicatedServer_RealUdpPartialLeaveReusesSlot) {
    RegisterServerComponents();
    World world;
    const Entity scene = world.CreateEntity(Transform{Vec3{12, 4, -6}, Quaternion{}});
    ServerConfig config;
    config.port = 0;
    config.lan_advertise = false;
    config.max_players = 2;
    config.build_id = 95;
    DedicatedServer server;
    std::string error;
    if (!server.Start(config, world, &error)) {
        std::printf("    (no UDP here: %s)\\n", error.c_str());
        return;
    }

    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const auto now = [&] { return std::chrono::duration<f64>(Clock::now() - start).count(); };
    NetClientConfig cc;
    cc.server = net::Address::Loopback(server.Port());
    cc.build_id = 95;
    cc.join_timeout = 3.0;
    cc.send_rpc = false;
    cc.name = "leaving";
    cc.duration = 4.0;
    NetClient leaving;
    CHECK(leaving.Start(cc, now(), &error));
    cc.name = "remaining";
    cc.duration = 10.0;
    NetClient remaining;
    CHECK(remaining.Start(cc, now(), &error));

    const f64 join_deadline = now() + 5.0;
    while ((!leaving.Result().joined || !remaining.Result().joined ||
            !leaving.Result().pawn_seen || !remaining.Result().pawn_seen ||
            leaving.Result().snapshots == 0 || remaining.Result().snapshots == 0) &&
           now() < join_deadline) {
        server.Tick(now());
        leaving.Tick(now());
        remaining.Tick(now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(leaving.Result().joined && remaining.Result().joined);
    CHECK(leaving.Result().pawn_seen && remaining.Result().pawn_seen);
    if (!leaving.Result().joined || !remaining.Result().joined) {
        server.Stop();
        return;
    }
    CHECK(server.Stats().players == 2 && world.EntityCount() == 3);

    // Refusal while both slots are occupied must not touch a separate
    // game-owned World, even when a slot subsequently becomes available.
    World refused_world;
    const Entity preserved_refused_entity = refused_world.CreateEntity(Transform{Vec3{27, 5, -9}, Quaternion{}});
    NetClient refused;
    CHECK(refused.UseWorld(refused_world));
    u32 applied = 0;
    refused.OnSnapshotApplied([&](World&) { ++applied; });
    cc.name = "full-refused";
    CHECK(refused.Start(cc, now(), &error));
    const f64 refuse_deadline = now() + 3.0;
    while (!refused.Done() && now() < refuse_deadline) {
        server.Tick(now());
        leaving.Tick(now());
        remaining.Tick(now());
        refused.Tick(now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(refused.Done() && !refused.Result().joined && refused.Result().join_result == "Full");
    CHECK(refused.Result().snapshots == 0 && refused.Result().entities_seen == 0 && applied == 0);
    CHECK(&refused.GetWorld() == &refused_world && refused_world.EntityCount() == 1);
    CHECK(refused_world.IsAlive(preserved_refused_entity));
    CHECK(server.Stats().players == 2 && server.Stats().refused == 1);

    const f64 leave_deadline = now() + 5.0;
    while ((!leaving.Done() || server.Stats().players != 1) && now() < leave_deadline) {
        server.Tick(now());
        leaving.Tick(now());
        remaining.Tick(now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(leaving.Done() && remaining.Result().joined && !remaining.Done());
    CHECK(server.Stats().players == 1 && server.Stats().joins == 2 &&
          server.Stats().leaves == 1 && server.Stats().replicated_entities == 2);
    CHECK(world.EntityCount() == 2 && world.IsAlive(scene));

    // Counts alone can conceal a survivor pawn being destroyed and replaced.
    // Remember its authoritative owner and replication identity before churn.
    u32 survivor_net_id = 0;
    net::PeerId survivor_owner = net::kNoPeer;
    u32 survivor_pawns = 0;
    // ServerPlayer is reflected in dedicated_server.cpp, not in this test TU.
    // Inspect its network archetype instead of instantiating an unreflected
    // GetComponentId<ServerPlayer>() here (which breaks RPC registration).
    world.ForEach<net::NetIdentity>([&](net::NetIdentity& identity) {
        if (std::string(identity.archetype) != kPlayerArchetype) return;
        ++survivor_pawns;
        survivor_net_id = identity.net_id;
        survivor_owner = identity.owner;
    });
    CHECK(survivor_pawns == 1 && survivor_net_id != 0 && survivor_owner != net::kNoPeer);
    const auto check_survivor_identity = [&]() {
        u32 pawn_count = 0;
        u32 matching = 0;
        world.ForEach<net::NetIdentity>([&](net::NetIdentity& identity) {
            if (std::string(identity.archetype) != kPlayerArchetype) return;
            ++pawn_count;
            if (identity.net_id == survivor_net_id && identity.owner == survivor_owner) ++matching;
        });
        CHECK(pawn_count == 1 && matching == 1);
    };

    // Counts after a replacement leaves cannot detect temporary pawn
    // reassignment while two clients are connected. Inspect the live
    // authoritative identities without instantiating ServerPlayer here.
    const auto two_live_pawns_keep_survivor_identity = [&]() {
        u32 pawns = 0;
        u32 survivor_matches = 0;
        u32 replacement_matches = 0;
        world.ForEach<net::NetIdentity>([&](net::NetIdentity& identity) {
            if (std::string(identity.archetype) != kPlayerArchetype) return;
            ++pawns;
            if (identity.net_id == survivor_net_id && identity.owner == survivor_owner) {
                ++survivor_matches;
            } else if (identity.net_id != 0 && identity.net_id != survivor_net_id &&
                       identity.owner != net::kNoPeer && identity.owner != survivor_owner) {
                ++replacement_matches;
            }
        });
        return pawns == 2 && survivor_matches == 1 && replacement_matches == 1;
    };

    // The replication server allocates monotonically increasing IDs. A
    // vacated player slot must not alias an earlier replacement pawn in
    // delayed snapshots or a surviving client's replication history.
    const auto live_replacement_net_id = [&]() {
        u32 id = 0;
        world.ForEach<net::NetIdentity>([&](net::NetIdentity& identity) {
            if (std::string(identity.archetype) == kPlayerArchetype &&
                identity.owner != survivor_owner) id = identity.net_id;
        });
        return id;
    };
    const auto record_replacement_net_id = [&](u32& recorded, bool& stable) {
        const u32 current = live_replacement_net_id();
        stable &= current != 0 && (recorded == 0 || recorded == current);
        if (recorded == 0) recorded = current;
    };
    // Each new UDP session must have its own owner PeerId even when it
    // occupies the same player slot as a departed replacement.
    const auto live_replacement_owner = [&]() {
        net::PeerId owner = net::kNoPeer;
        world.ForEach<net::NetIdentity>([&](net::NetIdentity& identity) {
            if (std::string(identity.archetype) == kPlayerArchetype &&
                identity.owner != survivor_owner) owner = identity.owner;
        });
        return owner;
    };
    const auto record_replacement_owner = [&](net::PeerId& recorded, bool& stable) {
        const net::PeerId current = live_replacement_owner();
        stable &= current != net::kNoPeer && (recorded == net::kNoPeer || recorded == current);
        if (recorded == net::kNoPeer) recorded = current;
    };

    // A live replacement client must see the same authoritative pawn
    // identity as the server, not a stale pawn from a reused capacity slot.
    const auto client_owns_authoritative_pawn = [&](NetClient& client, u32 net_id, net::PeerId owner) {
        u32 owned_pawns = 0;
        u32 matching = 0;
        client.GetWorld().ForEach<net::NetIdentity>([&](net::NetIdentity& identity) {
            if (std::string(identity.archetype) != kPlayerArchetype || !identity.locally_owned) return;
            ++owned_pawns;
            if (identity.net_id == net_id && identity.owner == owner) ++matching;
        });
        return net_id != 0 && owner != net::kNoPeer && owned_pawns == 1 && matching == 1;
    };

    // A replacement must also receive the surviving player's authoritative
    // pawn as a remotely owned entity, without duplicates or ghost pawns.
    const auto replacement_sees_remote_survivor = [&](NetClient& client) {
        u32 player_pawns = 0;
        u32 remote_survivors = 0;
        client.GetWorld().ForEach<net::NetIdentity>([&](net::NetIdentity& identity) {
            if (std::string(identity.archetype) != kPlayerArchetype) return;
            ++player_pawns;
            if (identity.net_id == survivor_net_id && identity.owner == survivor_owner &&
                !identity.locally_owned) ++remote_survivors;
        });
        return player_pawns == 2 && remote_survivors == 1;
    };

    // The surviving client must also replicate each live replacement as a
    // remotely owned pawn with the server's identity, never a stale slot occupant.
    const auto survivor_sees_remote_replacement = [&](u32 net_id, net::PeerId owner) {
        if (net_id == 0 || owner == net::kNoPeer) return false;
        u32 player_pawns = 0;
        u32 remote_replacements = 0;
        remaining.GetWorld().ForEach<net::NetIdentity>([&](net::NetIdentity& identity) {
            if (std::string(identity.archetype) != kPlayerArchetype) return;
            ++player_pawns;
            if (identity.net_id == net_id && identity.owner == owner &&
                !identity.locally_owned) ++remote_replacements;
        });
        return player_pawns == 2 && remote_replacements == 1;
    };

    // Every replacement session must replicate the same static scene entity,
    // with the server's identity and transform, despite reused player slots.
    const auto replacement_sees_authoritative_scene = [&](NetClient& client) {
        const auto* server_identity = world.GetComponent<net::NetIdentity>(scene);
        if (!server_identity || server_identity->net_id == 0 ||
            server_identity->owner != net::kNoPeer) return false;
        u32 scenes = 0;
        u32 matches = 0;
        client.GetWorld().ForEach<net::NetIdentity, Transform>(
            [&](net::NetIdentity& identity, Transform& transform) {
                if (std::string(identity.archetype) != kSceneArchetype) return;
                ++scenes;
                if (identity.net_id == server_identity->net_id &&
                    identity.owner == net::kNoPeer && !identity.locally_owned &&
                    transform.position.x == 12.0f && transform.position.y == 4.0f &&
                    transform.position.z == -6.0f) ++matches;
            });
        return scenes == 1 && matches == 1;
    };

    // A departed replacement must disappear from the surviving client's
    // replicated World, not merely from the authoritative server's World.
    const auto survivor_client_has_no_ghost_pawn = [&]() {
        u32 player_pawns = 0;
        u32 matching = 0;
        remaining.GetWorld().ForEach<net::NetIdentity>([&](net::NetIdentity& identity) {
            if (std::string(identity.archetype) != kPlayerArchetype) return;
            ++player_pawns;
            if (identity.net_id == survivor_net_id && identity.owner == survivor_owner &&
                identity.locally_owned) ++matching;
        });
        return player_pawns == 1 && matching == 1;
    };

    // A previously Full-refused client is terminal, not an implicit
    // reconnect candidate when the other player remains active.
    error.clear();
    CHECK(!refused.Start(cc, now(), &error));
    CHECK(error.find("fresh client") != std::string::npos);
    CHECK(refused.Done() && !refused.Result().joined && refused.Result().join_result == "Full");
    CHECK(&refused.GetWorld() == &refused_world && refused_world.EntityCount() == 1);
    CHECK(refused_world.IsAlive(preserved_refused_entity) && refused.Result().snapshots == 0 && applied == 0);
    const Transform* untouched = refused_world.GetComponent<Transform>(preserved_refused_entity);
    CHECK(untouched != nullptr);
    if (untouched) CHECK(untouched->position.x == 27.0f &&
                         untouched->position.y == 5.0f && untouched->position.z == -9.0f);

    // The surviving player must keep receiving authoritative snapshots while
    // a new peer fills the vacated slot; a join alone is not sufficient.
    const u32 remaining_snapshots_before_replacement = remaining.Result().snapshots;
    NetClient fresh;
    bool first_occupied = false;
    bool first_identity_stable = true;
    u32 first_replacement_net_id = 0;
    bool first_replacement_id_stable = true;
    net::PeerId first_replacement_owner = net::kNoPeer;
    bool first_replacement_owner_stable = true;
    bool first_client_identity_stable = true;
    bool first_survivor_client_identity_stable = true;
    bool first_replacement_saw_remote_survivor = false;
    bool first_replacement_saw_scene = false;
    bool survivor_saw_first_remote_replacement = false;
    cc.name = "replacement";
    cc.duration = 1.0;
    CHECK(fresh.Start(cc, now(), &error));
    const f64 fresh_deadline = now() + 5.0;
    while (!fresh.Done() && now() < fresh_deadline) {
        server.Tick(now());
        remaining.Tick(now());
        fresh.Tick(now());
        // The join event creates the pawn before the next replication tick
        // assigns its nonzero net_id. Check after the client sees that pawn.
        if (fresh.Result().pawn_seen && server.Stats().players == 2 && world.EntityCount() == 3) {
            first_occupied = true;
            first_identity_stable &= two_live_pawns_keep_survivor_identity();
            record_replacement_net_id(first_replacement_net_id, first_replacement_id_stable);
            record_replacement_owner(first_replacement_owner, first_replacement_owner_stable);
            first_client_identity_stable &= client_owns_authoritative_pawn(
                fresh, first_replacement_net_id, first_replacement_owner);
            first_survivor_client_identity_stable &= client_owns_authoritative_pawn(
                remaining, survivor_net_id, survivor_owner);
            first_replacement_saw_remote_survivor |= replacement_sees_remote_survivor(fresh);
            first_replacement_saw_scene |= replacement_sees_authoritative_scene(fresh);
            survivor_saw_first_remote_replacement |= survivor_sees_remote_replacement(
                first_replacement_net_id, first_replacement_owner);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(fresh.Done() && fresh.Result().joined && fresh.Result().pawn_seen);
    CHECK(first_occupied && first_identity_stable);
    CHECK(first_replacement_id_stable && first_replacement_net_id != 0);
    CHECK(first_replacement_owner_stable && first_replacement_owner != net::kNoPeer &&
          first_replacement_owner != survivor_owner);
    CHECK(first_client_identity_stable);
    CHECK(first_survivor_client_identity_stable);
    CHECK(first_replacement_saw_remote_survivor);
    CHECK(first_replacement_saw_scene);
    CHECK(survivor_saw_first_remote_replacement);
    CHECK(fresh.Result().snapshots > 0 && fresh.Result().entities_seen >= 3);
    CHECK(remaining.Result().joined && !remaining.Done());
    CHECK(remaining.Result().pawn_seen &&
          remaining.Result().snapshots > remaining_snapshots_before_replacement);
    for (int i = 0; i < 200 && server.Stats().players != 1; ++i) {
        server.Tick(now());
        remaining.Tick(now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(server.Stats().players == 1 && server.Stats().joins == 3 &&
          server.Stats().leaves == 2 && server.Stats().peak_players == 2 &&
          server.Stats().refused == 1);
    CHECK(world.EntityCount() == 2 && world.IsAlive(scene));
    check_survivor_identity();

    // Releasing the replacement must not stall the surviving player's
    // authoritative snapshot stream after capacity drops back to one.
    const u32 snapshots_after_replacement_leave = remaining.Result().snapshots;
    const f64 survivor_deadline = now() + 1.0;
    while (remaining.Result().snapshots <= snapshots_after_replacement_leave &&
           now() < survivor_deadline) {
        server.Tick(now());
        remaining.Tick(now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(remaining.Result().joined && !remaining.Done());
    CHECK(remaining.Result().snapshots > snapshots_after_replacement_leave);
    CHECK(server.Stats().players == 1 && world.EntityCount() == 2);
    const f64 first_cleanup_deadline = now() + 1.0;
    while (!survivor_client_has_no_ghost_pawn() && now() < first_cleanup_deadline) {
        server.Tick(now());
        remaining.Tick(now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(survivor_client_has_no_ghost_pawn());
    CHECK(replacement_sees_authoritative_scene(remaining));

    // A second fresh session must reuse the same vacated capacity without
    // evicting the original survivor or leaving a ghost pawn behind.
    const u32 snapshots_before_second_replacement = remaining.Result().snapshots;
    NetClient second_replacement;
    cc.name = "replacement-again";
    cc.duration = 0.8;
    CHECK(second_replacement.Start(cc, now(), &error));
    bool second_slot_occupied = false;
    bool second_identity_stable = true;
    u32 second_replacement_net_id = 0;
    bool second_replacement_id_stable = true;
    net::PeerId second_replacement_owner = net::kNoPeer;
    bool second_replacement_owner_stable = true;
    bool second_client_identity_stable = true;
    bool second_survivor_client_identity_stable = true;
    bool second_replacement_saw_remote_survivor = false;
    bool second_replacement_saw_scene = false;
    bool survivor_saw_second_remote_replacement = false;
    const f64 second_deadline = now() + 4.0;
    while (!second_replacement.Done() && now() < second_deadline) {
        server.Tick(now());
        remaining.Tick(now());
        second_replacement.Tick(now());
        if (second_replacement.Result().pawn_seen && server.Stats().players == 2 && world.EntityCount() == 3) {
            second_slot_occupied = true;
            second_identity_stable &= two_live_pawns_keep_survivor_identity();
            record_replacement_net_id(second_replacement_net_id, second_replacement_id_stable);
            record_replacement_owner(second_replacement_owner, second_replacement_owner_stable);
            second_client_identity_stable &= client_owns_authoritative_pawn(
                second_replacement, second_replacement_net_id, second_replacement_owner);
            second_survivor_client_identity_stable &= client_owns_authoritative_pawn(
                remaining, survivor_net_id, survivor_owner);
            second_replacement_saw_remote_survivor |= replacement_sees_remote_survivor(second_replacement);
            second_replacement_saw_scene |= replacement_sees_authoritative_scene(second_replacement);
            survivor_saw_second_remote_replacement |= survivor_sees_remote_replacement(
                second_replacement_net_id, second_replacement_owner);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(second_replacement.Done() && second_replacement.Result().joined);
    CHECK(second_replacement.Result().pawn_seen && second_replacement.Result().snapshots > 0);
    CHECK(second_slot_occupied && second_identity_stable);
    CHECK(second_replacement_id_stable && second_replacement_net_id != 0 &&
          second_replacement_net_id != first_replacement_net_id);
    CHECK(second_replacement_owner_stable && second_replacement_owner != net::kNoPeer &&
          second_replacement_owner != survivor_owner &&
          second_replacement_owner != first_replacement_owner);
    CHECK(second_client_identity_stable);
    CHECK(second_survivor_client_identity_stable);
    CHECK(second_replacement_saw_remote_survivor);
    CHECK(second_replacement_saw_scene);
    CHECK(survivor_saw_second_remote_replacement);
    CHECK(remaining.Result().joined && !remaining.Done());
    CHECK(remaining.Result().snapshots > snapshots_before_second_replacement);
    for (int i = 0; i < 200 && server.Stats().players != 1; ++i) {
        server.Tick(now());
        remaining.Tick(now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(server.Stats().players == 1 && server.Stats().joins == 4 &&
          server.Stats().leaves == 3 && server.Stats().peak_players == 2 &&
          server.Stats().refused == 1 && server.Stats().replicated_entities == 2);
    CHECK(world.EntityCount() == 2 && world.IsAlive(scene));
    check_survivor_identity();
    const f64 second_cleanup_deadline = now() + 1.0;
    while (!survivor_client_has_no_ghost_pawn() && now() < second_cleanup_deadline) {
        server.Tick(now());
        remaining.Tick(now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(survivor_client_has_no_ghost_pawn());
    CHECK(replacement_sees_authoritative_scene(remaining));

    // Repeated slot reuse must not stop the survivor's authoritative
    // snapshot stream after the second replacement disconnects.
    const u32 snapshots_after_second_leave = remaining.Result().snapshots;
    const f64 second_survivor_deadline = now() + 1.0;
    while (remaining.Result().snapshots <= snapshots_after_second_leave &&
           now() < second_survivor_deadline) {
        server.Tick(now());
        remaining.Tick(now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(remaining.Result().joined && !remaining.Done());
    CHECK(remaining.Result().snapshots > snapshots_after_second_leave);
    CHECK(client_owns_authoritative_pawn(remaining, survivor_net_id, survivor_owner));

    // Completed replacement sessions must not silently restart in-place:
    // their old UDP/replication state requires a fresh NetClient instance.
    error.clear();
    CHECK(!fresh.Start(cc, now(), &error));
    CHECK(error.find("fresh client") != std::string::npos);
    error.clear();
    CHECK(!second_replacement.Start(cc, now(), &error));
    CHECK(error.find("fresh client") != std::string::npos);
    CHECK(fresh.Done() && fresh.Result().joined);
    CHECK(second_replacement.Done() && second_replacement.Result().joined);
    CHECK(remaining.Result().joined && !remaining.Done());
    CHECK(server.Stats().players == 1 && server.Stats().joins == 4);

    // Rejected in-place restarts must not interrupt the surviving peer's
    // snapshot stream or create a hidden join, leave, or refusal event.
    const u32 snapshots_after_rejected_restarts = remaining.Result().snapshots;
    const f64 restart_rejection_deadline = now() + 1.0;
    while (remaining.Result().snapshots <= snapshots_after_rejected_restarts &&
           now() < restart_rejection_deadline) {
        server.Tick(now());
        remaining.Tick(now());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(remaining.Result().joined && !remaining.Done());
    CHECK(remaining.Result().snapshots > snapshots_after_rejected_restarts);
    CHECK(client_owns_authoritative_pawn(remaining, survivor_net_id, survivor_owner));
    CHECK(server.Stats().players == 1 && server.Stats().joins == 4 &&
          server.Stats().leaves == 3 && server.Stats().refused == 1);
    CHECK(world.EntityCount() == 2 && survivor_client_has_no_ghost_pawn());

    // Slot churn and rejected restarts must preserve the replicated scene
    // entity, its server identity, and its authoritative static transform.
    const net::NetIdentity* server_scene_identity = world.GetComponent<net::NetIdentity>(scene);
    CHECK(server_scene_identity != nullptr && server_scene_identity->net_id != 0 &&
          server_scene_identity->owner == net::kNoPeer);
    u32 client_scene_matches = 0;
    u32 client_scene_entities = 0;
    if (server_scene_identity) {
        remaining.GetWorld().ForEach<net::NetIdentity, Transform>(
            [&](net::NetIdentity& identity, Transform& transform) {
                if (std::string(identity.archetype) == kSceneArchetype) ++client_scene_entities;
                if (identity.net_id != server_scene_identity->net_id) return;
                ++client_scene_matches;
                CHECK(std::string(identity.archetype) == kSceneArchetype);
                CHECK(identity.owner == net::kNoPeer && !identity.locally_owned);
                CHECK(transform.position.x == 12.0f &&
                      transform.position.y == 4.0f && transform.position.z == -6.0f);
            });
    }
    CHECK(client_scene_matches == 1 && client_scene_entities == 1);
    // Shutdown may remove session-owned pawns, but must not strip or reassign
    // the game-owned scene's authoritative network identity.
    const u32 scene_net_id_before_stop = server_scene_identity ? server_scene_identity->net_id : 0;
    server.Stop();
    CHECK(!server.Running());
    CHECK(server.Stats().players == 0 && server.Stats().peak_players == 2 &&
          server.Stats().joins == 4 && server.Stats().leaves == 3 && server.Stats().refused == 1);
    CHECK(world.EntityCount() == 1 && world.IsAlive(scene));
    const auto* scene_identity_after_stop = world.GetComponent<net::NetIdentity>(scene);
    CHECK(scene_identity_after_stop != nullptr);
    if (scene_identity_after_stop)
        CHECK(scene_identity_after_stop->net_id == scene_net_id_before_stop &&
              scene_identity_after_stop->owner == net::kNoPeer);
    const Transform* preserved = world.GetComponent<Transform>(scene);
    CHECK(preserved != nullptr);
    if (preserved) CHECK(preserved->position.x == 12.0f &&
                         preserved->position.y == 4.0f && preserved->position.z == -6.0f);
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
    // Token refusals must not apply even an early snapshot to a game-owned World.
    World refused_world;
    const Entity preserved = refused_world.CreateEntity(Transform{Vec3{17, 6, -9}, Quaternion{}});
    u32 refused_snapshots = 0;
    auto make = [&](const char* name, const std::string& token, World* external = nullptr) {
        NetClientConfig cc;
        cc.server = net::Address::Loopback(server.Port());
        cc.name = name;
        cc.token = token;
        cc.build_id = 89;
        cc.duration = 1.0;
        cc.join_timeout = 3.0;
        auto c = std::make_unique<NetClient>();
        if (external) {
            CHECK(c->UseWorld(*external));
            c->OnSnapshotApplied([&](World&) { ++refused_snapshots; });
        }
        CHECK(c->Start(cc, now(), &error));
        return c;
    };
    std::vector<std::unique_ptr<NetClient>> clients;
    clients.push_back(make("good", net::IssueJoinToken(config.token_secret, {"acct-good", unix_now + 60, 89})));
    clients.push_back(make("expired", net::IssueJoinToken(config.token_secret, {"acct-old", unix_now - 5, 89}), &refused_world));
    clients.push_back(make("forged", net::IssueJoinToken("some-other-issuer-secret", {"acct-x", unix_now + 60, 89}), &refused_world));
    clients.push_back(make("build", net::IssueJoinToken(config.token_secret, {"acct-b", unix_now + 60, 88}), &refused_world));
    clients.push_back(make("none", "", &refused_world));
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
    for (usize i = 1; i < clients.size(); ++i) {
        CHECK(!clients[i]->Result().joined && clients[i]->Result().entities_seen == 0);
        CHECK(clients[i]->Result().snapshots == 0 && &clients[i]->GetWorld() == &refused_world);
        CHECK(!clients[i]->UseWorld(world)); // a refused session cannot rebind the caller's World
    }
    CHECK(refused_snapshots == 0);
    CHECK(refused_world.EntityCount() == 1 && refused_world.IsAlive(preserved));
    const Transform* original = refused_world.GetComponent<Transform>(preserved);
    CHECK(original != nullptr);
    if (original) CHECK(original->position.x == 17.0f && original->position.y == 6.0f &&
                        original->position.z == -9.0f);
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
