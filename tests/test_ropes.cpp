#include "test_framework.h"

#ifdef AETHER_TEST_HAS_ROPES

#include "aether/assets/asset_guid.h"
#include "aether/ecs/world.h"
#include "aether/job/job_system.h"
#include "aether/pak/pak.h"
#include "aether/physics/components.h"
#include "aether/physics/constraints.h"
#include "aether/physics/physics_scene.h"
#include "aether/player/game.h"
#include "aether/player/user_paths.h"
#include "aether/ropes/rope_system.h"
#include "aether/ropes/ropes_kit.h"
#include "aether/save/save_system.h"
#include "aether/scene/components.h"
#include "aether/scene/entity_guid.h"
#include "aether/scene/gameplay.h"
#if AETHER_TEST_HAS_SCRIPTING
#include "aether/script/script_system.h"
#endif

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <cstring>
#include <limits>
#include <set>

// The ropes kit: a Rope becomes a chain of small bodies joined by ball-and-socket joints. Checked on real Jolt bodies (hanging, falling, carrying a weight, being
// cut, rebuilt when edited and taken apart when removed), then in the player with a script.

using namespace aether;
using namespace aether::ropes;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

bool Near(f32 a, f32 b, f32 eps) { return std::fabs(a - b) <= eps; }

struct Sim {
    JobSystem jobs{2};
    World world;
    PhysicsWorld physics{jobs};
    PhysicsScene scene{world, physics};
    RopeSystem system{world, scene};

    Sim() {
        RegisterPhysicsComponentSerializers();
        RegisterRopeComponents();
        physics.SetGravity(Vec3(0, -9.81f, 0));
    }
    void Ground() {
        BoxCollider ground;
        ground.half_extents = Vec3(100, 0.5f, 100);
        world.CreateEntity(Transform{Vec3(0, -0.5f, 0), Quaternion::Identity()}, ground);
    }
    Entity MakeRope(const Rope& r, const Vec3& at = Vec3(0, 10, 0)) { return world.CreateEntity(Transform{at, Quaternion::Identity()}, r); }
    // A 40 kg crate with an identity, so a rope can name it.
    Entity Crate(const Vec3& at, EntityGuid& guid_out, f32 mass = 40.0f) {
        RigidBody rb;
        rb.mass = mass;
        rb.linear_damping = 0.0f;
        BoxCollider box;
        box.half_extents = Vec3(0.5f, 0.5f, 0.5f);
        guid_out = NewEntityGuid();
        return world.CreateEntity(Transform{at, Quaternion::Identity()}, rb, box, IdComponent{guid_out});
    }
    void Run(int steps) {
        for (int i = 0; i < steps; ++i) {
            system.Apply();
            scene.Step(kDt);
        }
    }
    Vec3 Position(Entity e) { return world.GetComponent<Transform>(e)->position; }
};

Rope Hanging(f32 length = 5.0f, u8 segments = 10) {
    Rope r;
    r.length = length;
    r.segments = segments;
    r.damping = 3.0f; // heavy air drag: it settles quickly
    return r;
}

} // namespace

AETHER_TEST(Ropes_ComponentsRegisterAndRoundTrip) {
    RegisterRopeComponents();
    CHECK(FindComponentIdByName("Rope") != kInvalidComponentId && FindComponentIdByName("RopeSegment") != kInvalidComponentId);
    Rope r;
    r.enabled = false;
    r.length = 12.5f;
    r.segments = 30;
    r.direction = Vec3(1, 0, 0);
    r.radius = 0.2f;
    r.mass = 9.0f;
    r.damping = 0.7f;
    r.pin_start = false;
    r.start_entity = NewEntityGuid();
    r.end_entity = NewEntityGuid();
    r.end_anchor = Vec3(0, 0.5f, 0);
    std::strncpy(r.segment_model, "Models/chain.glb", sizeof(r.segment_model) - 1);
    Rope back;
    CHECK(reflect::FromJson(back, reflect::ToJson(r)) && !back.enabled && Near(back.length, 12.5f, 1e-5f) && back.segments == 30 && Near(back.direction.x, 1.0f, 1e-6f));
    CHECK(Near(back.radius, 0.2f, 1e-6f) && Near(back.mass, 9.0f, 1e-5f) && Near(back.damping, 0.7f, 1e-6f) && !back.pin_start);
    CHECK(back.start_entity == r.start_entity && back.end_entity == r.end_entity && Near(back.end_anchor.y, 0.5f, 1e-6f) && std::string(back.segment_model) == "Models/chain.glb");
    Rope loaded; // saved with nothing: a 5 m rope of 12 segments hanging straight down from the world
    CHECK(reflect::FromJson(loaded, reflect::Json::object()) && loaded.enabled && Near(loaded.length, 5.0f, 1e-6f) && loaded.segments == 12 && loaded.pin_start && loaded.end_entity.IsNull());
}

AETHER_TEST(Ropes_ARopeBecomesAChainOfJointedSegments) {
    Sim s;
    const Entity rope = s.MakeRope(Hanging(5.0f, 10));
    s.Run(1);
    CHECK(s.system.RopeCount() == 1);
    const std::vector<Entity> segs = s.system.Segments(rope);
    CHECK(segs.size() == 10);
    if (segs.size() != 10) return;
    std::set<u64> guids;
    for (u32 i = 0; i < 10; ++i) {
        const RopeSegment* seg = s.world.GetComponent<RopeSegment>(segs[i]);
        const PhysicsJoint* joint = s.world.GetComponent<PhysicsJoint>(segs[i]);
        const IdComponent* id = s.world.GetComponent<IdComponent>(segs[i]);
        const RigidBody* rb = s.world.GetComponent<RigidBody>(segs[i]);
        CHECK(seg != nullptr && joint != nullptr && id != nullptr && rb != nullptr && s.world.GetComponent<SphereCollider>(segs[i]) != nullptr);
        if (!seg || !joint || !id || !rb) return;
        CHECK(seg->index == i && joint->type == JointType::Point && !id->guid.IsNull() && Near(rb->mass, 0.2f, 1e-5f)); // 2 kg shared by 10
        guids.insert(id->guid.lo);
        // Each is joined to the one before (the first to the world: a null guid).
        if (i == 0) CHECK(joint->connected.IsNull());
        else CHECK(joint->connected == s.world.GetComponent<IdComponent>(segs[i - 1])->guid);
    }
    CHECK(guids.size() == 10); // all different
    CHECK(s.scene.JointCount() == 10); // and the physics scene made a Jolt constraint for each
    // Laid out down from the rope's position, half a segment apart at the ends: centres at y = 10 - 0.25, 10 - 0.75, ...
    CHECK(Near(s.Position(segs[0]).y, 9.75f, 0.05f) && Near(s.Position(segs[9]).y, 5.25f, 0.2f));
    CHECK(Near(s.system.CurrentLength(rope), 5.0f, 0.3f));
    // Segment positions as the script API gives them.
    Vec3 p;
    CHECK(s.system.SegmentPosition(rope, 3, p) && Near(p.y, s.Position(segs[3]).y, 1e-5f) && !s.system.SegmentPosition(rope, 10, p) && !s.system.SegmentPosition(kNullEntity, 0, p));
}

AETHER_TEST(Ropes_ASidewaysRopeSwingsDownAndHangsFromItsPin) {
    Sim s;
    Rope r = Hanging(5.0f, 10);
    r.direction = Vec3(1, 0, 0); // starts out level
    const Entity rope = s.MakeRope(r);
    s.Run(1);
    const std::vector<Entity> segs = s.system.Segments(rope);
    CHECK(segs.size() == 10);
    if (segs.size() != 10) return;
    CHECK(Near(s.Position(segs[9]).y, 10.0f, 0.1f) && s.Position(segs[9]).x > 4.0f); // level at first
    s.Run(420);
    // Hanging: the free end is straight below the pin, about a rope's length down; the pinned end has stayed at the pin.
    const Vec3 end = s.Position(segs[9]), top = s.Position(segs[0]);
    CHECK(Near(end.x, 0.0f, 0.6f) && end.y > 4.6f && end.y < 5.8f);
    CHECK((top - Vec3(0, 10, 0)).Length() < 0.6f);
    CHECK(std::fabs(s.system.CurrentLength(rope) - 5.0f) < 0.9f); // it has not stretched far
}

AETHER_TEST(Ropes_AnUnpinnedRopeFalls) {
    Sim s;
    Rope r = Hanging(4.0f, 8);
    r.pin_start = false;
    r.damping = 0.2f; // ordinary air: the heavy drag the other tests use would hold a fall to about 3 m/s
    const Entity rope = s.MakeRope(r);
    s.Run(1);
    const std::vector<Entity> segs = s.system.Segments(rope);
    CHECK(segs.size() == 8);
    if (segs.size() != 8) return;
    CHECK(s.world.GetComponent<PhysicsJoint>(segs[0]) == nullptr && s.world.GetComponent<PhysicsJoint>(segs[1]) != nullptr); // no joint to the world
    s.Run(60);
    CHECK(s.Position(segs[0]).y < 6.0f); // a second of falling: about 4.9 m down
}

AETHER_TEST(Ropes_ARopeCarriesAWeightAndCuttingItDropsIt) {
    // A 4 m rope from the ceiling point (0, 10, 0) with a 40 kg crate tied to the end (the crate's top is where the rope ends: y = 6).
    Sim held;
    held.Ground();
    EntityGuid crate_guid;
    const Entity crate = held.Crate(Vec3(0, 5.5f, 0), crate_guid);
    Rope r = Hanging(4.0f, 8);
    r.end_entity = crate_guid;
    r.end_anchor = Vec3(0, 0.5f, 0);
    const Entity rope = held.MakeRope(r);
    held.Run(240);
    CHECK(held.world.GetComponent<PhysicsJoint>(crate) != nullptr); // the rope gave the crate its joint
    CHECK(held.Position(crate).y > 4.0f); // held up, not on the floor (it stretches a little under 40 kg)
    // Cut the middle: the crate falls to the ground and the top half stays at the pin.
    CHECK(held.system.Cut(rope, 0.5f));
    CHECK(!held.system.Cut(rope, 0.5f)); // the same place twice: already cut
    held.Run(240);
    CHECK(held.Position(crate).y < 1.5f);
    CHECK(held.Position(held.system.Segments(rope)[0]).y > 8.5f);
    // Disabling the rope takes it apart: its segments go and so does the joint it gave the crate.
    held.world.GetComponent<Rope>(rope)->enabled = false;
    held.Run(1);
    CHECK(held.system.RopeCount() == 0 && held.world.GetComponent<PhysicsJoint>(crate) == nullptr && held.system.Segments(rope).empty());

    // The control: no rope, and the same crate just falls.
    Sim loose;
    loose.Ground();
    EntityGuid g;
    const Entity dropped = loose.Crate(Vec3(0, 5.5f, 0), g);
    loose.Run(240);
    CHECK(loose.Position(dropped).y < 1.0f);
}

// A rope that has hung at rest for a while is asleep to the physics engine; cutting it must still let the lower half fall (taking a joint away does not wake a
// body by itself).
AETHER_TEST(Ropes_CuttingASettledRopeWakesItAndTheLowerHalfFalls) {
    Sim s;
    const Entity rope = s.MakeRope(Hanging(5.0f, 10));
    s.Run(600); // ten seconds: long since still
    const std::vector<Entity> segs = s.system.Segments(rope);
    CHECK(segs.size() == 10);
    if (segs.size() != 10) return;
    const f32 before = s.Position(segs[9]).y;
    CHECK(before > 4.6f && before < 5.8f);
    CHECK(s.system.Cut(rope, 0.5f));
    s.Run(120);
    CHECK(s.Position(segs[9]).y < before - 2.0f);       // the free end has fallen away
    CHECK(s.Position(segs[0]).y > 9.0f);               // the pinned end has not
}

AETHER_TEST(Ropes_CutRefusesWhatItCannotCut) {
    Sim s;
    const Entity rope = s.MakeRope(Hanging());
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    CHECK(!s.system.Cut(rope, 0.5f)); // not built yet
    s.Run(1);
    CHECK(!s.system.Cut(rope, nan) && !s.system.Cut(kNullEntity, 0.5f) && !s.system.Cut(s.world.CreateEntity(Transform{Vec3(0, 0, 0), Quaternion::Identity()}), 0.5f));
    CHECK(s.system.Cut(rope, 0.0f) && s.system.Cut(rope, 1.0f) && s.system.Cut(rope, 5.0f) == false); // 0 and 1 are the two ends; 5 clamps to 1, already cut
    // After a rebuild (an edit) the rope is whole again and can be cut again.
    s.world.GetComponent<Rope>(rope)->segments = 6;
    s.Run(1);
    CHECK(s.system.Segments(rope).size() == 6 && s.system.Cut(rope, 0.0f));
}

AETHER_TEST(Ropes_EditingDisablingAndDestroyingTearDownTheOldSegments) {
    Sim s;
    const Entity rope = s.MakeRope(Hanging(5.0f, 6));
    s.Run(1);
    const std::vector<Entity> old = s.system.Segments(rope);
    CHECK(old.size() == 6);
    const usize with_rope = s.world.EntityCount();
    s.world.GetComponent<Rope>(rope)->segments = 12;
    s.Run(1);
    const std::vector<Entity> now = s.system.Segments(rope);
    CHECK(now.size() == 12 && s.system.RopeCount() == 1);
    for (const Entity e : old) CHECK(!s.world.IsAlive(e)); // the old chain is gone
    CHECK(s.world.EntityCount() == with_rope + 6);          // 6 fewer old, 12 new
    // Moving the rope entity afterwards does not rebuild it (a rope is made where it is found).
    s.world.GetComponent<Transform>(rope)->position = Vec3(50, 10, 0);
    s.Run(1);
    CHECK(s.system.Segments(rope) == now);
    // Destroying the rope entity removes its segments.
    s.world.DestroyEntity(rope);
    s.Run(1);
    CHECK(s.system.RopeCount() == 0);
    for (const Entity e : now) CHECK(!s.world.IsAlive(e));
}

AETHER_TEST(Ropes_BadSettingsMakeNoRopeAndSegmentsAreClamped) {
    Sim s;
    const usize before = s.world.EntityCount();
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    Rope bad_length = Hanging();
    bad_length.length = 0.0f;
    Rope nan_length = Hanging();
    nan_length.length = nan;
    Rope no_mass = Hanging();
    no_mass.mass = -1.0f;
    Rope no_direction = Hanging();
    no_direction.direction = Vec3(0, 0, 0);
    Rope nan_radius = Hanging();
    nan_radius.radius = nan;
    for (const Rope& r : {bad_length, nan_length, no_mass, no_direction, nan_radius}) s.MakeRope(r);
    s.Run(5);
    CHECK(s.system.RopeCount() == 0 && s.world.EntityCount() == before + 5); // five rope entities, no segments
    Sim t;
    Rope many = Hanging(5.0f, 200);
    Rope few = Hanging(5.0f, 0);
    const Entity a = t.MakeRope(many, Vec3(0, 10, 0)), b = t.MakeRope(few, Vec3(20, 10, 0));
    t.Run(1);
    CHECK(t.system.Segments(a).size() == 64 && t.system.Segments(b).size() == 2);
    // A radius larger than the segments leave room for is held to 45% of a segment's length.
    Sim u;
    Rope fat = Hanging(1.0f, 10);
    fat.radius = 5.0f;
    const Entity c = u.MakeRope(fat);
    u.Run(1);
    const std::vector<Entity> segs = u.system.Segments(c);
    CHECK(segs.size() == 10 && Near(u.world.GetComponent<SphereCollider>(segs[0])->radius, 0.045f, 1e-5f));
}

AETHER_TEST(Ropes_ARopePinnedToABodyFollowsIt) {
    Sim s;
    RigidBody rb;
    rb.mass = 500.0f;
    rb.gravity_scale = 0.0f;
    rb.linear_damping = 0.0f;
    BoxCollider box;
    box.half_extents = Vec3(0.5f, 0.5f, 0.5f);
    const EntityGuid block_guid = NewEntityGuid();
    const Entity block = s.world.CreateEntity(Transform{Vec3(0, 10, 0), Quaternion::Identity()}, rb, box, IdComponent{block_guid});
    Rope r = Hanging(3.0f, 6);
    r.start_entity = block_guid;
    const Entity rope = s.MakeRope(r, Vec3(0, 9.5f, 0)); // from the block's underside
    s.Run(2);
    s.physics.BodyInterface().SetLinearVelocity(s.scene.BodyOf(block), JPH::Vec3(3, 0, 0));
    s.Run(60);
    const std::vector<Entity> segs = s.system.Segments(rope);
    CHECK(segs.size() == 6);
    if (segs.size() != 6) return;
    CHECK(s.Position(block).x > 2.0f);                                   // the block has moved on
    CHECK(std::fabs(s.Position(segs[0]).x - s.Position(block).x) < 0.8f); // and the pinned end went with it
}

AETHER_TEST(Ropes_TheKitRegistersTheComponentsAndInstallsTheScriptTable) {
    const std::unique_ptr<kit::IKit> kit = MakeRopesKit();
    CHECK(std::string(kit->Name()) == "Ropes" && kit->Deps().empty() && kit->Stages().empty());
    Sim s;
    kit::KitRuntimeContext context;
    context.world = &s.world;
    CHECK(kit->CreateRuntime(context) == nullptr);
    kit->RegisterComponents();
    CHECK(FindComponentIdByName("Rope") != kInvalidComponentId);
    std::string installed;
    kit::KitScriptApiContext api;
    api.install = [&](const std::string& name) { installed = name; };
    kit->InstallScriptApi(&api);
    CHECK(installed == "Ropes");
    CHECK(RopeSystem::Active() == &s.system);
}

namespace {

nlohmann::json AssetEntry(const char* path, const char* importer) {
    return {{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", path}, {"importer", importer}};
}

} // namespace

#if AETHER_TEST_HAS_SCRIPTING
// The real player: a rope in the scene is built by the physics step, a script reads it (Ropes.Segments, Length, SegmentPosition) and cuts it (Ropes.Cut).
AETHER_TEST(Ropes_PlayerBuildsARopeAndAScriptReadsAndCutsIt) {
    if (!player::Game::HasPhysics() || !player::Game::HasScripting()) return;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_ropes_player_tests";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string script_guid = assets::ToString(assets::NewAssetGuid());
    ScriptComponent component;
    assets::AssetGuid parsed;
    CHECK(assets::ParseAssetGuid(script_guid, parsed));
    component.script.guid = parsed;
    const auto at = [](f32 x, f32 y, f32 z) { return reflect::ToJson(Transform{Vec3(x, y, z), Quaternion::Identity()}); };
    const nlohmann::json scene = {{"$type", "Scene"}, {"$version", 1}, {"entities", nlohmann::json::array({
        {{"components", {{"Transform", at(0, 10, 0)}, {"Rope", reflect::ToJson(Hanging(5.0f, 10))}, {"ScriptComponent", reflect::ToJson(component)}, {"Tags", reflect::Json{{"names", {"Rope"}}}}}}}})}};
    const std::string script =
        "local S = {}\nfunction S:OnCreate()\n  self.t = 0\nend\n"
        "function S:OnUpdate(dt)\n  self.t = self.t + dt\n"
        "  local n = Ropes.Segments(self.entity)\n  SaveGames.SetNumber('n', n)\n"
        "  if n == 0 then return end\n"
        "  SaveGames.SetNumber('len', Ropes.Length(self.entity))\n"
        "  local p = Ropes.SegmentPosition(self.entity, n - 1)\n  SaveGames.SetNumber('end_y', p.y)\n"
        "  if self.t > 1.0 and not self.cut then\n    self.cut = true\n    SaveGames.SetNumber('end_y_before', p.y)\n    SaveGames.SetNumber('cut', Ropes.Cut(self.entity, 0.5) and 1 or 0)\n  end\n"
        "  if self.cut then SaveGames.SetNumber('end_y_after', p.y) end\nend\nreturn S\n";
    const nlohmann::json assets_json = nlohmann::json::array({AssetEntry("Scenes/start.ascene", "Scene"), {{"guid", script_guid}, {"path", "Scripts/Rope.luau"}, {"importer", "Script"}}});
    const nlohmann::json manifest = {{"$type", "CookManifest"}, {"$version", 1}, {"project", "Demo"}, {"configuration", "Development"},
                                     {"startup_scene", "Scenes/start.ascene"}, {"fixed_timestep_hz", 60.0}, {"gravity", {0.0, -9.81, 0.0}},
                                     {"layers", {"Default"}}, {"collision_matrix", nlohmann::json::array()}, {"assets", assets_json}, {"files", nlohmann::json::array()}};
    pak::PakWriter writer;
    CHECK(writer.Add("Manifest.json", manifest.dump(2)));
    CHECK(writer.Add("Content/Scenes/start.ascene", scene.dump()));
    CHECK(writer.Add("Content/Scripts/Rope.luau", script));
    const std::filesystem::path file = dir / "Game.apak";
    CHECK(writer.Write(file.string()));
    player::GamePackage package;
    std::string error;
    CHECK(package.Mount(file.string(), 0, &error) && package.LoadManifest(&error));
    player::Game game(package);
    game.SetUserPaths(player::ResolveUserPaths("Demo", dir / "user"));
    CHECK(game.LoadStartupScene(&error));
    game.BeginPlay();
    CHECK(game.Saves() != nullptr && game.PhysicsQueries() != nullptr);
    if (!game.Saves() || !game.PhysicsQueries()) return;
    for (int i = 0; i < 60; ++i) game.Tick(kDt);
    CHECK(game.ScriptErrors().empty());
    const save::SaveBag& bag = game.Saves()->Bag();
    CHECK(bag.GetInt("n") == 10);
    CHECK(Near(bag.GetFloat("len"), 5.0f, 0.4f));
    CHECK(bag.GetFloat("end_y") > 4.5f && bag.GetFloat("end_y") < 5.6f); // hanging below the pin at y = 10
    for (int i = 0; i < 180; ++i) game.Tick(kDt);
    CHECK(game.ScriptErrors().empty() && bag.GetInt("cut") == 1);
    CHECK(bag.GetFloat("end_y_after") < bag.GetFloat("end_y_before") - 2.0f); // the cut let the lower half fall
}
#endif

#endif
