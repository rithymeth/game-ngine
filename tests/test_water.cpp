#include "test_framework.h"

#ifdef AETHER_TEST_HAS_WATER

#include "aether/assets/asset_guid.h"
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
#include "aether/water/water_kit.h"
#include "aether/water/water_system.h"
#if AETHER_TEST_HAS_SCRIPTING
#include "aether/script/script_system.h"
#endif

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <limits>

// The water kit: waves as a function of place and time, floaters that sit where their weight meets the water they displace, rock with the waves, right
// themselves and drift with a current, and boats that drive and turn. Checked on real Jolt bodies, then in the player with a script.

using namespace aether;
using namespace aether::water;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

bool Near(f32 a, f32 b, f32 eps) { return std::fabs(a - b) <= eps; }

struct Sim {
    JobSystem jobs{2};
    World world;
    PhysicsWorld physics{jobs};
    PhysicsScene scene{world, physics};
    WaterSystem system{world, scene};

    Sim() {
        RegisterPhysicsComponentSerializers();
        RegisterWaterComponents();
        physics.SetGravity(Vec3(0, -9.81f, 0));
    }
    Entity Sea(f32 level = 0.0f, f32 wave_height = 0.0f) {
        WaterSurface w;
        w.wave_height = wave_height;
        w.wave_length = 8.0f;
        w.wave_speed = 2.0f;
        return world.CreateEntity(Transform{Vec3(0, level, 0), Quaternion::Identity()}, w);
    }
    // A 2 m by 0.5 m by 2 m raft of `mass` kg (so water of 1000 kg/m^3 carries it with 0.2 of it under at 400 kg).
    Entity Raft(const Vec3& at, f32 mass = 400.0f, const Quaternion& rotation = Quaternion::Identity()) {
        RigidBody rb;
        rb.mass = mass;
        rb.linear_damping = 0.0f;
        BoxCollider box;
        box.half_extents = Vec3(1.0f, 0.25f, 1.0f);
        Floater f;
        f.half_extents = Vec3(1.0f, 0.25f, 1.0f);
        return world.CreateEntity(Transform{at, rotation}, rb, box, f);
    }
    Entity Boat(const Vec3& at, const Boat& boat = water::Boat{}) {
        RigidBody rb;
        rb.mass = 400.0f;
        rb.linear_damping = 0.0f;
        BoxCollider box;
        box.half_extents = Vec3(0.6f, 0.25f, 1.5f);
        Floater f;
        f.half_extents = Vec3(0.6f, 0.25f, 1.5f);
        f.columns_z = 5;
        return world.CreateEntity(Transform{at, Quaternion::Identity()}, rb, box, f, boat);
    }
    void Run(int steps) {
        for (int i = 0; i < steps; ++i) {
            system.Apply(kDt);
            scene.Step(kDt);
        }
    }
    Vec3 Position(Entity e) { return world.GetComponent<Transform>(e)->position; }
    Vec3 Velocity(Entity e) {
        const JPH::Vec3 v = physics.BodyInterface().GetLinearVelocity(scene.BodyOf(e));
        return Vec3(v.GetX(), v.GetY(), v.GetZ());
    }
    Vec3 Up(Entity e) {
        const Quaternion q = world.GetComponent<Transform>(e)->rotation;
        const Vec3 u(q.x, q.y, q.z);
        const Vec3 t = u.Cross(Vec3(0, 1, 0)) * 2.0f;
        return Vec3(0, 1, 0) + t * q.w + u.Cross(t);
    }
    Vec3 Forward(Entity e) {
        const Quaternion q = world.GetComponent<Transform>(e)->rotation;
        const Vec3 u(q.x, q.y, q.z);
        const Vec3 t = u.Cross(Vec3(0, 0, 1)) * 2.0f;
        return Vec3(0, 0, 1) + t * q.w + u.Cross(t);
    }
};

} // namespace

AETHER_TEST(WaterKit_ComponentsRegisterAndRoundTrip) {
    RegisterWaterComponents();
    CHECK(FindComponentIdByName("WaterSurface") != kInvalidComponentId && FindComponentIdByName("Floater") != kInvalidComponentId &&
          FindComponentIdByName("Boat") != kInvalidComponentId);
    WaterSurface w;
    w.infinite = false;
    w.density = 1025.0f;
    w.wave_height = 1.5f;
    w.current_speed = 2.0f;
    const reflect::Json json = reflect::ToJson(w);
    WaterSurface back;
    CHECK(reflect::FromJson(back, json) && !back.infinite && Near(back.density, 1025.0f, 1e-3f) && Near(back.wave_height, 1.5f, 1e-5f) && Near(back.current_speed, 2.0f, 1e-5f));
}

AETHER_TEST(WaterKit_WavesAreAFunctionOfPlaceAndTime) {
    WaterSurface w;
    w.wave_height = 0.5f;
    w.wave_length = 10.0f;
    w.wave_speed = 2.0f;
    w.wave_direction = 0.0f;
    // Flat water is flat, and waves are never higher than the two components together.
    WaterSurface flat = w;
    flat.wave_height = 0.0f;
    CHECK(WaveHeight(flat, 3.0f, 4.0f, 7.0f) == 0.0f);
    f32 lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 400; ++i) {
        const f32 h = WaveHeight(w, static_cast<f32>(i) * 0.37f, static_cast<f32>(i) * 0.11f, static_cast<f32>(i) * 0.05f);
        lo = std::min(lo, h);
        hi = std::max(hi, h);
    }
    CHECK(hi <= 0.5f * 1.4f + 1e-4f && lo >= -0.5f * 1.4f - 1e-4f && hi > 0.3f && lo < -0.3f);
    // The first wave travels: at t the shape is what it was at 0, moved by speed*t along the direction. The second, shorter wave travels slower, so compare
    // only the part the first one makes by using a surface with that alone.
    WaterSurface solo = w;
    solo.wave_height = 0.5f;
    const f32 a = WaveHeight(solo, 1.0f, 0.0f, 0.0f), b = WaveHeight(solo, 1.0f + 2.0f * 1.5f, 0.0f, 1.5f);
    CHECK(Near(a, b, 0.22f)); // within the small, slower component's share
    // Never a NaN from odd settings.
    WaterSurface odd = w;
    odd.wave_length = 0.0f;
    CHECK(WaveHeight(odd, 1.0f, 1.0f, 1.0f) == 0.0f);
}

AETHER_TEST(WaterKit_ASurfaceIsFoundWhereItCoversAndABoundedOneWins) {
    Sim s;
    SurfaceSample sample;
    CHECK(!s.system.SurfaceAt(0.0f, 0.0f, sample)); // no water yet
    CHECK(s.system.HeightAt(0.0f, 0.0f) == std::numeric_limits<f32>::lowest());
    s.Sea(0.0f);
    WaterSurface pond;
    pond.infinite = false;
    pond.half_extents = Vec3(5.0f, 0.0f, 5.0f);
    pond.density = 1025.0f;
    pond.wave_height = 0.0f;
    s.world.CreateEntity(Transform{Vec3(0, 3, 0), Quaternion::Identity()}, pond);
    CHECK(s.system.SurfaceAt(0.0f, 0.0f, sample) && Near(sample.height, 3.0f, 1e-4f) && Near(sample.density, 1025.0f, 1e-3f)); // the pond, over the sea
    CHECK(s.system.SurfaceAt(20.0f, 0.0f, sample) && Near(sample.height, 0.0f, 1e-4f));                                       // outside it, the sea
    CHECK(!s.system.SurfaceAt(std::numeric_limits<f32>::quiet_NaN(), 0.0f, sample));
}

AETHER_TEST(WaterKit_ABodyFloatsWhereItsWeightMeetsTheWaterItDisplaces) {
    Sim s;
    s.Sea(0.0f);
    const Entity raft = s.Raft(Vec3(0, 1.0f, 0), 400.0f);
    s.Run(60 * 10);
    const FloaterState state = s.system.StateOf(raft);
    CHECK(state.in_water);
    // 400 kg on a 2 m x 2 m hull in water of 1000 kg/m^3: 0.1 m of a 0.5 m hull is under, so 0.2 of it, and its middle 0.15 m above the surface.
    CHECK(Near(state.submersion, 0.2f, 0.06f));
    CHECK(Near(s.Position(raft).y, 0.15f, 0.07f));
    CHECK(std::fabs(s.Velocity(raft).y) < 0.2f); // and at rest
}

AETHER_TEST(WaterKit_ABodyHeavierThanItsHullCanDisplaceSinks) {
    Sim s;
    s.Sea(0.0f);
    const Entity rock = s.Raft(Vec3(0, 0.5f, 0), 3000.0f); // the hull holds 2000 kg of water at most
    s.Run(60 * 5);
    CHECK(s.Position(rock).y < -3.0f);
    const Entity wood = s.Raft(Vec3(10, 0.5f, 0), 300.0f);
    s.Run(60 * 6);
    CHECK(s.Position(wood).y > -0.2f); // the light one is held up
}

AETHER_TEST(WaterKit_ATippedBodyRollsBackUpright) {
    Sim s;
    s.Sea(0.0f);
    const Entity raft = s.Raft(Vec3(0, 0.3f, 0), 400.0f, Quaternion::FromAxisAngle(Vec3(0, 0, 1), 0.6f)); // 34 degrees over
    CHECK(s.Up(raft).y < 0.9f);
    s.Run(60 * 10);
    CHECK(s.Up(raft).y > 0.98f);
}

AETHER_TEST(WaterKit_WavesRockABodyAndACurrentCarriesIt) {
    Sim s;
    const Entity sea = s.Sea(0.0f, 0.6f);
    const Entity raft = s.Raft(Vec3(0, 1.0f, 0), 400.0f);
    s.Run(60 * 4);
    f32 lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 60 * 8; ++i) {
        s.Run(1);
        lo = std::min(lo, s.Position(raft).y);
        hi = std::max(hi, s.Position(raft).y);
    }
    CHECK(hi - lo > 0.25f); // it rides the swell

    // A current on calm water: the body is carried, and goes towards the current's speed.
    WaterSurface* w = s.world.GetComponent<WaterSurface>(sea);
    w->wave_height = 0.0f;
    w->current_speed = 2.0f;
    w->current_direction = 0.0f;
    const Entity drifter = s.Raft(Vec3(30, 1.0f, 0), 400.0f);
    s.Run(60 * 12);
    CHECK(s.Velocity(drifter).x > 1.0f && s.Velocity(drifter).x < 2.3f);
    CHECK(Near(s.Velocity(drifter).z, 0.0f, 0.3f));
    SurfaceSample sample;
    CHECK(s.system.SurfaceAt(0.0f, 0.0f, sample) && Near(sample.current.x, 2.0f, 1e-4f) && Near(sample.current.z, 0.0f, 1e-4f));
}

AETHER_TEST(WaterKit_OnlyBodiesOverTheSurfaceAreFloatedAndDisabledOnesSink) {
    Sim s;
    WaterSurface pond;
    pond.infinite = false;
    pond.half_extents = Vec3(5.0f, 0.0f, 5.0f);
    pond.wave_height = 0.0f;
    s.world.CreateEntity(Transform{Vec3(0, 0, 0), Quaternion::Identity()}, pond);
    const Entity inside = s.Raft(Vec3(0, 1.0f, 0));
    const Entity outside = s.Raft(Vec3(20, 1.0f, 0));
    const Entity off = s.Raft(Vec3(0, 1.0f, 3.0f)); // over the pond, but its floating is off
    s.world.GetComponent<Floater>(off)->enabled = false;
    s.Run(60 * 6);
    CHECK(s.Position(inside).y > -0.2f);
    CHECK(s.Position(outside).y < -5.0f);
    CHECK(s.Position(off).y < -5.0f);
    CHECK(!s.system.StateOf(outside).in_water);
}

AETHER_TEST(WaterKit_ABoatDrivesAndTurnsAndNeedsItsPropellerInTheWater) {
    // Straight ahead.
    {
        Sim s;
        s.Sea(0.0f);
        Boat b;
        b.thrust = 6000.0f;
        b.propeller_offset = Vec3(0.0f, -0.25f, -1.3f);
        const Entity boat = s.Boat(Vec3(0, 0.5f, 0), b);
        s.Run(60 * 3);
        const f32 z0 = s.Position(boat).z;
        CHECK(s.system.Drive(boat, 1.0f, 0.0f));
        s.Run(60 * 8);
        CHECK(s.Velocity(boat).z > 2.0f);
        CHECK(s.Position(boat).z > z0 + 6.0f);
        CHECK(std::fabs(s.Position(boat).x) < 1.0f);
        // The controls stay until changed; reversing goes the other way.
        CHECK(s.system.Drive(boat, -1.0f, 0.0f));
        s.Run(60 * 14);
        CHECK(s.Velocity(boat).z < 0.5f);
    }
    // Steering right (a turn clockwise seen from above) swings the bow towards -X.
    {
        Sim s;
        s.Sea(0.0f);
        Boat b;
        b.thrust = 6000.0f;
        b.propeller_offset = Vec3(0.0f, -0.25f, -1.3f);
        const Entity boat = s.Boat(Vec3(0, 0.5f, 0), b);
        s.Run(60 * 3);
        CHECK(s.system.Drive(boat, 1.0f, 1.0f));
        s.Run(60 * 3); // a turn takes it right round in the end, so look at it part of the way
        CHECK(s.Forward(boat).x < -0.3f);
        CHECK(s.Velocity(boat).Length() > 1.0f);
    }
    // A propeller above the water does nothing.
    {
        Sim s;
        s.Sea(0.0f);
        Boat b;
        b.thrust = 6000.0f;
        b.propeller_offset = Vec3(0.0f, 2.0f, -1.3f);
        const Entity boat = s.Boat(Vec3(0, 0.5f, 0), b);
        s.Run(60 * 3);
        CHECK(s.system.Drive(boat, 1.0f, 0.0f));
        s.Run(60 * 5);
        CHECK(std::fabs(s.Velocity(boat).z) < 0.3f);
    }
}

AETHER_TEST(WaterKit_DriveRefusesWhatIsNotABoatOrNotANumber) {
    Sim s;
    s.Sea(0.0f);
    const Entity raft = s.Raft(Vec3(0, 1, 0));
    const Entity boat = s.Boat(Vec3(5, 1, 0));
    CHECK(!s.system.Drive(raft, 1.0f, 0.0f)); // a floater, but not a boat
    CHECK(!s.system.Drive(Entity{}, 1.0f, 0.0f));
    CHECK(!s.system.Drive(boat, std::numeric_limits<f32>::quiet_NaN(), 0.0f));
    CHECK(!s.system.Drive(boat, 0.0f, std::numeric_limits<f32>::infinity()));
    CHECK(s.system.Drive(boat, 9.0f, -9.0f)); // out of range is held to 1
    s.world.GetComponent<Boat>(boat)->enabled = false;
    CHECK(!s.system.Drive(boat, 1.0f, 0.0f));
    s.Run(2);
    CHECK(s.system.StateOf(Entity{}).submersion == 0.0f);
}

AETHER_TEST(WaterKit_TheClockSetsTheWavesAndARunRepeats) {
    const auto run = [] {
        Sim s;
        s.Sea(0.0f, 0.5f);
        const Entity raft = s.Raft(Vec3(0, 1.0f, 0));
        s.Run(60 * 5);
        return std::make_pair(s.Position(raft), s.system.Time());
    };
    const auto a = run(), b = run();
    CHECK(Near(a.second, 5.0f, 0.02f));
    CHECK(a.first.x == b.first.x && a.first.y == b.first.y && a.first.z == b.first.z);
    Sim s;
    s.system.SetTime(12.0f);
    CHECK(s.system.Time() == 12.0f);
    s.system.SetTime(std::numeric_limits<f32>::quiet_NaN());
    CHECK(s.system.Time() == 12.0f);
}

AETHER_TEST(WaterKit_TheKitRegistersTheComponentsAndInstallsTheScriptTable) {
    const std::unique_ptr<kit::IKit> kit = MakeWaterKit();
    CHECK(std::string(kit->Name()) == "Water" && kit->Deps().empty() && kit->Stages().empty());
    Sim s;
    kit::KitRuntimeContext context;
    context.world = &s.world;
    CHECK(kit->CreateRuntime(context) == nullptr);
    kit->RegisterComponents();
    CHECK(FindComponentIdByName("Floater") != kInvalidComponentId);
    std::string installed;
    kit::KitScriptApiContext api;
    api.install = [&](const std::string& name) { installed = name; };
    kit->InstallScriptApi(&api);
    CHECK(installed == "Water");
    CHECK(WaterSystem::Active() == &s.system);
}

namespace {

nlohmann::json AssetEntry(const char* path, const char* importer) {
    return {{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", path}, {"importer", importer}};
}

} // namespace

#if AETHER_TEST_HAS_SCRIPTING
// The real player: a sea and a boat in the scene; the physics step floats the boat, and a script asks the water (HeightAt, IsWater, Submersion) and drives (Drive).
AETHER_TEST(WaterKit_PlayerFloatsABoatAndAScriptDrivesIt) {
    if (!player::Game::HasPhysics() || !player::Game::HasScripting()) return;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_water_player_tests";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string script_guid = assets::ToString(assets::NewAssetGuid());
    ScriptComponent component;
    assets::AssetGuid parsed;
    CHECK(assets::ParseAssetGuid(script_guid, parsed));
    component.script.guid = parsed;
    WaterSurface sea;
    sea.wave_height = 0.0f;
    RigidBody rb;
    rb.mass = 400.0f;
    BoxCollider box;
    box.half_extents = Vec3(0.6f, 0.25f, 1.5f);
    Floater floater;
    floater.half_extents = Vec3(0.6f, 0.25f, 1.5f);
    water::Boat boat;
    boat.thrust = 6000.0f;
    boat.propeller_offset = Vec3(0.0f, -0.25f, -1.3f);
    const auto at = [](f32 x, f32 y, f32 z) { return reflect::ToJson(Transform{Vec3(x, y, z), Quaternion::Identity()}); };
    const nlohmann::json scene = {{"$type", "Scene"}, {"$version", 1}, {"entities", nlohmann::json::array({
        {{"components", {{"Transform", at(0, 0, 0)}, {"WaterSurface", reflect::ToJson(sea)}}}},
        {{"components", {{"Transform", at(0, 0.5f, 0)}, {"RigidBody", reflect::ToJson(rb)}, {"BoxCollider", reflect::ToJson(box)}, {"Floater", reflect::ToJson(floater)},
                         {"Boat", reflect::ToJson(boat)}, {"ScriptComponent", reflect::ToJson(component)}}}}})}};
    const std::string script =
        "local S = {}\nfunction S:OnCreate()\n  self.t = 0\nend\n"
        "function S:OnUpdate(dt)\n  self.t = self.t + dt\n"
        "  SaveGames.SetNumber('h', Water.HeightAt(0, 0))\n  SaveGames.SetNumber('is', Water.IsWater(3, 4) and 1 or 0)\n"
        "  SaveGames.SetNumber('sub', Water.Submersion(self.entity))\n"
        "  SaveGames.SetNumber('drove', Water.Drive(self.entity, 1, 0) and 1 or 0)\n"
        "  SaveGames.SetNumber('z', self.entity:Get('Transform').position.z)\nend\nreturn S\n";
    const nlohmann::json assets_json = nlohmann::json::array({AssetEntry("Scenes/start.ascene", "Scene"), {{"guid", script_guid}, {"path", "Scripts/Boat.luau"}, {"importer", "Script"}}});
    const nlohmann::json manifest = {{"$type", "CookManifest"}, {"$version", 1}, {"project", "Demo"}, {"configuration", "Development"},
                                     {"startup_scene", "Scenes/start.ascene"}, {"fixed_timestep_hz", 60.0}, {"gravity", {0.0, -9.81, 0.0}},
                                     {"layers", {"Default"}}, {"collision_matrix", nlohmann::json::array()}, {"assets", assets_json}, {"files", nlohmann::json::array()}};
    pak::PakWriter writer;
    CHECK(writer.Add("Manifest.json", manifest.dump(2)));
    CHECK(writer.Add("Content/Scenes/start.ascene", scene.dump()));
    CHECK(writer.Add("Content/Scripts/Boat.luau", script));
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
    for (int i = 0; i < 60 * 8; ++i) game.Tick(kDt);
    CHECK(game.ScriptErrors().empty());
    const save::SaveBag& bag = game.Saves()->Bag();
    CHECK(Near(bag.GetFloat("h"), 0.0f, 1e-3f) && bag.GetInt("is") == 1);
    CHECK(bag.GetFloat("sub") > 0.05f && bag.GetFloat("sub") < 0.6f); // it floats, part under
    CHECK(bag.GetInt("drove") == 1 && bag.GetFloat("z") > 3.0f);       // and the script drove it forward
}
#endif

#endif
