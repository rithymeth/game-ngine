#include "test_framework.h"

#ifdef AETHER_TEST_HAS_DESTRUCTION

#include "aether/assets/asset_guid.h"
#include "aether/destruction/destruction_kit.h"
#include "aether/destruction/destruction_system.h"
#include "aether/ecs/world.h"
#include "aether/job/job_system.h"
#include "aether/pak/pak.h"
#include "aether/physics/components.h"
#include "aether/physics/physics_scene.h"
#include "aether/player/game.h"
#include "aether/player/user_paths.h"
#include "aether/save/save_system.h"
#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"
#if AETHER_TEST_HAS_SCRIPTING
#include "aether/script/script_system.h"
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>

// The destruction kit: a Destructible with a box collider breaks into a grid of physics pieces on a hard hit, on enough damage or on demand, and the pieces
// are debris that expires and is capped. Checked on real Jolt bodies, then in the player with a script.

using namespace aether;
using namespace aether::destruction;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

bool Near(f32 a, f32 b, f32 eps) { return std::fabs(a - b) <= eps; }

struct Sim {
    JobSystem jobs{2};
    World world;
    PhysicsWorld physics{jobs};
    PhysicsScene scene{world, physics};
    DestructionSystem system{world, scene};

    explicit Sim(Vec3 gravity = Vec3(0, 0, 0)) {
        RegisterPhysicsComponentSerializers();
        RegisterDestructionComponents();
        physics.SetGravity(gravity);
        scene.SetEventHandler([this](const PhysicsEvent& e) { system.OnEvent(e); });
    }
    // A 2 x 2 x 2 m crate: static unless `motion` says otherwise.
    Entity Crate(const Vec3& at, Destructible d = {}, bool with_model = false, const Vec3& scale = Vec3(1, 1, 1)) {
        BoxCollider box;
        box.half_extents = Vec3(1, 1, 1);
        const Entity e = world.CreateEntity(Transform{at, Quaternion::Identity(), scale}, box, d);
        if (with_model) {
            ModelRenderer m;
            SetModelPath(m, "Models/crate.gltf");
            world.AddComponent<ModelRenderer>(e, m);
        }
        return e;
    }
    Entity Ball(const Vec3& at, const Vec3& velocity, f32 mass = 5.0f) {
        RigidBody rb;
        rb.mass = mass;
        rb.linear_damping = 0.0f;
        SphereCollider sphere;
        sphere.radius = 0.5f;
        const Entity e = world.CreateEntity(Transform{at, Quaternion::Identity()}, rb, sphere);
        scene.Sync();
        physics.BodyInterface().SetLinearVelocity(scene.BodyOf(e), JPH::Vec3(velocity.x, velocity.y, velocity.z));
        return e;
    }
    std::vector<Entity> Pieces() {
        std::vector<Entity> out;
        const ComponentId id = GetComponentId<Debris>();
        world.ForEachArchetype([&](const Archetype& a) {
            if (!a.Mask().test(id)) return;
            for (usize c = 0; c < a.ChunkCount(); ++c) {
                const Entity* chunk = a.EntityArray(c);
                out.insert(out.end(), chunk, chunk + a.ChunkEntityCount(c));
            }
        });
        return out;
    }
    Vec3 Velocity(Entity e) {
        const JPH::Vec3 v = physics.BodyInterface().GetLinearVelocity(scene.BodyOf(e));
        return Vec3(v.GetX(), v.GetY(), v.GetZ());
    }
    void Run(int steps) {
        scene.Sync();
        for (int i = 0; i < steps; ++i) {
            system.Update(kDt);
            scene.Step(kDt);
        }
    }
};

Destructible Brittle() {
    Destructible d;
    d.break_speed = 4.0f;
    d.burst_speed = 0.0f;
    return d;
}

} // namespace

AETHER_TEST(Destruction_ComponentsRegisterAndRoundTrip) {
    RegisterDestructionComponents();
    CHECK(FindComponentIdByName("Destructible") != kInvalidComponentId && FindComponentIdByName("Debris") != kInvalidComponentId);
    Destructible d;
    d.enabled = false;
    d.health = 40.0f;
    d.break_speed = 9.0f;
    d.pieces_x = 2;
    d.pieces_y = 4;
    d.pieces_z = 5;
    d.mass = 33.0f;
    d.burst_speed = 7.0f;
    d.inherit_velocity = 0.25f;
    d.piece_lifetime = 12.0f;
    const reflect::Json saved = reflect::ToJson(d);
    Destructible back;
    CHECK(reflect::FromJson(back, saved) && !back.enabled && Near(back.health, 40.0f, 1e-5f) && Near(back.break_speed, 9.0f, 1e-5f));
    CHECK(back.pieces_x == 2 && back.pieces_y == 4 && back.pieces_z == 5 && Near(back.mass, 33.0f, 1e-5f) && Near(back.burst_speed, 7.0f, 1e-5f));
    CHECK(Near(back.inherit_velocity, 0.25f, 1e-5f) && Near(back.piece_lifetime, 12.0f, 1e-5f));
    // Saved with nothing: 3 x 3 x 3, on, breaks from a hit of 6 m/s.
    Destructible loaded;
    CHECK(reflect::FromJson(loaded, reflect::Json::object()) && loaded.enabled && loaded.pieces_x == 3 && Near(loaded.break_speed, 6.0f, 1e-5f));
}

AETHER_TEST(Destruction_PieceCountIsHeldToTheGrid) {
    Destructible d;
    CHECK(PieceCount(d) == 27);
    d.pieces_x = 0; // not a grid: one piece along the axis
    d.pieces_y = 1;
    d.pieces_z = 2;
    CHECK(PieceCount(d) == 2);
    d.pieces_x = 255;
    d.pieces_y = 255;
    d.pieces_z = 255;
    CHECK(PieceCount(d) == 216);
}

AETHER_TEST(Destruction_ABreakReplacesTheEntityWithAGridOfPieces) {
    Sim s;
    Destructible d = Brittle();
    d.pieces_x = 2;
    d.pieces_y = 3;
    d.pieces_z = 1;
    d.mass = 12.0f;
    const Entity crate = s.Crate(Vec3(10, 5, 0), d, true, Vec3(1, 1, 1));
    s.scene.Sync();
    const std::vector<Entity> pieces = s.system.Break(crate, Vec3(10, 5, 0));
    CHECK(!s.world.IsAlive(crate));
    CHECK(pieces.size() == 6);
    if (pieces.size() != 6) return;
    f32 mass = 0.0f;
    Vec3 lo(1e9f, 1e9f, 1e9f), hi(-1e9f, -1e9f, -1e9f);
    for (const Entity p : pieces) {
        const Transform* t = s.world.GetComponent<Transform>(p);
        const RigidBody* rb = s.world.GetComponent<RigidBody>(p);
        const BoxCollider* box = s.world.GetComponent<BoxCollider>(p);
        const ModelRenderer* m = s.world.GetComponent<ModelRenderer>(p);
        CHECK(t != nullptr && rb != nullptr && box != nullptr && m != nullptr && s.world.GetComponent<Debris>(p) != nullptr);
        if (!t || !rb || !box || !m) return;
        mass += rb->mass;
        // Each piece is the source shrunk by its share of the axis: 1/2, 1/3, 1/1 of the scale; the model is the source's.
        CHECK(Near(t->scale.x, 0.5f, 1e-5f) && Near(t->scale.y, 1.0f / 3.0f, 1e-5f) && Near(t->scale.z, 1.0f, 1e-5f));
        CHECK(std::string(m->asset_path) == "Models/crate.gltf");
        lo = Vec3(std::min(lo.x, t->position.x), std::min(lo.y, t->position.y), std::min(lo.z, t->position.z));
        hi = Vec3(std::max(hi.x, t->position.x), std::max(hi.y, t->position.y), std::max(hi.z, t->position.z));
    }
    CHECK(Near(mass, 12.0f, 1e-4f));
    // The centres span the crate less half a piece on each side: x 10 +- 0.5, y 5 +- 2/3, z 0.
    CHECK(Near(lo.x, 9.5f, 1e-4f) && Near(hi.x, 10.5f, 1e-4f) && Near(lo.y, 5.0f - 2.0f / 3.0f, 1e-4f) && Near(hi.y, 5.0f + 2.0f / 3.0f, 1e-4f));
    CHECK(Near(lo.z, 0.0f, 1e-4f) && Near(hi.z, 0.0f, 1e-4f));
    // Real bodies, and the source's body is gone after the next sync.
    s.scene.Sync();
    for (const Entity p : pieces) CHECK(!s.scene.BodyOf(p).IsInvalid());
    CHECK(s.system.DebrisCount() == 6);
}

AETHER_TEST(Destruction_PiecesBurstOutwardAndKeepTheirShareOfTheMotion) {
    Sim s;
    Destructible d = Brittle();
    d.pieces_x = 2;
    d.pieces_y = 1;
    d.pieces_z = 1;
    d.burst_speed = 4.0f;
    d.inherit_velocity = 0.5f;
    const Entity crate = s.Crate(Vec3(0, 0, 0), d);
    // A moving crate: give it a dynamic body that travels at 6 m/s along z.
    RigidBody rb;
    rb.mass = 10.0f;
    rb.linear_damping = 0.0f;
    s.world.AddComponent<RigidBody>(crate, rb);
    s.scene.Sync();
    s.physics.BodyInterface().SetLinearVelocity(s.scene.BodyOf(crate), JPH::Vec3(0, 0, 6));
    const std::vector<Entity> pieces = s.system.Break(crate, Vec3(0, 0, 0));
    CHECK(pieces.size() == 2);
    if (pieces.size() != 2) return;
    const Vec3 a = s.Velocity(pieces[0]), b = s.Velocity(pieces[1]);
    // Pieces at x = -0.5 and +0.5: thrown to -x and +x at 4 m/s, and half the crate's 6 m/s along z.
    CHECK(Near(a.x, -4.0f, 1e-3f) && Near(b.x, 4.0f, 1e-3f));
    CHECK(Near(a.z, 3.0f, 1e-3f) && Near(b.z, 3.0f, 1e-3f) && Near(a.y, 0.0f, 1e-3f));
}

AETHER_TEST(Destruction_DamageWearsHealthDownThenBreaks) {
    Sim s;
    Destructible d = Brittle();
    d.health = 30.0f;
    d.break_speed = 0.0f;
    const Entity crate = s.Crate(Vec3(0, 0, 0), d);
    CHECK(!s.system.Damage(crate, 10.0f)); // 20 left
    CHECK(!s.system.Damage(crate, 10.0f)); // 10 left
    CHECK(s.world.IsAlive(crate) && s.system.Pending() == 0);
    s.Run(1);
    CHECK(s.world.IsAlive(crate) && Near(s.world.GetComponent<Destructible>(crate)->health, 10.0f, 1e-5f));
    CHECK(s.system.Damage(crate, 15.0f)); // spent: it will break
    CHECK(s.system.Pending() == 1 && s.world.IsAlive(crate)); // not yet: nothing changes until Update
    CHECK(s.system.Damage(crate, 1.0f) == false); // no health left to lose; already marked, nothing doubled
    CHECK(s.system.Pending() == 1);
    CHECK(s.system.Update(kDt) == 27);
    CHECK(!s.world.IsAlive(crate) && s.system.Pending() == 0);
}

AETHER_TEST(Destruction_DamageRefusesWhatCannotTakeIt) {
    Sim s;
    Destructible tough = Brittle();
    tough.health = 10.0f;
    const Entity crate = s.Crate(Vec3(0, 0, 0), tough);
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    CHECK(!s.system.Damage(crate, 0.0f) && !s.system.Damage(crate, -5.0f) && !s.system.Damage(crate, nan) && !s.system.Damage(crate, std::numeric_limits<f32>::infinity()));
    CHECK(Near(s.world.GetComponent<Destructible>(crate)->health, 10.0f, 1e-6f) && s.system.Pending() == 0);
    Destructible no_health = Brittle(); // health 0: only a hard hit breaks it
    CHECK(!s.system.Damage(s.Crate(Vec3(5, 0, 0), no_health), 1000.0f));
    Destructible off = Brittle();
    off.health = 5.0f;
    off.enabled = false;
    const Entity disabled = s.Crate(Vec3(9, 0, 0), off);
    CHECK(!s.system.Damage(disabled, 99.0f) && !s.system.Shatter(disabled));
    const Entity plain = s.world.CreateEntity(Transform{Vec3(0, 0, 0), Quaternion::Identity()});
    CHECK(!s.system.Damage(plain, 5.0f) && !s.system.Shatter(plain) && !s.system.Shatter(kNullEntity) && !s.system.Damage(kNullEntity, 5.0f));
    CHECK(s.system.Pending() == 0);
}

AETHER_TEST(Destruction_ShatterBreaksRegardlessAndNeedsABoxCollider) {
    Sim s;
    Destructible d;
    d.health = 1000.0f;
    d.break_speed = 0.0f;
    const Entity crate = s.Crate(Vec3(0, 0, 0), d);
    CHECK(s.system.Shatter(crate) && s.system.Shatter(crate)); // twice is still one break
    CHECK(s.system.Pending() == 1);
    CHECK(s.system.Update(kDt) == 27 && !s.world.IsAlive(crate));
    // A Destructible with no BoxCollider has nothing to cut up: Break makes nothing and leaves it be.
    const Entity ghost = s.world.CreateEntity(Transform{Vec3(0, 0, 0), Quaternion::Identity()}, Destructible{});
    CHECK(s.system.Break(ghost, Vec3(0, 0, 0)).empty() && s.world.IsAlive(ghost));
}

AETHER_TEST(Destruction_AHardHitBreaksAndASoftOneDoesNot) {
    Sim s;
    const Entity hard_target = s.Crate(Vec3(0, 0, 0), Brittle());
    const Entity soft_target = s.Crate(Vec3(0, 0, 20), Brittle());
    // Both balls start 4 m from a face of the crate (its face is at x = 1 / z = 21): a 10 m/s ball arrives at the speed it left at.
    const Entity fast = s.Ball(Vec3(5, 0, 0), Vec3(-10, 0, 0));
    const Entity slow = s.Ball(Vec3(5, 0, 20), Vec3(-2, 0, 0));
    (void)fast;
    (void)slow;
    s.Run(90);
    CHECK(!s.world.IsAlive(hard_target)); // 10 m/s >= 4: broken
    CHECK(s.world.IsAlive(soft_target));  // 2 m/s < 4: still standing
    CHECK(s.Pieces().size() == 27);
}

AETHER_TEST(Destruction_APieceKeepsFallingUnderGravityAndStopsOnTheGround) {
    Sim s(Vec3(0, -9.81f, 0));
    s.scene.Sync();
    BoxCollider ground;
    ground.half_extents = Vec3(50, 0.5f, 50);
    s.world.CreateEntity(Transform{Vec3(0, -0.5f, 0), Quaternion::Identity()}, ground);
    Destructible d = Brittle();
    d.pieces_x = d.pieces_y = d.pieces_z = 2;
    d.burst_speed = 0.0f;
    d.piece_lifetime = 0.0f; // never removed
    const Entity crate = s.Crate(Vec3(0, 6, 0), d);
    s.system.Shatter(crate);
    s.Run(1);
    const std::vector<Entity> pieces = s.Pieces();
    CHECK(pieces.size() == 8);
    if (pieces.empty()) return;
    const f32 start = s.world.GetComponent<Transform>(pieces[0])->position.y;
    s.Run(240);
    for (const Entity p : pieces) {
        const f32 y = s.world.GetComponent<Transform>(p)->position.y;
        CHECK(y < start - 3.0f && y > 0.0f); // fell to the ground, not through it
    }
}

AETHER_TEST(Destruction_DebrisExpiresAndIsCapped) {
    Sim s;
    Destructible d = Brittle();
    d.pieces_x = d.pieces_y = d.pieces_z = 2;
    d.piece_lifetime = 0.5f;
    s.system.Shatter(s.Crate(Vec3(0, 0, 0), d));
    s.Run(1);
    CHECK(s.Pieces().size() == 8);
    s.Run(20);
    CHECK(s.Pieces().size() == 8); // 0.35 s in
    s.Run(15);
    CHECK(s.Pieces().empty()); // 0.6 s: gone

    // A cap: the oldest pieces go first.
    s.system.SetMaxDebris(10);
    Destructible keep = Brittle();
    keep.pieces_x = keep.pieces_y = keep.pieces_z = 2;
    keep.piece_lifetime = 0.0f;
    s.system.Shatter(s.Crate(Vec3(0, 0, 0), keep));
    s.Run(1);
    CHECK(s.Pieces().size() == 8);
    u64 newest_of_first = 0;
    for (const Entity p : s.Pieces()) newest_of_first = std::max(newest_of_first, s.world.GetComponent<Debris>(p)->serial);
    s.system.Shatter(s.Crate(Vec3(20, 0, 0), keep));
    s.Run(1);
    const std::vector<Entity> left = s.Pieces();
    CHECK(left.size() == 10);
    // The ones that went were the first crate's oldest pieces: every survivor is newer than the two removed (serials 9..16 are the second crate's).
    u64 oldest = ~0ull;
    for (const Entity p : left) oldest = std::min(oldest, s.world.GetComponent<Debris>(p)->serial);
    CHECK(oldest > newest_of_first - 6); // the first crate's oldest 6 of 8 went: its last 2 survive beside the second crate's 8
}

AETHER_TEST(Destruction_TheKitRegistersTheComponentsAndInstallsTheScriptTable) {
    const std::unique_ptr<kit::IKit> kit = MakeDestructionKit();
    CHECK(std::string(kit->Name()) == "Destruction" && kit->Deps().empty() && kit->Stages().empty());
    Sim s;
    kit::KitRuntimeContext context;
    context.world = &s.world;
    CHECK(kit->CreateRuntime(context) == nullptr);
    kit->RegisterComponents();
    CHECK(FindComponentIdByName("Destructible") != kInvalidComponentId);
    std::string installed;
    kit::KitScriptApiContext api;
    api.install = [&](const std::string& name) { installed = name; };
    kit->InstallScriptApi(&api);
    CHECK(installed == "Destruction");
    CHECK(DestructionSystem::Active() == &s.system);
}

namespace {

nlohmann::json AssetEntry(const char* path, const char* importer) {
    return {{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", path}, {"importer", importer}};
}

} // namespace

#if AETHER_TEST_HAS_SCRIPTING
// The real player: a fast ball breaks a crate through the physics events, and a script breaks another with Destruction.Damage and a third with Shatter.
AETHER_TEST(Destruction_PlayerBreaksOnImpactAndScriptsDamageAndShatter) {
    if (!player::Game::HasPhysics() || !player::Game::HasScripting()) return;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_destruction_player_tests";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string script_guid = assets::ToString(assets::NewAssetGuid());
    ScriptComponent component;
    assets::AssetGuid parsed;
    CHECK(assets::ParseAssetGuid(script_guid, parsed));
    component.script.guid = parsed;
    Destructible brittle = Brittle();
    Destructible fragile = Brittle();
    fragile.health = 20.0f;
    fragile.break_speed = 0.0f;
    BoxCollider box;
    box.half_extents = Vec3(1, 1, 1);
    RigidBody rb;
    rb.linear_damping = 0.0f;
    SphereCollider sphere;
    sphere.radius = 0.5f;
    const auto at = [](f32 x, f32 y, f32 z) { return reflect::ToJson(Transform{Vec3(x, y, z), Quaternion::Identity()}); };
    const auto crate = [&](f32 x, f32 z, const Destructible& d, const char* tag, bool scripted) {
        nlohmann::json components = {{"Transform", at(x, 0, z)}, {"BoxCollider", reflect::ToJson(box)}, {"Destructible", reflect::ToJson(d)}, {"Tags", reflect::Json{{"names", {tag}}}}};
        if (scripted) components["ScriptComponent"] = reflect::ToJson(component);
        return nlohmann::json{{"components", components}};
    };
    Destructible kicked = Brittle();
    kicked.health = 1000.0f; // the script tells the two scripted crates apart by their health
    kicked.break_speed = 0.0f;
    const nlohmann::json scene = {{"$type", "Scene"}, {"$version", 1}, {"entities", nlohmann::json::array({
        crate(0, 0, brittle, "Hit", false), crate(0, 10, fragile, "Worn", true), crate(0, 20, kicked, "Kicked", true),
        {{"components", {{"Transform", at(6, 0, 0)}, {"RigidBody", reflect::ToJson(rb)}, {"SphereCollider", reflect::ToJson(sphere)}, {"Tags", reflect::Json{{"names", {"Ball"}}}}}}}})}};
    const std::string script =
        "local S = {}\nfunction S:OnCreate()\n  self.t = 0\n  self.step = 0\nend\n"
        "function S:OnUpdate(dt)\n  self.t = self.t + dt\n"
        "  local d = self.entity:Get('Destructible')\n  if d == nil then return end\n"
        "  if d.health >= 1000 then\n    if self.t > 0.1 then SaveGames.SetNumber('s', Destruction.Shatter(self.entity) and 1 or 0) end\n"
        "  elseif self.step == 0 then\n    self.step = 1\n    SaveGames.SetNumber('d1', Destruction.Damage(self.entity, 8) and 1 or 0)\n"
        "  elseif self.step == 1 and self.t > 0.1 then\n    self.step = 2\n    SaveGames.SetNumber('d2', Destruction.Damage(self.entity, 15) and 1 or 0)\n  end\n"
        "  SaveGames.SetNumber('debris', Destruction.Debris())\nend\nreturn S\n";
    const nlohmann::json assets_json = nlohmann::json::array({AssetEntry("Scenes/start.ascene", "Scene"), {{"guid", script_guid}, {"path", "Scripts/Smash.luau"}, {"importer", "Script"}}});
    const nlohmann::json manifest = {{"$type", "CookManifest"}, {"$version", 1}, {"project", "Demo"}, {"configuration", "Development"},
                                     {"startup_scene", "Scenes/start.ascene"}, {"fixed_timestep_hz", 60.0}, {"gravity", {0.0, 0.0, 0.0}},
                                     {"layers", {"Default"}}, {"collision_matrix", nlohmann::json::array()}, {"assets", assets_json}, {"files", nlohmann::json::array()}};
    pak::PakWriter writer;
    CHECK(writer.Add("Manifest.json", manifest.dump(2)));
    CHECK(writer.Add("Content/Scenes/start.ascene", scene.dump()));
    CHECK(writer.Add("Content/Scripts/Smash.luau", script));
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
    // Fire the ball at the first crate at 12 m/s.
    const std::vector<Entity> balls = FindEntitiesWithTag(game.GetWorld(), "Ball");
    CHECK(balls.size() == 1);
    if (balls.size() != 1) return;
    game.Tick(kDt);
    game.PhysicsQueries()->Physics().BodyInterface().SetLinearVelocity(game.PhysicsQueries()->BodyOf(balls[0]), JPH::Vec3(-12, 0, 0));
    for (int i = 0; i < 90; ++i) game.Tick(kDt);
    CHECK(game.ScriptErrors().empty());
    CHECK(FindEntitiesWithTag(game.GetWorld(), "Hit").empty());    // broken by the impact
    CHECK(FindEntitiesWithTag(game.GetWorld(), "Worn").empty());   // 8 then 15 damage on 20 health
    CHECK(FindEntitiesWithTag(game.GetWorld(), "Kicked").empty()); // shattered by the script
    CHECK(game.Saves()->Bag().GetInt("d1") == 0 && game.Saves()->Bag().GetInt("d2") == 1 && game.Saves()->Bag().GetInt("s") == 1);
    // 3 crates x 27 pieces, none expired yet (8 s lifetime).
    usize pieces = 0;
    game.GetWorld().ForEach<Debris>([&](Debris&) { ++pieces; });
    CHECK(pieces == 81);
}
#endif

#endif
