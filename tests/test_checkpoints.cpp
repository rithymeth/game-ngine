#include "test_framework.h"

#ifdef AETHER_TEST_HAS_CHECKPOINTS

#include "aether/assets/asset_guid.h"
#include "aether/checkpoints/checkpoint_system.h"
#include "aether/checkpoints/checkpoints_kit.h"
#include "aether/ecs/world.h"
#include "aether/pak/pak.h"
#include "aether/player/game.h"
#include "aether/player/user_paths.h"
#include "aether/save/save_system.h"
#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"
#if AETHER_TEST_HAS_SCRIPTING
#include "aether/script/script_system.h"
#endif

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <limits>

// The checkpoint kit: Checkpoint components mark respawn points; the last one reached is kept in the game's save bag.

using namespace aether;
using namespace aether::chk;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

bool Near(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }

Checkpoint Marker(const char* name, f32 radius = 3.0f) {
    Checkpoint c;
    c.name = name;
    c.radius = radius;
    return c;
}

struct Rig {
    World world;
    save::SaveBag bag;
    CheckpointSystem system{world, bag, [] { return std::string("Scenes/a"); }};
    Entity player;
    Entity a, b;

    Rig() {
        a = world.CreateEntity(Transform{Vec3(0, 0, 0), Quaternion::Identity()}, Marker("A"));
        b = world.CreateEntity(Transform{Vec3(50, 0, 0), Quaternion::Identity()}, Marker("B"));
        player = world.CreateEntity(Transform{Vec3(100, 0, 0), Quaternion::Identity()});
        AddTag(world, player, "Player");
    }
    void MovePlayer(Vec3 to) { world.GetComponent<Transform>(player)->position = to; }
};

} // namespace

AETHER_TEST(Checkpoints_ComponentRegistersAndRoundTrips) {
    RegisterCheckpointComponents();
    CHECK(FindComponentIdByName("Checkpoint") != kInvalidComponentId);
    Checkpoint c = Marker("Gate", 7.5f);
    c.activator_tag = "Hero";
    c.respawn_offset = Vec3(1, 2, 3);
    c.enabled = false;
    const reflect::Json saved = reflect::ToJson(c);
    Checkpoint back;
    CHECK(reflect::FromJson(back, saved) && back.name == "Gate" && Near(back.radius, 7.5f) && back.activator_tag == "Hero" && !back.enabled);
    CHECK(Near(back.respawn_offset.x, 1.0f) && Near(back.respawn_offset.y, 2.0f) && Near(back.respawn_offset.z, 3.0f));
    // Saved with only a name: the player activates it within 3 m, and it respawns a little above the ground.
    Checkpoint loaded;
    CHECK(reflect::FromJson(loaded, reflect::Json{{"name", "Camp"}}) && loaded.name == "Camp" && loaded.enabled && Near(loaded.radius, 3.0f) && loaded.activator_tag == "Player");
    CHECK(Near(loaded.respawn_offset.y, 0.5f) && Near(loaded.respawn_offset.x, 0.0f));
}

AETHER_TEST(Checkpoints_AreReachedByTheirActivatorsAndRemembered) {
    Rig r;
    CHECK(CheckpointSystem::Active() == &r.system);
    r.system.Update();
    CHECK(r.system.Last().empty() && r.system.Events().empty() && !r.system.IsReached("A"));

    // Coming within the radius makes it the last one: the bag holds the name, the scene and that it was reached.
    r.MovePlayer(Vec3(1, 0, 0));
    r.system.Update();
    CHECK(r.system.Last() == "A" && r.system.LastScene() == "Scenes/a" && r.system.IsReached("A") && !r.system.IsReached("B"));
    CHECK(r.bag.GetString("checkpoint.last") == "A" && r.bag.GetString("checkpoint.scene") == "Scenes/a" && r.bag.GetInt("checkpoint.A") == 1);
    CHECK(r.system.Events().size() == 1 && r.system.Events()[0].checkpoint == r.a && r.system.Events()[0].activator == r.player && r.system.Events()[0].name == "A" && r.system.Events()[0].first_time);
    r.system.ClearEvents();

    // Staying there says nothing more; another checkpoint takes over; going back to the first is not "first time".
    r.system.Update();
    r.system.Update();
    CHECK(r.system.Events().empty());
    r.MovePlayer(Vec3(50, 1, 0));
    r.system.Update();
    CHECK(r.system.Last() == "B" && r.system.IsReached("A") && r.system.IsReached("B") && r.system.Events().size() == 1 && r.system.Events()[0].first_time);
    r.system.ClearEvents();
    r.MovePlayer(Vec3(0, 0, 2));
    r.system.Update();
    CHECK(r.system.Last() == "A" && r.system.Events().size() == 1 && !r.system.Events()[0].first_time);

    // The edge: on the radius counts, a little beyond does not.
    Rig edge;
    edge.MovePlayer(Vec3(3.01f, 0, 0));
    edge.system.Update();
    CHECK(edge.system.Last().empty());
    edge.MovePlayer(Vec3(3.0f, 0, 0));
    edge.system.Update();
    CHECK(edge.system.Last() == "A");

    // What does not count: a disabled checkpoint, one with no name, an entity without the activator tag, an activator without a position.
    Rig q;
    q.world.GetComponent<Checkpoint>(q.a)->enabled = false;
    q.world.CreateEntity(Transform{Vec3(100, 0, 0), Quaternion::Identity()}, Marker("")); // no name
    q.MovePlayer(Vec3(0, 0, 0));
    q.system.Update();
    CHECK(q.system.Last().empty()); // A is disabled
    q.world.GetComponent<Checkpoint>(q.a)->enabled = true;
    q.world.GetComponent<Checkpoint>(q.a)->activator_tag = "Hero";
    q.system.Update();
    CHECK(q.system.Last().empty()); // the player is no hero
    const Entity hero = q.world.CreateEntity(Transform{Vec3(1, 0, 0), Quaternion::Identity()});
    AddTag(q.world, hero, "Hero");
    q.system.Update();
    CHECK(q.system.Last() == "A" && q.system.Events().back().activator == hero);
    // A nonsense radius is held to the minimum: nothing is within 0.01 m of the player here.
    Rig odd;
    odd.world.GetComponent<Checkpoint>(odd.a)->radius = std::numeric_limits<f32>::quiet_NaN();
    odd.MovePlayer(Vec3(1, 0, 0));
    odd.system.Update();
    CHECK(odd.system.Last().empty());
}

AETHER_TEST(Checkpoints_ActivateFindAndRespawn) {
    Rig r;
    CHECK(r.system.Find("A") == r.a && r.system.Find("B") == r.b && r.system.Find("Nope").IsNull() && r.system.Find("").IsNull());
    CHECK(r.system.Respawn(r.player) == Result::NoCheckpoint); // none reached yet
    CHECK(!r.system.Activate("Nope") && !r.system.Activate("") && r.system.Last().empty());

    // A script can make one the last at once (no activator), and Respawn puts the entity there: the checkpoint's place plus its offset, facing as it faces.
    Rig s;
    s.world.GetComponent<Checkpoint>(s.b)->respawn_offset = Vec3(0, 2, 1);
    s.world.GetComponent<Transform>(s.b)->rotation = Quaternion::FromAxisAngle(Vec3(0, 1, 0), 1.0f);
    CHECK(s.system.Activate("B") && s.system.Last() == "B" && s.system.Events().size() == 1 && s.system.Events()[0].activator.IsNull() && s.system.Events()[0].checkpoint == s.b);
    CHECK(s.system.Respawn(s.player) == Result::Ok);
    const Transform* t = s.world.GetComponent<Transform>(s.player);
    CHECK(t != nullptr && Near(t->position.x, 50.0f) && Near(t->position.y, 2.0f) && Near(t->position.z, 1.0f));
    CHECK(t != nullptr && Near(t->rotation.y, s.world.GetComponent<Transform>(s.b)->rotation.y) && Near(t->rotation.w, s.world.GetComponent<Transform>(s.b)->rotation.w));

    // What it refuses: a dead or null entity, one with no position, and a last checkpoint that is not in this scene.
    CHECK(s.system.Respawn(Entity{}) == Result::DeadEntity);
    const Entity gone = s.world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
    s.world.DestroyEntity(gone);
    CHECK(s.system.Respawn(gone) == Result::DeadEntity);
    CHECK(s.world.GetComponent<Transform>(s.player) != nullptr);
    World bare;
    save::SaveBag bag;
    bag.SetString("checkpoint.last", "Elsewhere");
    CheckpointSystem other(bare, bag);
    const Entity ghost = bare.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
    CHECK(other.Respawn(ghost) == Result::NotInScene && other.Last() == "Elsewhere"); // another scene's checkpoint: the bag knows it, this world does not
    s.system.MakeActive();
    const Entity no_position = s.world.CreateEntity(IdComponent{NewEntityGuid()});
    CHECK(s.system.Respawn(no_position) == Result::NoTransform);

    // The state is in the bag, so a system over a copy of it (a loaded slot) agrees, without announcing anything.
    save::SaveBag loaded = s.bag;
    CheckpointSystem restored(s.world, loaded);
    CHECK(restored.Last() == "B" && restored.IsReached("B") && !restored.IsReached("A") && restored.Events().empty());
    s.system.MakeActive();
}

AETHER_TEST(Checkpoints_TheKitAddsItsStageAndSystem) {
    const std::unique_ptr<kit::IKit> kit = MakeCheckpointsKit();
    CHECK(std::string(kit->Name()) == "Checkpoints" && kit->Deps().empty());
    const std::vector<kit::KitStage> stages = kit->Stages();
    CHECK(stages.size() == 1 && stages[0].name == "Player.Checkpoints" && stages[0].after == std::vector<std::string>{"Player.Update"});

    // The runtime needs the world and the host's save bag; the scene name is optional.
    World world;
    save::SaveBag bag;
    kit::KitRuntimeContext context;
    CHECK(kit->CreateRuntime(context) == nullptr);
    context.world = &world;
    CHECK(kit->CreateRuntime(context) == nullptr);
    context.find_service = [&](const char* name) -> void* { return std::string(name) == "save.bag" ? static_cast<void*>(&bag) : nullptr; };
    std::unique_ptr<kit::IKitRuntime> runtime = kit->CreateRuntime(context);
    CHECK(runtime != nullptr);
    if (!runtime) return;
    auto* system = static_cast<CheckpointSystem*>(runtime->Find("CheckpointSystem"));
    CHECK(system != nullptr && runtime->Find("Nope") == nullptr);
    if (!system) return;

    // The scheduler system updates it and hands each event to the host's callback, then clears them.
    const Entity marker = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()}, Marker("Camp"));
    const Entity who = world.CreateEntity(Transform{Vec3(1, 0, 0), Quaternion::Identity()});
    AddTag(world, who, "Player");
    std::vector<CheckpointEvent> heard;
    SystemDesc desc = MakeCheckpointsSystem(system, [&](const CheckpointEvent& e) { heard.push_back(e); });
    CHECK(desc.name == "Player.Checkpoints" && desc.run != nullptr);
    FrameContext frame;
    desc.run(world, frame);
    CHECK(heard.size() == 1 && heard[0].checkpoint == marker && heard[0].activator == who && system->Events().empty() && bag.GetString("checkpoint.scene").empty());
    MakeCheckpointsSystem(nullptr, {}).run(world, frame); // no system: nothing happens, nothing crashes

    // With the host's scene service the bag also holds the scene.
    std::function<std::string()> scene = [] { return std::string("Scenes/camp"); };
    context.find_service = [&](const char* name) -> void* {
        if (std::string(name) == "save.bag") return &bag;
        if (std::string(name) == "checkpoints.scene") return &scene;
        return nullptr;
    };
    std::unique_ptr<kit::IKitRuntime> with_scene = kit->CreateRuntime(context);
    CHECK(with_scene != nullptr);
    if (with_scene) {
        auto* second = static_cast<CheckpointSystem*>(with_scene->Find("CheckpointSystem"));
        CHECK(second != nullptr && second->Activate("Camp") && bag.GetString("checkpoint.scene") == "Scenes/camp");
    }
}

namespace {

nlohmann::json AssetEntry(const char* path, const char* importer) {
    return {{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", path}, {"importer", importer}};
}

} // namespace

// The real player: a checkpoint in the scene is reached by the tagged player, its script hears OnCheckpoint, the bag a slot writes holds the state, and a
// scene change leaves it alone (and from the next scene, which has no such checkpoint, Respawn says so).
AETHER_TEST(Checkpoints_PlayerReachesCheckpointsThroughASceneChange) {
    if (!player::Game::HasScripting()) return;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_checkpoints_player_tests";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto script_component = [](const std::string& guid) {
        ScriptComponent component;
        assets::AssetGuid parsed;
        assets::ParseAssetGuid(guid, parsed);
        component.script.guid = parsed;
        return component;
    };
    const std::string watch_guid = assets::ToString(assets::NewAssetGuid());
    const std::string swap_guid = assets::ToString(assets::NewAssetGuid());
    Tags tags;
    tags.names = {"Player"};
    const nlohmann::json a = {{"$type", "Scene"}, {"$version", 1}, {"entities", nlohmann::json::array({
        {{"components", {{"Transform", reflect::ToJson(Transform{Vec3(0, 0, 0), Quaternion::Identity()})}, {"Tags", reflect::ToJson(tags)}}}},
        {{"components", {{"Transform", reflect::ToJson(Transform{Vec3(1, 0, 0), Quaternion::Identity()})}, {"Checkpoint", reflect::ToJson(Marker("Gate"))},
                         {"ScriptComponent", reflect::ToJson(script_component(watch_guid))}}}},
        {{"components", {{"ScriptComponent", reflect::ToJson(script_component(swap_guid))}}}}})}};
    const nlohmann::json b = {{"$type", "Scene"}, {"$version", 1}, {"entities", nlohmann::json::array()}};
    const std::string watch = "local W = {}\nfunction W:OnCheckpoint(who)\n  SaveGames.SetNumber('heard', SaveGames.GetNumber('heard', 0) + 1)\nend\nreturn W\n";
    const std::string swap = "local S = {}\nfunction S:OnUpdate(dt)\n  if not self.asked then self.asked = true; Scene.Load('Scenes/b.ascene') end\nend\nreturn S\n";
    const nlohmann::json assets_json = nlohmann::json::array({AssetEntry("Scenes/a.ascene", "Scene"), AssetEntry("Scenes/b.ascene", "Scene"),
        {{"guid", watch_guid}, {"path", "Scripts/Watch.luau"}, {"importer", "Script"}}, {{"guid", swap_guid}, {"path", "Scripts/Swap.luau"}, {"importer", "Script"}}});
    const nlohmann::json manifest = {{"$type", "CookManifest"}, {"$version", 1}, {"project", "Demo"}, {"configuration", "Development"},
                                     {"startup_scene", "Scenes/a.ascene"}, {"fixed_timestep_hz", 60.0}, {"gravity", {0.0, -9.81, 0.0}},
                                     {"layers", {"Default"}}, {"collision_matrix", nlohmann::json::array()}, {"assets", assets_json}, {"files", nlohmann::json::array()}};
    pak::PakWriter writer;
    CHECK(writer.Add("Manifest.json", manifest.dump(2)));
    CHECK(writer.Add("Content/Scenes/a.ascene", a.dump()));
    CHECK(writer.Add("Content/Scenes/b.ascene", b.dump()));
    CHECK(writer.Add("Content/Scripts/Watch.luau", watch));
    CHECK(writer.Add("Content/Scripts/Swap.luau", swap));
    const std::filesystem::path file = dir / "Game.apak";
    CHECK(writer.Write(file.string()));
    player::GamePackage package;
    std::string error;
    CHECK(package.Mount(file.string(), 0, &error) && package.LoadManifest(&error));
    player::Game game(package);
    game.SetUserPaths(player::ResolveUserPaths("Demo", dir / "user"));
    CHECK(game.LoadStartupScene(&error));
    game.BeginPlay();
    CHECK(game.Saves() != nullptr);
    if (!game.Saves()) return;
    game.Tick(1.0f / 60.0f); // the checkpoint is reached in this frame's checkpoint stage; the swap script then asks for the next scene
    CHECK(game.Saves()->Bag().GetString("checkpoint.last") == "Gate" && game.Saves()->Bag().GetString("checkpoint.scene") == "Scenes/a.ascene");
    for (int i = 0; i < 3; ++i) game.Tick(1.0f / 60.0f);
    CHECK(game.SceneName() == "Scenes/b.ascene" && game.ScriptErrors().empty());
    CHECK(game.Saves()->Bag().GetInt("heard") == 1); // the checkpoint's script heard it once
    CHECK(game.Saves()->Bag().GetString("checkpoint.last") == "Gate" && game.Saves()->Bag().GetInt("checkpoint.Gate") == 1); // the scene change left it alone
    CheckpointSystem* system = CheckpointSystem::Active();
    CHECK(system != nullptr);
    if (!system) return;
    CHECK(system->Last() == "Gate" && system->LastScene() == "Scenes/a.ascene"); // a script can read the scene to load
    const Entity visitor = game.GetWorld().CreateEntity(Transform{Vec3(9, 9, 9), Quaternion::Identity()});
    CHECK(system->Respawn(visitor) == Result::NotInScene); // the last checkpoint is in the other scene
}

#if AETHER_TEST_HAS_SCRIPTING
AETHER_TEST(Checkpoints_LuauTableActsOnTheSystem) {
    Rig r;
    script::LuauHost host;
    GuidIndex guids;
    const assets::AssetGuid hero = assets::NewAssetGuid();
    (void)GetComponentId<ScriptComponent>();
    script::ScriptSystem scripts(host, r.world, guids, [&](const assets::AssetGuid& g, std::string& source, std::string& name) {
        if (g != hero) return false;
        source = "local H = {} function H:Run() self.last0 = Checkpoints.Last(); self.scene0 = Checkpoints.GetScene(); self.no_cp = Checkpoints.Respawn(self.entity); "
                 "self.exists = Checkpoints.Exists('A'); self.gone = Checkpoints.Exists('Nope'); self.reached0 = Checkpoints.IsReached('A'); "
                 "self.act = Checkpoints.Activate('B'); self.act_bad = Checkpoints.Activate('Nope'); self.last1 = Checkpoints.Last(); self.scene1 = Checkpoints.GetScene(); "
                 "self.reached1 = Checkpoints.IsReached('B'); self.respawn = Checkpoints.Respawn(self.entity) end return H";
        name = "H.luau";
        return true;
    });
    ScriptComponent component;
    component.script.guid = hero;
    const Entity e = r.world.CreateEntity(IdComponent{NewEntityGuid()}, Transform{Vec3(-5, -5, -5), Quaternion::Identity()}, std::move(component));
    guids.Add(r.world.GetComponent<IdComponent>(e)->guid, e);
    Lifecycle life(r.world, guids);
    scripts.Register(life);
    life.BeginPlay();
    CHECK(!host.Run("return Checkpoints.IsReached(5)").ok && !host.Run("return Checkpoints.Activate(nil)").ok && !host.Run("return Checkpoints.Respawn('x')").ok); // wrong types are script errors
    CHECK(scripts.SendEvent(e, "Run") && scripts.Errors().empty());
    const auto text = [&](const char* f) { const auto v = scripts.GetField(e, f); return std::holds_alternative<std::string>(v) ? std::get<std::string>(v) : std::string("<not a string>"); };
    const auto flag = [&](const char* f) { const auto v = scripts.GetField(e, f); return std::holds_alternative<bool>(v) && std::get<bool>(v); };
    CHECK(text("last0").empty() && text("scene0").empty() && text("no_cp") == "no_checkpoint");
    CHECK(flag("exists") && !flag("gone") && !flag("reached0"));
    CHECK(flag("act") && !flag("act_bad") && text("last1") == "B" && text("scene1") == "Scenes/a" && flag("reached1") && text("respawn") == "ok");
    const Transform* t = r.world.GetComponent<Transform>(e);
    CHECK(t != nullptr && Near(t->position.x, 50.0f) && Near(t->position.y, 0.5f) && Near(t->position.z, 0.0f)); // B's place plus the default offset
    life.EndPlay();
}
#endif

#endif
