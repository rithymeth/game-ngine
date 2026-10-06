#include "aether/assets/asset_guid.h"
#include "aether/blueprint/compiler.h"
#include "aether/blueprint/vm.h"
#include "aether/pak/pak.h"
#include "aether/player/game.h"
#include "aether/player/settings_apply.h"
#include "aether/player/user_paths.h"
#include "aether/reflection/serialize.h"
#include "aether/save/save_bag.h"
#include "aether/save/world_state.h"
#include "aether/scene/components.h"
#include "aether/scene/entity_guid.h"
#include "test_framework.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <filesystem>

// Phase 28 step 6 (§28.7): the player's user folders, its SaveSystem and
// world context, the Player.Save stage, and applying settings.

using namespace aether;
using namespace aether::player;
using nlohmann::json;
namespace stdfs = std::filesystem;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

stdfs::path TestDir(const std::string& name) {
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_player_save_tests" / name;
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    return dir;
}

// A cooked game whose scene has one saveable entity (a door's position).
stdfs::path MakeGame(const stdfs::path& dir) {
    SaveableEntity saveable;
    saveable.tag = "door";
    saveable.fields = {"Transform.position"};
    const json scene = {{"$type", "Scene"},
                        {"$version", 1},
                        {"entities", json::array({{{"guid", ToString(EntityGuid{0xD00A, 7})},
                                                   {"components", {{"Transform", reflect::ToJson(Transform{Vec3(1, 2, 3), Quaternion::Identity()})},
                                                                   {"SaveableEntity", reflect::ToJson(saveable)}}}}})}};
    const json manifest = {{"$type", "CookManifest"},
                           {"$version", 1},
                           {"project", "Demo"},
                           {"configuration", "Development"},
                           {"startup_scene", "Scenes/start.ascene"},
                           {"fixed_timestep_hz", 60.0},
                           {"gravity", {0.0, -9.81, 0.0}},
                           {"layers", {"Default"}},
                           {"collision_matrix", json::array()},
                           {"assets", json::array({{{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", "Scenes/start.ascene"}, {"importer", "Scene"}}})},
                           {"files", json::array()}};
    pak::PakWriter writer;
    writer.Add("Manifest.json", manifest.dump(2));
    writer.Add("Content/Scenes/start.ascene", scene.dump(2));
    const stdfs::path file = dir / "Game.apak";
    std::string error;
    writer.Write(file.string(), &error);
    return file;
}

} // namespace

AETHER_TEST(PlayerSave_UserPathsPrecedenceAndSanitizing) {
    CHECK(SanitizeProjectFolder("My Game: 2") == "My_Game__2");
    CHECK(SanitizeProjectFolder("../../etc") == "______etc");
    CHECK(SanitizeProjectFolder("") == "Game");
    CHECK(SanitizeProjectFolder("...") == "Game");
    const UserPaths given = ResolveUserPaths("Demo", "/tmp/aether_given");
    CHECK(given.root == stdfs::path("/tmp/aether_given"));
    CHECK(given.saves == stdfs::path("/tmp/aether_given/Saves") && given.settings == stdfs::path("/tmp/aether_given/Config"));
    const UserPaths system = ResolveUserPaths("../Demo");
    CHECK(!system.root.empty());
    CHECK(system.root.filename() == "___Demo" || std::getenv("AETHER_USER_DIR") != nullptr);
    CHECK(system.root.string().find("..") == std::string::npos || std::getenv("AETHER_USER_DIR") != nullptr);
}

AETHER_TEST(PlayerSave_WithoutPathsThereIsNoSaveSystem) {
    const stdfs::path dir = TestDir("none");
    GamePackage package;
    std::string error;
    CHECK(package.Mount(MakeGame(dir).string(), 0, &error) && package.LoadManifest(&error));
    Game game(package);
    CHECK(game.Saves() == nullptr);
    CHECK(game.LoadStartupScene(&error));
    game.BeginPlay();
    game.Tick(0.016f); // the Player.Save stage does nothing
}

AETHER_TEST(PlayerSave_ContextFollowsTheScene) {
    const stdfs::path dir = TestDir("context");
    GamePackage package;
    std::string error;
    CHECK(package.Mount(MakeGame(dir).string(), 0, &error) && package.LoadManifest(&error));
    {
        Game game(package);
        game.SetUserPaths(ResolveUserPaths("Demo", dir / "user"));
        CHECK(game.Saves() != nullptr && save::SaveSystem::Active() == game.Saves());
        CHECK(game.LoadStartupScene(&error));
        const save::SaveSystem::WorldContext ctx = game.Saves()->GetWorldContext();
        CHECK(ctx.world == &game.GetWorld() && ctx.guids == &game.Guids() && ctx.lifecycle == &game.GetLifecycle() && ctx.tracker != nullptr);
        CHECK(ctx.tracker->Baseline().size() == 1);
        const save::WorldSnapshot snap = save::CaptureWorld(*ctx.world, *ctx.guids, ctx.tracker);
        CHECK(snap.entities.size() == 1 && snap.destroyed.empty());
        CHECK(game.LoadScene("Scenes/start.ascene", &error)); // a reload: the context is the new world's
        CHECK(game.Saves()->GetWorldContext().world == &game.GetWorld());
        // A whole-world save from the player's own SaveSystem.
        save::SaveBag bag;
        bag.SetInt("coins", 3);
        bag.world = save::CaptureWorld(game.GetWorld(), game.Guids(), game.Saves()->GetWorldContext().tracker);
    }
    // The game is gone and so is the active system: nothing dangles.
    CHECK(save::SaveSystem::Active() == nullptr);
}

AETHER_TEST(PlayerSave_AsyncSaveFinishesOnATick) {
    const stdfs::path dir = TestDir("async");
    GamePackage package;
    std::string error;
    CHECK(package.Mount(MakeGame(dir).string(), 0, &error) && package.LoadManifest(&error));
    Game game(package);
    game.SetUserPaths(ResolveUserPaths("Demo", dir / "user"));
    CHECK(game.LoadStartupScene(&error));
    game.BeginPlay();
    save::SaveBag bag;
    bag.SetInt("coins", 9);
    bool done = false, ok = false;
    game.Saves()->SaveAsync("auto", bag, [&](const save::SaveResult& r) {
        done = true;
        ok = r.ok;
    });
    game.Saves()->Flush();
    CHECK(!done); // delivered by Pump, on the main thread
    game.Tick(0.016f);
    CHECK(done && ok);
    save::SaveBag back;
    CHECK(game.Saves()->Load("auto", back).ok && back.GetInt("coins") == 9);
    CHECK(stdfs::exists(dir / "user" / "Saves" / "auto.asav"));
}

AETHER_TEST(PlayerSave_FinishedSavesReachBlueprints) {
    // The Player.Save stage uses DispatchAll: every instance with the event runs it.
    bp::Blueprint blueprint;
    bp::Graph events;
    events.name = "EventGraph";
    blueprint.graphs.push_back(events);
    bp::GraphBuilder b(*blueprint.FindGraph("EventGraph"));
    const bp::NodeId finished = b.Add("Event.OnSaveFinished"), print = b.Add("Debug.Print");
    b.Connect(finished, "then", print, "exec").Connect(finished, "slot", print, "text");
    bp::CompileResult compiled = bp::CompileBlueprint(blueprint);
    CHECK(compiled.Ok());
    World world;
    bp::BlueprintVM vm(world);
    std::vector<std::string> printed;
    vm.SetPrintHandler([&](Entity, const std::string& text) { printed.push_back(text); });
    const Entity a = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
    const Entity c = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
    const Entity none = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
    (void)none;
    CHECK(vm.Attach(a, compiled.blueprint) && vm.Attach(c, compiled.blueprint));
    const bp::VmValue args[] = {std::string("slot3"), true};
    CHECK(vm.DispatchAll("Event.OnSaveFinished", args) == 2);
    CHECK(printed.size() == 2 && printed[0] == "slot3" && printed[1] == "slot3");
    CHECK(vm.DispatchAll("Event.Custom:Nothing") == 0);
}

AETHER_TEST(PlayerSave_ApplySettingsPushesVolumesQualityAndWindow) {
    save::GameSettings s;
    s.master = 0.5f;
    s.music = 1.0f;
    s.sfx = 0.0f;
    s.quality = "Low";
    s.width = 800;
    s.height = 600;
    std::vector<std::pair<std::string, f32>> buses;
    std::string quality;
    u32 w = 0, h = 0;
    SettingsTargets t;
    t.set_bus_volume_db = [&](const std::string& bus, f32 db) { buses.emplace_back(bus, db); };
    t.set_quality = [&](const std::string& q) { quality = q; };
    t.set_window = [&](u32 ww, u32 hh, bool, bool) {
        w = ww;
        h = hh;
    };
    CHECK(ApplySettings(s, t).size() == 6); // four buses, quality, window
    CHECK(buses.size() == 4 && buses[0].first == "Master" && std::fabs(buses[0].second - save::BusVolumeDb(0.5f)) < 1e-4f);
    CHECK(buses[1].second == 0.0f && buses[2].second == -80.0f);
    CHECK(quality == "Low" && w == 800 && h == 600);
    // Only what changed.
    save::GameSettings next = s;
    next.sfx = 1.0f;
    buses.clear();
    const auto done = ApplySettings(next, t, &s);
    CHECK(done.size() == 1 && buses.size() == 1 && buses[0].first == "SFX");
    CHECK(ApplySettings(s, t, &s).empty());
    CHECK(ApplySettings(s, SettingsTargets{}).empty()); // no hooks, nothing to do
}

AETHER_TEST(PlayerSave_LoadSettingsAppliesAndObserves) {
    const stdfs::path dir = TestDir("settings");
    GamePackage package;
    std::string error;
    CHECK(package.Mount(MakeGame(dir).string(), 0, &error) && package.LoadManifest(&error));
    const UserPaths paths = ResolveUserPaths("Demo", dir / "user");
    {
        Game game(package);
        game.SetUserPaths(paths);
        save::GameSettings s;
        s.music = 0.25f;
        s.quality = "Medium";
        game.Settings().Set(s);
        CHECK(game.Settings().Save().ok);
    }
    Game game(package);
    game.SetUserPaths(paths);
    std::vector<std::pair<std::string, f32>> buses;
    std::string quality;
    SettingsTargets t;
    t.set_bus_volume_db = [&](const std::string& bus, f32 db) { buses.emplace_back(bus, db); };
    t.set_quality = [&](const std::string& q) { quality = q; };
    CHECK(game.LoadSettings(t).empty());
    CHECK(game.Settings().Get().music == 0.25f && quality == "Medium" && buses.size() == 4);
    buses.clear();
    save::GameSettings changed = game.Settings().Get();
    changed.voice = 0.5f;
    CHECK(game.Settings().Set(changed));
    CHECK(buses.size() == 1 && buses[0].first == "Voice"); // later changes apply too
}

// ---- Phase 28 step 7: the old rebinds move into the settings ----

#include "aether/input/bindings.h"
#include "aether/player/bindings_migrate.h"

namespace {

void WriteLegacy(const stdfs::path& root, const std::string& text) {
    const stdfs::path file = input::UserBindingsPath(root);
    stdfs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

} // namespace

AETHER_TEST(PlayerSave_MigratesLegacyBindingsOnce) {
    const stdfs::path root = TestDir("migrate");
    input::UserBindings ub;
    ub.Set("Gameplay", "Jump", 0, input::Key::K);
    std::string error;
    stdfs::create_directories(input::UserBindingsPath(root).parent_path());
    CHECK(input::SaveUserBindings(ub, input::UserBindingsPath(root), &error));
    save::SettingsStore<save::GameSettings> store(root / "Config");
    CHECK(MigrateLegacyBindings(root, store).empty());
    CHECK(store.Get().bindings.overrides.size() == 1);
    CHECK(store.Get().bindings.Find("Gameplay", "Jump", 0) != nullptr);
    CHECK(!stdfs::exists(input::UserBindingsPath(root)));
    stdfs::path moved = input::UserBindingsPath(root);
    moved += ".migrated";
    CHECK(stdfs::exists(moved));
    CHECK(MigrateLegacyBindings(root, store).empty()); // a second run does nothing
    CHECK(store.Get().bindings.overrides.size() == 1);
}

AETHER_TEST(PlayerSave_MigrationLeavesADamagedFileAlone) {
    const stdfs::path root = TestDir("migrate_bad");
    WriteLegacy(root, "{ not json");
    save::SettingsStore<save::GameSettings> store(root / "Config");
    const auto warnings = MigrateLegacyBindings(root, store);
    CHECK(warnings.size() == 1);
    CHECK(store.Get().bindings.overrides.empty());
    CHECK(stdfs::exists(input::UserBindingsPath(root)));
}

AETHER_TEST(PlayerSave_SettingsBindingsWinOverTheOldFile) {
    const stdfs::path root = TestDir("migrate_wins");
    input::UserBindings ub;
    ub.Set("Gameplay", "Jump", 0, input::Key::K);
    std::string error;
    stdfs::create_directories(input::UserBindingsPath(root).parent_path());
    CHECK(input::SaveUserBindings(ub, input::UserBindingsPath(root), &error));
    save::SettingsStore<save::GameSettings> store(root / "Config");
    save::GameSettings s;
    s.bindings.Set("Gameplay", "Jump", 0, input::Key::J);
    store.Set(s);
    CHECK(MigrateLegacyBindings(root, store).size() == 1);
    CHECK(store.Get().bindings.Find("Gameplay", "Jump", 0)->key == input::Key::J);
    CHECK(!stdfs::exists(input::UserBindingsPath(root)));
}

AETHER_TEST(PlayerSave_NoOldFileNoMigration) {
    const stdfs::path root = TestDir("migrate_none");
    save::SettingsStore<save::GameSettings> store(root / "Config");
    CHECK(MigrateLegacyBindings(root, store).empty());
    CHECK(store.Get().bindings.overrides.empty());
}

#ifdef AETHER_TEST_PLAYER_PATH
namespace {
int RunPlayer(const stdfs::path& pak, const stdfs::path& user) {
    const std::string cmd = std::string("\"") + AETHER_TEST_PLAYER_PATH + "\" --headless --frames 3 --pak \"" + pak.string() + "\" --user-dir \"" +
                            user.string() + "\" > /dev/null 2>&1";
    return std::system(cmd.c_str());
}
} // namespace

AETHER_TEST(PlayerSave_PlayerExecutableLeavesSettingsAloneUnlessChanged) {
#if !defined(_WIN32)
    const stdfs::path dir = TestDir("exe");
    const stdfs::path pak = MakeGame(dir);
    const stdfs::path user = dir / "user";
    CHECK(RunPlayer(pak, user) == 0);
    CHECK(!stdfs::exists(user / "Config" / "settings.asettings")); // nothing changed: nothing written
    // An old rebinds file is moved into a new settings file, once.
    input::UserBindings ub;
    ub.Set("Gameplay", "Jump", 0, input::Key::K);
    std::string error;
    stdfs::create_directories(input::UserBindingsPath(user).parent_path());
    CHECK(input::SaveUserBindings(ub, input::UserBindingsPath(user), &error));
    CHECK(RunPlayer(pak, user) == 0);
    CHECK(stdfs::exists(user / "Config" / "settings.asettings"));
    CHECK(!stdfs::exists(input::UserBindingsPath(user)));
    save::SettingsStore<save::GameSettings> store(user / "Config");
    CHECK(store.Load().ok && store.Get().bindings.overrides.size() == 1);
    // A run that changes nothing leaves the file as it is.
    const auto written = stdfs::last_write_time(store.Path());
    CHECK(RunPlayer(pak, user) == 0);
    CHECK(stdfs::last_write_time(store.Path()) == written);
#endif
}
#endif
