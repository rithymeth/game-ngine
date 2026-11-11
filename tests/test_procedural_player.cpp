#include "aether/assets/asset_database.h"
#include "aether/input/bindings.h"
#include "aether/job/job_system.h"
#include "aether/physics/character.h"
#include "aether/physics/components.h"
#include "aether/physics/physics_scene.h"
#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/serialization.h"
#include "test_framework.h"
#include "world/procedural_document.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>

#ifndef AETHER_TEST_HAS_SCRIPTING
#define AETHER_TEST_HAS_SCRIPTING 0
#endif
#ifndef AETHER_TEST_HAS_PHYSICS
#define AETHER_TEST_HAS_PHYSICS 0
#endif
#if AETHER_TEST_HAS_SCRIPTING
#include "aether/script/script_system.h"
#endif

using namespace aether;
using namespace aether::editor;
using namespace aether::terrain;
namespace stdfs = std::filesystem;

// A baked world you can be in: a Player with the third-person controller, its body, a camera, a sun and a sky, on dry walkable land.

namespace {

ProceduralWorldSettings Hills() {
    ProceduralWorldSettings s = MakePreset(WorldPreset::Temperate);
    s.terrain.width = s.terrain.height = 129;
    s.terrain.hydraulic_droplets = 4000;
    s.terrain.river_min_catchment = 80;
    return s;
}

} // namespace

AETHER_TEST(ProceduralPlayer_TheSpawnIsDryWalkableLandInsideTheMap) {
    for (const WorldPreset preset : {WorldPreset::Temperate, WorldPreset::Alpine, WorldPreset::Desert, WorldPreset::Islands}) {
        ProceduralWorldSettings s = MakePreset(preset);
        s.terrain.width = s.terrain.height = 97;
        s.terrain.hydraulic_droplets = 3000;
        ProceduralWorldDocument doc(s);
        doc.Generate();
        const GeneratedTerrain& t = doc.Terrain();
        const Vec3 spawn = ProceduralWorldDocument::FindSpawnPoint(t);
        const f32 extent = static_cast<f32>(t.Width() - 1) * t.heightmap.cell_size;
        AETHER_CHECK(spawn.x > 0.0f && spawn.x < extent && spawn.z > 0.0f && spawn.z < extent);
        AETHER_CHECK(std::fabs(spawn.y - t.heightmap.SampleLinear(spawn.x, spawn.z)) < 0.01f); // on the ground
        const f32 cell = t.heightmap.cell_size;
        const f32 dx = (t.heightmap.SampleLinear(spawn.x + cell, spawn.z) - t.heightmap.SampleLinear(spawn.x - cell, spawn.z)) / (2.0f * cell);
        const f32 dz = (t.heightmap.SampleLinear(spawn.x, spawn.z + cell) - t.heightmap.SampleLinear(spawn.x, spawn.z - cell)) / (2.0f * cell);
        std::printf("    spawn at (%.0f, %.0f) of %.0f m: %.1f m up (sea %.1f), slope %.2f\n", spawn.x, spawn.z, extent, spawn.y, t.sea_height, std::sqrt(dx * dx + dz * dz));
        AETHER_CHECK(spawn.y > t.sea_height + 1.5f); // dry
        AETHER_CHECK(std::sqrt(dx * dx + dz * dz) < 0.75f); // gentler than the 45 degrees (1.0) a character stands on, with room to spare
    }
}

AETHER_TEST(ProceduralPlayer_ThePlayerModelIsAFigureStandingOnTheGround) {
    const assets::procedural::ProceduralMesh model = ProceduralWorldDocument::BuildPlayerModel();
    std::string error;
    AETHER_CHECK(assets::procedural::ValidateMesh(model, &error));
    AETHER_CHECK(model.mesh.primitives.size() == 4 && model.materials.size() == 4);
    AETHER_CHECK(model.TriangleCount() > 100 && model.TriangleCount() < 700);
    f32 lo = 1e9f, hi = -1e9f, half_width = 0.0f;
    for (const auto& p : model.mesh.primitives)
        for (const auto& v : p.vertices) { lo = std::min(lo, v.position[1]); hi = std::max(hi, v.position[1]); half_width = std::max({half_width, std::fabs(v.position[0]), std::fabs(v.position[2])}); }
    AETHER_CHECK(lo > -0.01f && lo < 0.05f);   // the soles on y = 0
    AETHER_CHECK(hi > 1.6f && hi < 1.85f);     // about a person
    AETHER_CHECK(half_width < 0.5f);           // narrower than the character's capsule is tall
}

AETHER_TEST(ProceduralPlayer_TheJointedPartsMakeUpTheWholeFigureAndPivotAtTheirJoints) {
    const PlayerPart parts[] = {PlayerPart::Body, PlayerPart::LegLeft, PlayerPart::LegRight, PlayerPart::ArmLeft, PlayerPart::ArmRight};
    u32 triangles = 0;
    for (const PlayerPart part : parts) {
        const assets::procedural::ProceduralMesh m = ProceduralWorldDocument::BuildPlayerModel(part);
        std::string error;
        AETHER_CHECK(assets::procedural::ValidateMesh(m, &error));
        AETHER_CHECK(m.TriangleCount() > 0);
        triangles += m.TriangleCount();
        f32 lo = 1e9f, hi = -1e9f;
        for (const auto& p : m.mesh.primitives)
            for (const auto& v : p.vertices) { lo = std::min(lo, v.position[1]); hi = std::max(hi, v.position[1]); }
        if (part == PlayerPart::Body) {
            AETHER_CHECK(lo > 0.8f && hi > 1.6f); // still at the figure's own height: its origin is the feet
        } else {
            AETHER_CHECK(hi < 0.12f && lo < -0.3f); // a limb hangs down from its joint, which is its origin
            const Vec3 joint = ProceduralWorldDocument::PlayerJoint(part);
            AETHER_CHECK(joint.y > 0.8f && std::fabs(joint.x) > 0.1f);
        }
    }
    AETHER_CHECK(triangles == ProceduralWorldDocument::BuildPlayerModel(PlayerPart::Whole).TriangleCount()); // nothing lost, nothing doubled
}

#if AETHER_TEST_HAS_SCRIPTING && AETHER_TEST_HAS_PHYSICS && AETHER_EDITOR_NET_PHYSICS
namespace {

struct Play {
    stdfs::path root;
    std::unique_ptr<assets::AssetDatabase> db;
    World world;
    std::unique_ptr<JobSystem> jobs;
    std::unique_ptr<PhysicsWorld> physics;
    std::unique_ptr<PhysicsScene> physics_scene;
    std::unique_ptr<CharacterSystem> characters;
    GuidIndex guids;
    script::LuauHost host;
    std::unique_ptr<Lifecycle> life;
    std::unique_ptr<script::ScriptSystem> scripts;
    input::InputSystem input;
    input::InputState state;
    input::InputAssetLibrary library;
    Entity player, camera;
    const GeneratedTerrain* terrain = nullptr;

    ~Play() {
        scripts.reset();
        life.reset();
    }

    // Bakes `doc` as a playable world into `root` and loads it as the player would.
    bool Load(const ProceduralWorldDocument& doc, const stdfs::path& where, std::string* why) {
        root = where;
        std::error_code ignored;
        stdfs::remove_all(root, ignored);
        BakeOptions options;
        options.name = "Play";
        options.terrain_texture = 0;
        options.water_texture = 0;
        options.max_scene_instances = 200;
        const BakeReport report = doc.Bake(root.string(), options);
        if (!report.ok || !report.player_added) { *why = report.error.empty() ? "no player was added" : report.error; return false; }
        terrain = &doc.Terrain();
        db = std::make_unique<assets::AssetDatabase>(root);
        db->Scan();
        RegisterPhysicsComponentSerializers();
        (void)GetComponentId<MeshCollider>();
        (void)GetComponentId<CharacterMovement>();
        if (!LoadSceneJson(world, (root / report.scene).string())) { *why = "the scene did not load"; return false; }
        jobs = std::make_unique<JobSystem>(2);
        physics = std::make_unique<PhysicsWorld>(*jobs);
        physics_scene = std::make_unique<PhysicsScene>(world, *physics);
        physics_scene->Sync();
        characters = std::make_unique<CharacterSystem>(world, *physics, physics_scene.get());
        guids.Rebuild(world);
        life = std::make_unique<Lifecycle>(world, guids);
        scripts = std::make_unique<script::ScriptSystem>(host, world, guids, script::ScriptSystem::DatabaseLoader(*db));
        scripts->Register(*life);
        scripts->BindPhysicsScene(physics_scene.get()); // as the player does: the third person camera casts a sphere
        library.Load(*db);
        library.RegisterActions(input);
        if (!library.Activate(input, "Gameplay", 0)) { *why = "the Gameplay bindings did not activate"; return false; }
        scripts->BindInput(&input);
        const std::vector<Entity> players = FindEntitiesWithTag(world, "Player"), cameras = FindEntitiesWithTag(world, "MainCamera");
        if (players.size() != 1 || cameras.size() != 1) { *why = "expected one Player and one MainCamera"; return false; }
        player = players[0];
        camera = cameras[0];
        life->BeginPlay();
        for (const std::string& e : scripts->Errors()) std::printf("    script error: %s\n", e.c_str());
        if (!scripts->Errors().empty()) { *why = "a script failed"; return false; }
        if (scripts->InstanceCount() != 5) { *why = "expected 5 scripts (the controller and four limbs), found " + std::to_string(scripts->InstanceCount()); return false; }
        return true;
    }
    void Frames(int n, f32 dt = 1.0f / 60.0f) {
        for (int i = 0; i < n; ++i) {
            input.Update(state, dt);
            characters->Step(dt);
            physics_scene->Step(dt);
            life->Update(dt);
            scripts->Tick(dt);
            state.EndFrame();
        }
        for (const std::string& e : scripts->Errors()) std::printf("    script error: %s\n", e.c_str());
        AETHER_CHECK(scripts->Errors().empty());
        scripts->ClearErrors();
    }
    Vec3 Position(Entity e) { return world.GetComponent<Transform>(e)->position; }
    f32 Ground(const Vec3& p) const { return terrain->heightmap.SampleLinear(p.x, p.z); }
};

} // namespace

AETHER_TEST(ProceduralPlayer_ABakedWorldCanBePlayed) {
    ProceduralWorldDocument doc(Hills());
    doc.Generate();
    Play play;
    std::string why;
    const bool loaded = play.Load(doc, stdfs::temp_directory_path() / "aether_procedural_play", &why);
    if (!loaded) std::printf("    %s\n", why.c_str());
    AETHER_CHECK(loaded);
    if (!loaded) return;
    AETHER_CHECK(stdfs::exists(play.root / "Scripts/ThirdPersonController.luau") && stdfs::exists(play.root / "Input/Gameplay.amapping"));

    // It lands: standing, grounded, on the surface.
    play.Frames(120);
    const CharacterMovement* movement = play.world.GetComponent<CharacterMovement>(play.player);
    AETHER_CHECK(movement && movement->grounded);
    const Vec3 start = play.Position(play.player);
    std::printf("    standing at (%.1f, %.1f, %.1f), the ground %.2f\n", start.x, start.y, start.z, play.Ground(start));
    AETHER_CHECK(std::fabs(start.y - play.Ground(start)) < 0.35f);

    // It jumps and comes down (here, on the gentle ground it spawned on, before any walking takes it to a slope).
    const f32 floor = play.Position(play.player).y;
    f32 apex = floor;
    play.state.SetButton(input::Key::Space, true);
    play.Frames(2);
    play.state.SetButton(input::Key::Space, false);
    for (int i = 0; i < 60; ++i) {
        play.Frames(1);
        apex = std::max(apex, play.Position(play.player).y);
    }
    play.Frames(90);
    std::printf("    jumped %.2f m above where it stood\n", apex - floor);
    AETHER_CHECK(apex - floor > 0.6f);
    movement = play.world.GetComponent<CharacterMovement>(play.player);
    AETHER_CHECK(movement && movement->grounded);

    // It walks: W, D, S and A in turn (relative to where the camera looks), a second and a half each. At least one way is open for a few metres;
    // none puts it through the ground.
    f32 best_distance = 0.0f, lowest = 1e9f, camera_lowest = 1e9f;
    for (const input::Key key : {input::Key::W, input::Key::D, input::Key::S, input::Key::A}) {
        const Vec3 from = play.Position(play.player);
        play.state.SetButton(key, true);
        for (int i = 0; i < 90; ++i) {
            play.Frames(1);
            const Vec3 p = play.Position(play.player);
            lowest = std::min(lowest, p.y - play.Ground(p));
            const Vec3 cam = play.Position(play.camera);
            camera_lowest = std::min(camera_lowest, cam.y - play.Ground(cam));
        }
        play.state.SetButton(key, false);
        const Vec3 to = play.Position(play.player);
        best_distance = std::max(best_distance, std::hypot(to.x - from.x, to.z - from.z));
        play.Frames(30);
    }
    std::printf("    the best direction covered %.1f m; the lowest the feet went below the ground: %.2f m\n", best_distance, lowest);
    AETHER_CHECK(best_distance > 3.0f);
    AETHER_CHECK(lowest > -0.4f);
    std::printf("    the camera's lowest clearance over the ground while walking: %.2f m\n", camera_lowest);
    AETHER_CHECK(camera_lowest > -0.1f); // the camera never goes into a hill

    // The camera followed: a few metres from the character, not inside the ground.
    const Vec3 p = play.Position(play.player), c = play.Position(play.camera);
    const f32 distance = std::sqrt((c.x - p.x) * (c.x - p.x) + (c.y - (p.y + 1.6f)) * (c.y - (p.y + 1.6f)) + (c.z - p.z) * (c.z - p.z));
    std::printf("    the camera is %.1f m from the character, %.2f m above the ground under it\n", distance, c.y - play.Ground(c));
    AETHER_CHECK(distance > 4.0f && distance < 8.0f);
    stdfs::remove_all(play.root);
}

AETHER_TEST(ProceduralPlayer_TheFigureSwingsItsLimbsWhenItWalksAndSettlesWhenItStops) {
    ProceduralWorldDocument doc(Hills());
    doc.Generate();
    Play play;
    std::string why;
    const bool loaded = play.Load(doc, stdfs::temp_directory_path() / "aether_procedural_swing", &why);
    if (!loaded) std::printf("    %s\n", why.c_str());
    AETHER_CHECK(loaded);
    if (!loaded) return;
    auto limb = [&](const char* tag) {
        const std::vector<Entity> found = FindEntitiesWithTag(play.world, tag);
        return found.empty() ? kNullEntity : found[0];
    };
    const Entity leg_left = limb("LegLeft"), leg_right = limb("LegRight"), arm_left = limb("ArmLeft"), arm_right = limb("ArmRight");
    AETHER_CHECK(!leg_left.IsNull() && !leg_right.IsNull() && !arm_left.IsNull() && !arm_right.IsNull());
    if (leg_left.IsNull() || leg_right.IsNull() || arm_left.IsNull() || arm_right.IsNull()) return;
    auto swing = [&](Entity e) { return play.world.GetComponent<Transform>(e)->rotation.x; }; // sin of half the angle about X
    // Standing: every limb is still.
    play.Frames(120);
    for (const Entity e : {leg_left, leg_right, arm_left, arm_right}) AETHER_CHECK(std::fabs(swing(e)) < 0.02f);
    // Walking (whichever way is open): the legs swing, a leg against the other, and each arm against the leg on its side.
    f32 widest = 0.0f, other_leg = 0.0f, same_side_arm = 0.0f;
    for (const input::Key key : {input::Key::W, input::Key::D, input::Key::S, input::Key::A}) {
        play.state.SetButton(key, true);
        for (int i = 0; i < 90; ++i) {
            play.Frames(1);
            if (std::fabs(swing(leg_left)) > widest) { widest = std::fabs(swing(leg_left)); other_leg = swing(leg_right) * swing(leg_left); same_side_arm = swing(arm_left) * swing(leg_left); }
        }
        play.state.SetButton(key, false);
        if (widest > 0.15f) break;
        play.Frames(30);
    }
    std::printf("    widest leg swing: sin(half angle) %.2f (%.0f degrees), against the other leg %.3f, against its own-side arm %.3f\n", widest, 2.0f * std::asin(std::min(widest, 1.0f)) * 57.29578f, other_leg, same_side_arm);
    AETHER_CHECK(widest > 0.15f);        // a swing of over 17 degrees
    AETHER_CHECK(other_leg < 0.0f);      // the legs go opposite ways
    AETHER_CHECK(same_side_arm < 0.0f);  // an arm against the leg on its side
    // Stopped: it settles.
    play.Frames(120);
    for (const Entity e : {leg_left, leg_right, arm_left, arm_right}) AETHER_CHECK(std::fabs(swing(e)) < 0.05f);
    stdfs::remove_all(play.root);
}
#endif
