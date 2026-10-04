#include "aether/assets/asset_guid.h"
#include "aether/cook/cooker.h"
#include "aether/pak/pak.h"
#include "aether/player/game.h"
#include "aether/project/project.h"
#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"
#include "aether/reflection/serialize.h"
#include "test_framework.h"

#if AETHER_TEST_PLAYER_PHYSICS
#include "aether/physics/components.h"
#endif

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

// Phase 25 step 4: the player's runtime - the manifest, mounting cooked
// archives, the startup scene with its prefabs, and the frame loop.

using namespace aether;
using namespace aether::player;
using nlohmann::json;
namespace stdfs = std::filesystem;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

stdfs::path TestDir(const std::string& name) {
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_player_tests" / name;
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    return dir;
}

json Manifest(const std::string& startup, const std::string& prefab_guid, f32 hz = 60.0f) {
    return {{"$type", "CookManifest"},
            {"$version", 1},
            {"project", "Demo"},
            {"configuration", "Development"},
            {"startup_scene", startup},
            {"fixed_timestep_hz", hz},
            {"gravity", {0.0, -9.81, 0.0}},
            {"layers", {"Default"}},
            {"collision_matrix", json::array()},
            {"assets", json::array({{{"guid", prefab_guid}, {"path", "Prefabs/crate.aprefab"}, {"importer", "Prefab"}},
                                    {{"guid", assets::ToString(assets::NewAssetGuid())},
                                     {"path", "Scenes/start.ascene"},
                                     {"importer", "Scene"}}})},
            {"files", json::array()}};
}

json Transform3(f32 x, f32 y, f32 z) {
    return reflect::ToJson(Transform{Vec3(x, y, z), Quaternion::Identity()});
}

json TagsJson(const std::string& tag) {
    Tags t;
    t.names = {tag};
    return reflect::ToJson(t);
}

// A pak: the startup scene has a tagged player and an instance of a crate
// prefab, whose one entity sits at (1, 2, 3).
stdfs::path MakePak(const stdfs::path& dir, const std::string& name, f32 hz = 60.0f, const std::string& player_tag = "Player") {
    const std::string prefab = assets::ToString(assets::NewAssetGuid());
    const json crate = {{"$type", "Prefab"},
                        {"$version", 1},
                        {"entities", json::array({{{"id", 1}, {"parent", 0}, {"components", {{"Transform", Transform3(1, 2, 3)}}}}})}};
    const json scene = {{"$type", "Scene"},
                        {"$version", 1},
                        {"entities", json::array({{{"components", {{"Transform", Transform3(0, 0, 0)},
                                                                   {"Tags", TagsJson(player_tag)}}}},
                                                  {{"components", {{"PrefabInstance", {{"source", prefab}}}}}}})}};
    pak::PakWriter writer;
    writer.Add("Manifest.json", Manifest("Scenes/start.ascene", prefab, hz).dump(2));
    writer.Add("Content/Scenes/start.ascene", scene.dump(2));
    writer.Add("Content/Prefabs/crate.aprefab", crate.dump(2));
    const stdfs::path file = dir / name;
    std::string error;
    AETHER_CHECK(writer.Write(file.string(), &error));
    return file;
}

Entity FindTagged(World& world, const std::string& tag) {
    const std::vector<Entity> all = FindEntitiesWithTag(world, tag);
    return all.empty() ? Entity{} : all.front();
}

} // namespace

AETHER_TEST(Player_ParsesTheManifest) {
    GameManifest m;
    std::string error;
    CHECK(ParseGameManifest(Manifest("Scenes/start.ascene", "g", 30.0f).dump(), m, &error));
    CHECK(m.project == "Demo" && m.configuration == cook::BuildConfiguration::Development);
    CHECK(m.startup_scene == "Scenes/start.ascene" && m.fixed_timestep_hz == 30.0f && m.gravity.y < -9.0f);
    CHECK(m.assets.size() == 2 && m.assets[0].path == "Prefabs/crate.aprefab" && m.layers.size() == 1);

    CHECK(!ParseGameManifest("not json", m, &error) && error.find("isn't JSON") != std::string::npos);
    json bad = Manifest("s", "g");
    bad["$type"] = "Scene";
    CHECK(!ParseGameManifest(bad.dump(), m, &error) && error.find("cook manifest") != std::string::npos);
    bad = Manifest("s", "g");
    bad["configuration"] = "Fast";
    CHECK(!ParseGameManifest(bad.dump(), m, &error) && error.find("configuration") != std::string::npos);
    bad = Manifest("s", "g", 0.0f);
    CHECK(!ParseGameManifest(bad.dump(), m, &error) && error.find("timestep") != std::string::npos);
    bad = Manifest("s", "g");
    bad["assets"].push_back({{"path", "x"}});
    CHECK(!ParseGameManifest(bad.dump(), m, &error) && error.find("no GUID") != std::string::npos);
}

AETHER_TEST(Player_RunsTheStartupSceneFromAPak) {
    const stdfs::path dir = TestDir("Run");
    const stdfs::path pak = MakePak(dir, "Game.apak");
    GamePackage package;
    std::string error;
    CHECK(package.Mount(pak.string(), 0, &error));
    CHECK(package.LoadManifest(&error) && package.Manifest().project == "Demo");
    CHECK(package.FindAsset("Prefabs/crate.aprefab") != nullptr && package.FindAsset("Nope") == nullptr);
    CHECK(GamePackage::FindPaks(dir) == std::vector<std::string>{pak.string()});

    Game game(package);
    CHECK(game.LoadStartupScene(&error));
    CHECK(game.SceneName() == "Scenes/start.ascene" && game.Warnings().empty());
    World& world = game.GetWorld();
    CHECK(!FindTagged(world, "Player").IsNull());
    // The crate prefab's entity came from the pak, at its prefab position.
    bool crate = false;
    world.ForEach<Transform>([&](const Transform& t) {
        crate |= std::fabs(t.position.x - 1) < 1e-4f && std::fabs(t.position.y - 2) < 1e-4f && std::fabs(t.position.z - 3) < 1e-4f;
    });
    CHECK(crate && game.Stats().prefab_instances == 1);

    // Lifecycle callbacks run in play; one second at 60 Hz is 60 fixed steps.
    int updates = 0, fixed = 0, started = 0;
    LifecycleCallbacks callbacks;
    callbacks.on_start = [&](Entity) { ++started; };
    callbacks.on_update = [&](Entity, f32) { ++updates; };
    callbacks.on_fixed_update = [&](Entity, f32) { ++fixed; };
    game.GetLifecycle().Register<Tags>(callbacks);
    game.BeginPlay();
    CHECK(game.IsPlaying() && started == 1);
    for (int i = 0; i < 60; ++i) game.Tick(1.0f / 60.0f);
    const GameStats s = game.Stats();
    CHECK(s.frames == 60 && s.fixed_steps == 60 && std::fabs(s.time - 1.0) < 1e-4);
    CHECK(updates == 60 && fixed == 60);
    game.EndPlay();
    CHECK(!game.IsPlaying());
}

AETHER_TEST(Player_FixedRateAndPatchPaks) {
    const stdfs::path dir = TestDir("Patch");
    // The base runs at 30 Hz; a later pak replaces the manifest and scene.
    const stdfs::path base = MakePak(dir, "0_Base.apak", 30.0f, "Player");
    GamePackage package;
    CHECK(package.Mount(base.string(), 0));
    Game game(package);
    CHECK(game.LoadStartupScene());
    game.BeginPlay();
    for (int i = 0; i < 60; ++i) game.Tick(1.0f / 60.0f);
    CHECK(game.Stats().fixed_steps == 30);

    const stdfs::path patch = MakePak(dir, "1_Patch.apak", 60.0f, "Hero");
    const std::vector<std::string> paks = GamePackage::FindPaks(dir);
    CHECK(paks.size() == 2 && paks[1] == patch.string());
    GamePackage patched;
    for (usize i = 0; i < paks.size(); ++i) CHECK(patched.Mount(paks[i], static_cast<int>(i)));
    Game game2(patched);
    CHECK(game2.LoadStartupScene());
    CHECK(!FindTagged(game2.GetWorld(), "Hero").IsNull() && FindTagged(game2.GetWorld(), "Player").IsNull());
}

AETHER_TEST(Player_Refusals) {
    std::string error;
    GamePackage empty;
    CHECK(!empty.LoadManifest(&error) && error.find("Nothing is mounted") != std::string::npos);
    Game no_manifest(empty);
    CHECK(!no_manifest.LoadStartupScene(&error));

    const stdfs::path dir = TestDir("Refuse");
    pak::PakWriter writer;
    writer.Add("Content/readme.txt", std::string("hi"));
    CHECK(writer.Write((dir / "NoManifest.apak").string()));
    GamePackage package;
    CHECK(package.Mount((dir / "NoManifest.apak").string()));
    CHECK(!package.LoadManifest(&error) && error.find("No Manifest.json") != std::string::npos);
    CHECK(!package.Mount((dir / "missing.apak").string(), 0, &error));

    GamePackage good;
    CHECK(good.Mount(MakePak(dir, "Game.apak").string()));
    Game game(good);
    CHECK(!game.LoadScene("Scenes/missing.ascene", &error) && error.find("Scenes/missing.ascene") != std::string::npos);
    CHECK(game.LoadStartupScene(&error));
}

AETHER_TEST(Player_PlaysWhatTheCookerCooked) {
    // A project cooked by aether_cook's library, then played from its pak.
    const stdfs::path dir = TestDir("Cooked");
    ProjectPaths paths;
    std::string error;
    CHECK(CreateProject(dir, "Cooked", &paths, &error));
    const json scene = {{"$type", "Scene"},
                        {"$version", 1},
                        {"entities", json::array({{{"components", {{"Transform", Transform3(4, 5, 6)},
                                                                   {"Tags", TagsJson("Spawn")}}}}})}};
    stdfs::create_directories(paths.content / "Scenes");
    std::ofstream(paths.content / "Scenes/main.ascene", std::ios::binary) << scene.dump(2);
    ProjectSettings settings;
    CHECK(LoadProject(paths.file, settings, &error));
    settings.startup_scene = "Scenes/main.ascene";
    settings.fixed_timestep_hz = 50.0f;
    CHECK(SaveProject(paths.file, settings, &error));

    cook::CookOptions options;
    options.project_file = paths.file;
    options.output_dir = dir / "Build";
    options.configuration = cook::BuildConfiguration::Shipping;
    const cook::CookReport report = cook::Cook(options);
    CHECK(report.ok);

    GamePackage package;
    for (const std::string& pak : GamePackage::FindPaks(dir / "Build")) CHECK(package.Mount(pak));
    CHECK(package.LoadManifest(&error));
    CHECK(package.Manifest().configuration == cook::BuildConfiguration::Shipping && package.Manifest().project == "Cooked");
    Game game(package);
    CHECK(game.LoadStartupScene(&error));
    const Entity spawn = FindTagged(game.GetWorld(), "Spawn");
    CHECK(!spawn.IsNull() && game.GetWorld().GetComponent<Transform>(spawn)->position.y == 5.0f);
    game.BeginPlay();
    for (int i = 0; i < 10; ++i) game.Tick(0.1f);
    CHECK(game.Stats().fixed_steps == 50); // 1 s at 50 Hz
}

#if AETHER_TEST_PLAYER_PHYSICS
AETHER_TEST(Player_SimulatesPhysics) {
    if (!Game::HasPhysics()) return; // a build without the physics module
    const stdfs::path dir = TestDir("Physics");
    // A ball 10 m up and a floor under it.
    RigidBody dynamic;
    RigidBody fixed;
    fixed.motion = BodyMotion::Static;
    SphereCollider sphere;
    sphere.radius = 0.5f;
    BoxCollider box;
    box.half_extents = Vec3(20, 0.5f, 20);
    const json ball = {{"Transform", Transform3(0, 10, 0)},
                       {"Tags", TagsJson("Ball")},
                       {"RigidBody", reflect::ToJson(dynamic)},
                       {"SphereCollider", reflect::ToJson(sphere)}};
    const json floor = {{"Transform", Transform3(0, -0.5f, 0)},
                        {"RigidBody", reflect::ToJson(fixed)},
                        {"BoxCollider", reflect::ToJson(box)}};
    const json scene = {{"$type", "Scene"},
                        {"$version", 1},
                        {"entities", json::array({{{"components", ball}}, {{"components", floor}}})}};
    pak::PakWriter writer;
    writer.Add("Manifest.json", Manifest("Scenes/start.ascene", assets::ToString(assets::NewAssetGuid())).dump());
    writer.Add("Content/Scenes/start.ascene", scene.dump());
    CHECK(writer.Write((dir / "Game.apak").string()));
    GamePackage package;
    CHECK(package.Mount((dir / "Game.apak").string()));
    Game game(package);
    CHECK(game.LoadStartupScene());
    game.BeginPlay();
    CHECK(game.Stats().physics_bodies == 2);
    const Entity e = FindTagged(game.GetWorld(), "Ball");
    for (int i = 0; i < 30; ++i) game.Tick(1.0f / 60.0f);
    const f32 half_second = game.GetWorld().GetComponent<Transform>(e)->position.y;
    CHECK(half_second < 9.0f && half_second > 8.0f); // ~10 - g t^2 / 2 = 8.77
    for (int i = 0; i < 240; ++i) game.Tick(1.0f / 60.0f);
    const f32 rest = game.GetWorld().GetComponent<Transform>(e)->position.y;
    CHECK(rest > 0.3f && rest < 0.7f); // on the floor
}
#endif
