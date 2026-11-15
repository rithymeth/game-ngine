#include "test_framework.h"

#ifdef AETHER_TEST_HAS_SPAWNER

#include "aether/assets/asset_guid.h"
#include "aether/ecs/world.h"
#include "aether/pak/pak.h"
#include "aether/player/game.h"
#include "aether/player/user_paths.h"
#include "aether/save/save_system.h"
#include "aether/scene/components.h"
#include "aether/scene/prefab.h"
#include "aether/spawner/spawner_kit.h"
#include "aether/spawner/spawner_system.h"
#if AETHER_TEST_HAS_SCRIPTING
#include "aether/script/script_system.h"
#endif

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <limits>

// The spawner kit: a Spawner component makes prefabs appear around an entity on a timer, up to a limit alive at once.

using namespace aether;
using namespace aether::spawn;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

bool Near(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// A world with a host that "spawns" by making an entity at the place it is asked for, and counts the calls.
struct Rig {
    World world;
    int calls = 0;
    bool host_works = true;
    std::vector<std::string> prefabs_asked;
    SpawnerSystem system{world, [this](const std::string& prefab, const Transform& place) {
                             ++calls;
                             prefabs_asked.push_back(prefab);
                             if (!host_works) return Entity{};
                             return world.CreateEntity(place);
                         }};

    Entity MakeSpawner(Spawner s, Vec3 at = Vec3(10, 0, 20)) {
        return world.CreateEntity(Transform{at, Quaternion::Identity()}, std::move(s));
    }
    // The system runs in fixed small steps, as frames do.
    void Run(f32 seconds, f32 step = 0.1f) {
        for (f32 t = 0.0f; t < seconds - 1e-4f; t += step) system.Update(step);
    }
};

Spawner Basic() {
    Spawner s;
    s.prefab = "Prefabs/Wolf.aprefab";
    s.start_delay = 1.0f;
    s.interval = 2.0f;
    s.burst = 2;
    s.max_alive = 3;
    return s;
}

} // namespace

AETHER_TEST(Spawner_ComponentRegistersAndRoundTrips) {
    RegisterSpawnerComponents();
    CHECK(FindComponentIdByName("Spawner") != kInvalidComponentId);
    Spawner s = Basic();
    s.radius = 4.5f;
    s.total = 12;
    s.timer = 0.75f;
    s.spawned = 5;
    s.enabled = false;
    const reflect::Json saved = reflect::ToJson(s);
    Spawner back;
    CHECK(reflect::FromJson(back, saved) && back.prefab == s.prefab && back.burst == 2 && back.max_alive == 3 && back.total == 12 && !back.enabled);
    CHECK(Near(back.radius, 4.5f) && Near(back.timer, 0.75f) && back.spawned == 5 && Near(back.start_delay, 1.0f) && Near(back.interval, 2.0f));
    // A spawner saved with fewer fields loads with the defaults for the rest: not started, nothing made, five at a time at most.
    reflect::Json old = {{"prefab", "Prefabs/Crate.aprefab"}};
    Spawner loaded;
    CHECK(reflect::FromJson(loaded, old) && loaded.prefab == "Prefabs/Crate.aprefab" && loaded.enabled && loaded.timer == -1.0f && loaded.spawned == 0 && loaded.max_alive == 5);
}

AETHER_TEST(Spawner_ScatterIsDeterministicAndEvenlySpread) {
    CHECK(ScatterOffset(7, 0.0f).x == 0.0f && ScatterOffset(7, 0.0f).z == 0.0f);
    CHECK(ScatterOffset(7, -3.0f).x == 0.0f && ScatterOffset(7, std::numeric_limits<f32>::quiet_NaN()).z == 0.0f && ScatterOffset(7, std::numeric_limits<f32>::infinity()).x == 0.0f);
    const Vec3 a = ScatterOffset(42, 10.0f);
    const Vec3 b = ScatterOffset(42, 10.0f);
    CHECK(a.x == b.x && a.z == b.z && a.y == 0.0f); // the same seed, the same point, in the horizontal plane
    CHECK(ScatterOffset(43, 10.0f).x != a.x || ScatterOffset(43, 10.0f).z != a.z);
    // Every point is inside the disc, they cover all four quadrants, and the mean distance is 2/3 of the radius (even over the area).
    f64 sum = 0.0;
    int quadrant[4] = {0, 0, 0, 0};
    const int count = 4000;
    for (int seed = 0; seed < count; ++seed) {
        const Vec3 p = ScatterOffset(static_cast<u32>(seed), 10.0f);
        const f32 d = std::sqrt(p.x * p.x + p.z * p.z);
        CHECK(d <= 10.0f + 1e-4f);
        sum += d;
        ++quadrant[(p.x >= 0 ? 1 : 0) + (p.z >= 0 ? 2 : 0)];
    }
    CHECK(std::fabs(sum / count - 6.667) < 0.25);
    for (const int q : quadrant) CHECK(q > count / 4 - 150 && q < count / 4 + 150);
}

AETHER_TEST(Spawner_SpawnsOnATimerUpToTheLimit) {
    Rig r;
    CHECK(SpawnerSystem::Active() == &r.system);
    const Entity e = r.MakeSpawner(Basic());
    r.Run(0.5f);
    CHECK(r.calls == 0 && r.system.AliveCount(e) == 0); // the start delay is 1 s
    r.Run(0.6f);
    CHECK(r.calls == 2 && r.system.AliveCount(e) == 2 && r.system.TotalSpawned(e) == 2); // a burst of 2
    r.Run(1.8f);
    CHECK(r.calls == 2); // the interval is 2 s: not yet
    r.Run(0.4f);
    CHECK(r.calls == 3 && r.system.AliveCount(e) == 3); // room for one more only: max_alive is 3
    r.Run(100.0f);
    CHECK(r.calls == 3 && r.system.TotalSpawned(e) == 3); // full: nothing more, however long it waits

    // The timer waits for room: when one is destroyed the next comes an interval later, not at once.
    const std::vector<SpawnEvent> events = r.system.Events();
    CHECK(events.size() == 3 && events[0].spawner == e && r.world.IsAlive(events[0].spawned));
    r.world.DestroyEntity(events[0].spawned);
    CHECK(r.system.AliveCount(e) == 2);
    r.Run(1.5f);
    CHECK(r.calls == 3);
    r.Run(0.7f);
    CHECK(r.calls == 4 && r.system.AliveCount(e) == 3);
    r.system.ClearEvents();
    CHECK(r.system.Events().empty());

    // They appear at the spawner's place, within its radius, with its rotation.
    Rig scatter;
    Spawner wide = Basic();
    wide.radius = 6.0f;
    wide.start_delay = 0.0f;
    wide.max_alive = 0;
    wide.interval = 1.0f;
    const Entity s2 = scatter.MakeSpawner(wide, Vec3(10, 3, 20));
    scatter.Run(20.0f);
    CHECK(scatter.system.TotalSpawned(s2) >= 30);
    bool apart = false;
    for (const SpawnEvent& ev : scatter.system.Events()) {
        const Transform* t = scatter.world.GetComponent<Transform>(ev.spawned);
        CHECK(t != nullptr);
        if (!t) continue;
        const f32 dx = t->position.x - 10.0f, dz = t->position.z - 20.0f;
        CHECK(std::sqrt(dx * dx + dz * dz) <= 6.0f + 1e-3f && Near(t->position.y, 3.0f));
        apart = apart || std::fabs(dx) > 1.0f;
    }
    CHECK(apart); // not all in one place
}

AETHER_TEST(Spawner_StopsAtTheTotalAndHonoursTheSwitches) {
    Rig r;
    Spawner s = Basic();
    s.total = 5;
    s.max_alive = 0; // no limit alive
    s.burst = 2;
    s.start_delay = 0.0f;
    s.interval = 1.0f;
    const Entity e = r.MakeSpawner(s);
    r.Run(30.0f);
    CHECK(r.calls == 5 && r.system.TotalSpawned(e) == 5 && r.system.AliveCount(e) == 5); // 2 + 2 + 1: never past the total
    CHECK(r.world.GetComponent<Spawner>(e)->spawned == 5);

    // A disabled spawner, one with no prefab, and a switch turned on later.
    Rig q;
    Spawner off = Basic();
    off.enabled = false;
    off.start_delay = 0.0f;
    const Entity d = q.MakeSpawner(off);
    Spawner none = Basic();
    none.prefab.clear();
    none.start_delay = 0.0f;
    q.MakeSpawner(none);
    q.Run(10.0f);
    CHECK(q.calls == 0);
    q.world.GetComponent<Spawner>(d)->enabled = true;
    q.Run(0.3f);
    CHECK(q.calls == 2);

    // A host that cannot make the prefab does not make the spawner try every frame: it waits the interval.
    Rig failing;
    failing.host_works = false;
    Spawner f = Basic();
    f.start_delay = 0.0f;
    f.interval = 5.0f;
    failing.MakeSpawner(f);
    failing.Run(11.0f);
    CHECK(failing.calls <= 3 && failing.calls >= 2);

    // Nonsense in the component is held to its range: a negative interval spawns no faster than the minimum, a huge burst no more than 100.
    Rig odd;
    Spawner weird = Basic();
    weird.interval = -5.0f;
    weird.burst = 100000;
    weird.max_alive = 0;
    weird.total = 0;
    weird.start_delay = 0.0f;
    odd.MakeSpawner(weird);
    odd.system.Update(0.1f);
    CHECK(odd.calls == Spawner::kMaxBurst);
    // Updates with no time, a negative time or not a number change nothing.
    Rig still;
    still.MakeSpawner(Basic());
    still.system.Update(0.0f);
    still.system.Update(-1.0f);
    still.system.Update(std::numeric_limits<f32>::quiet_NaN());
    CHECK(still.calls == 0);
}

AETHER_TEST(Spawner_SpawnNowSkipsTheTimerNotTheLimits) {
    Rig r;
    Spawner s = Basic();
    s.start_delay = 1000.0f;
    s.max_alive = 3;
    s.total = 4;
    s.enabled = false; // an explicit request works even for a spawner that is switched off
    const Entity e = r.MakeSpawner(s);
    CHECK(r.system.SpawnNow(e) == 1 && r.system.AliveCount(e) == 1);
    CHECK(r.system.SpawnNow(e, 5) == 2 && r.system.AliveCount(e) == 3); // room for two: max_alive 3
    CHECK(r.system.SpawnNow(e) == 0); // full
    r.world.DestroyEntity(r.system.Events().front().spawned);
    CHECK(r.system.SpawnNow(e, 3) == 1 && r.system.TotalSpawned(e) == 4); // total 4: that is all there will be
    r.world.DestroyEntity(r.system.Events().back().spawned);
    CHECK(r.system.SpawnNow(e) == 0 && r.system.TotalSpawned(e) == 4);
    // What it refuses: a bad count, an entity that is no spawner, a dead one, a spawner with no prefab.
    CHECK(r.system.SpawnNow(e, 0) == 0 && r.system.SpawnNow(e, -2) == 0 && r.system.SpawnNow(e, 101) == 0);
    const Entity plain = r.world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
    CHECK(r.system.SpawnNow(plain) == 0 && r.system.AliveCount(plain) == 0 && r.system.TotalSpawned(plain) == 0);
    CHECK(r.system.SpawnNow(Entity{}) == 0);
    Spawner empty = Basic();
    empty.prefab.clear();
    CHECK(r.system.SpawnNow(r.MakeSpawner(empty)) == 0);
    const Entity gone = r.MakeSpawner(Basic());
    r.world.DestroyEntity(gone);
    CHECK(r.system.SpawnNow(gone) == 0 && r.system.TotalSpawned(gone) == 0);
    // The prefab asked for is the component's.
    CHECK(!r.prefabs_asked.empty() && r.prefabs_asked.front() == "Prefabs/Wolf.aprefab");
}

AETHER_TEST(Spawner_TheKitAddsItsStageAndSystem) {
    const std::unique_ptr<kit::IKit> kit = MakeSpawnerKit();
    CHECK(std::string(kit->Name()) == "Spawner" && kit->Deps().empty());
    const std::vector<kit::KitStage> stages = kit->Stages();
    CHECK(stages.size() == 1 && stages[0].name == "Player.Spawner" && stages[0].after == std::vector<std::string>{"Player.Update"});

    // The runtime needs the world and the host's spawn function.
    World world;
    kit::KitRuntimeContext context;
    CHECK(kit->CreateRuntime(context) == nullptr);
    context.world = &world;
    CHECK(kit->CreateRuntime(context) == nullptr);
    int made = 0;
    SpawnFunction host = [&](const std::string&, const Transform& place) {
        ++made;
        return world.CreateEntity(place);
    };
    context.find_service = [&](const char* name) -> void* { return std::string(name) == "spawner.spawn" ? static_cast<void*>(&host) : nullptr; };
    std::unique_ptr<kit::IKitRuntime> runtime = kit->CreateRuntime(context);
    CHECK(runtime != nullptr);
    if (!runtime) return;
    auto* system = static_cast<SpawnerSystem*>(runtime->Find("SpawnerSystem"));
    CHECK(system != nullptr && runtime->Find("Nope") == nullptr);
    if (!system) return;

    // The scheduler system advances it and hands each spawn to the host's callback.
    Spawner s = Basic();
    s.start_delay = 0.0f;
    const Entity e = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()}, s);
    std::vector<SpawnEvent> heard;
    SystemDesc desc = MakeSpawnerSystem(system, [&](const SpawnEvent& ev) { heard.push_back(ev); });
    CHECK(desc.name == "Player.Spawner" && desc.run != nullptr);
    FrameContext frame;
    frame.dt = 0.1f;
    desc.run(world, frame);
    CHECK(made == 2 && heard.size() == 2 && heard[0].spawner == e && system->Events().empty()); // delivered and cleared
    MakeSpawnerSystem(nullptr, {}).run(world, frame); // no system: nothing happens, nothing crashes
    CHECK(made == 2);
}

namespace {

nlohmann::json AssetEntry(const char* path, const char* importer) {
    return {{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", path}, {"importer", importer}};
}

} // namespace

// The real player: a spawner in the scene makes instances of a package prefab, tells its script (OnSpawned), and one naming a prefab that is not there
// is a warning, once.
AETHER_TEST(Spawner_PlayerSpawnsPackagePrefabsAndTellsTheScript) {
    if (!player::Game::HasScripting()) return;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_spawner_player_tests";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string script_guid = assets::ToString(assets::NewAssetGuid());
    ScriptComponent component;
    assets::AssetGuid parsed;
    CHECK(assets::ParseAssetGuid(script_guid, parsed));
    component.script.guid = parsed;
    Spawner wolves;
    wolves.prefab = "Prefabs/Wolf.aprefab";
    wolves.start_delay = 0.0f;
    wolves.interval = 0.01f;
    wolves.burst = 1;
    wolves.max_alive = 3;
    wolves.radius = 2.0f;
    Spawner ghosts = wolves;
    ghosts.prefab = "Prefabs/Ghost.aprefab"; // not in the package
    const nlohmann::json transform = reflect::ToJson(Transform{Vec3(5, 0, 5), Quaternion::Identity()});
    const nlohmann::json scene = {{"$type", "Scene"}, {"$version", 1},
        {"entities", nlohmann::json::array({{{"components", {{"Transform", transform}, {"Spawner", reflect::ToJson(wolves)}, {"ScriptComponent", reflect::ToJson(component)}}}},
                                            {{"components", {{"Transform", transform}, {"Spawner", reflect::ToJson(ghosts)}}}}})}};
    const nlohmann::json wolf = {{"$type", "Prefab"}, {"$version", 1},
        {"entities", nlohmann::json::array({{{"id", 1}, {"parent", 0}, {"components", {{"Transform", reflect::ToJson(Transform{Vec3(0, 0, 0), Quaternion::Identity()})}}}}})}};
    const std::string script = "local S = {}\nfunction S:OnSpawned(who)\n  SaveGames.SetNumber('heard', SaveGames.GetNumber('heard', 0) + 1)\nend\nreturn S\n";
    const nlohmann::json assets_json = nlohmann::json::array({AssetEntry("Scenes/start.ascene", "Scene"), AssetEntry("Prefabs/Wolf.aprefab", "Prefab"),
                                                              {{"guid", script_guid}, {"path", "Scripts/Watch.luau"}, {"importer", "Script"}}});
    const nlohmann::json manifest = {{"$type", "CookManifest"}, {"$version", 1}, {"project", "Demo"}, {"configuration", "Development"},
                                     {"startup_scene", "Scenes/start.ascene"}, {"fixed_timestep_hz", 60.0}, {"gravity", {0.0, -9.81, 0.0}},
                                     {"layers", {"Default"}}, {"collision_matrix", nlohmann::json::array()}, {"assets", assets_json}, {"files", nlohmann::json::array()}};
    pak::PakWriter writer;
    CHECK(writer.Add("Manifest.json", manifest.dump(2)));
    CHECK(writer.Add("Content/Scenes/start.ascene", scene.dump()));
    CHECK(writer.Add("Content/Prefabs/Wolf.aprefab", wolf.dump()));
    CHECK(writer.Add("Content/Scripts/Watch.luau", script));
    const std::filesystem::path file = dir / "Game.apak";
    CHECK(writer.Write(file.string()));
    player::GamePackage package;
    std::string error;
    CHECK(package.Mount(file.string(), 0, &error) && package.LoadManifest(&error));
    player::Game game(package);
    game.SetUserPaths(player::ResolveUserPaths("Demo", dir / "user"));
    CHECK(game.LoadStartupScene(&error));
    game.BeginPlay();
    for (int i = 0; i < 30; ++i) game.Tick(1.0f / 30.0f);
    CHECK(game.ScriptErrors().empty());
    usize instances = 0;
    game.GetWorld().ForEach<PrefabInstance>([&](PrefabInstance&) { ++instances; });
    CHECK(instances == 3); // max_alive is 3: three wolves, then it waits
    SpawnerSystem* system = SpawnerSystem::Active();
    CHECK(system != nullptr && game.Saves() != nullptr);
    if (!system || !game.Saves()) return;
    CHECK(game.Saves()->Bag().GetInt("heard") == 3); // the spawner's script heard each one
    int ghost_warnings = 0;
    for (const std::string& w : game.Warnings()) ghost_warnings += w.find("Prefabs/Ghost.aprefab") != std::string::npos;
    CHECK(ghost_warnings == 1); // a prefab that is not there: one warning, however often the spawner tried
}

#if AETHER_TEST_HAS_SCRIPTING
AETHER_TEST(Spawner_LuauTableActsOnTheSystem) {
    Rig r;
    script::LuauHost host;
    GuidIndex guids;
    const assets::AssetGuid hero = assets::NewAssetGuid();
    (void)GetComponentId<ScriptComponent>();
    Spawner s = Basic();
    s.start_delay = 1000.0f;
    s.total = 4;
    const Entity target = r.MakeSpawner(s);
    script::ScriptSystem scripts(host, r.world, guids, [&](const assets::AssetGuid& g, std::string& source, std::string& name) {
        if (g != hero) return false;
        source = "local H = {} function H:Run(t) self.first = Spawner.SpawnNow(t); self.alive1 = Spawner.AliveCount(t); self.more = Spawner.SpawnNow(t, 10); "
                 "self.alive2 = Spawner.AliveCount(t); self.total = Spawner.TotalSpawned(t); self.bad0 = Spawner.SpawnNow(t, 0); self.bad200 = Spawner.SpawnNow(t, 200); "
                 "self.plain = Spawner.SpawnNow(self.entity); self.plain_alive = Spawner.AliveCount(self.entity); self.plain_total = Spawner.TotalSpawned(self.entity) end return H";
        name = "H.luau";
        return true;
    });
    ScriptComponent component;
    component.script.guid = hero;
    const Entity e = r.world.CreateEntity(IdComponent{NewEntityGuid()}, std::move(component));
    guids.Add(r.world.GetComponent<IdComponent>(e)->guid, e);
    Lifecycle life(r.world, guids);
    scripts.Register(life);
    life.BeginPlay();
    CHECK(!host.Run("return Spawner.SpawnNow(5)").ok && !host.Run("return Spawner.AliveCount(nil)").ok && !host.Run("return Spawner.TotalSpawned('x')").ok); // wrong types are script errors
    CHECK(scripts.SendEvent(e, "Run", {script::EntityRef{target}}) && scripts.Errors().empty());
    const auto num = [&](const char* f) { const auto v = scripts.GetField(e, f); return std::holds_alternative<f64>(v) ? std::get<f64>(v) : -999.0; };
    CHECK(num("first") == 1.0 && num("alive1") == 1.0);
    CHECK(num("more") == 2.0 && num("alive2") == 3.0 && num("total") == 3.0); // room for two more: max_alive is 3
    CHECK(num("bad0") == 0.0 && num("bad200") == 0.0);
    CHECK(num("plain") == 0.0 && num("plain_alive") == 0.0 && num("plain_total") == 0.0);
    life.EndPlay();
}
#endif

#endif
