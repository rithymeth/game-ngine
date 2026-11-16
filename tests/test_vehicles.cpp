#include "test_framework.h"

#ifdef AETHER_TEST_HAS_VEHICLES

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
#include "aether/vehicles/vehicle_system.h"
#include "aether/vehicles/vehicles_kit.h"
#if AETHER_TEST_HAS_SCRIPTING
#include "aether/script/script_system.h"
#endif

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <limits>

// The vehicles kit: a Vehicle on a dynamic box gets four wheels on springs, an engine, a gearbox and brakes (Jolt's vehicle constraint), driven by a
// VehicleInput. Everything is checked on real Jolt bodies on flat ground, then in the player with a script at the wheel.

using namespace aether;
using namespace aether::vehicles;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

bool Near(f32 a, f32 b, f32 eps) { return std::fabs(a - b) <= eps; }

struct Sim {
    JobSystem jobs{2};
    World world;
    PhysicsWorld physics{jobs};
    PhysicsScene scene{world, physics};
    VehicleSystem system{world, scene};

    Sim() {
        RegisterPhysicsComponentSerializers();
        RegisterVehicleComponents();
        physics.SetGravity(Vec3(0, -9.81f, 0));
        BoxCollider ground;
        ground.half_extents = Vec3(300, 0.5f, 300);
        ground.friction = 0.9f;
        world.CreateEntity(Transform{Vec3(0, -0.5f, 0), Quaternion::Identity()}, ground);
    }
    // A 1.8 x 0.8 x 4 m chassis of 1200 kg, dropped from just above the ground.
    Entity Car(const Vehicle& v = {}, const Vec3& at = Vec3(0, 0.9f, 0)) {
        RigidBody rb;
        rb.mass = 1200.0f;
        rb.linear_damping = 0.0f;
        rb.angular_damping = 0.05f;
        BoxCollider box;
        box.half_extents = Vec3(0.9f, 0.4f, 2.0f);
        return world.CreateEntity(Transform{at, Quaternion::Identity()}, rb, box, v);
    }
    void Run(int steps) {
        for (int i = 0; i < steps; ++i) {
            system.Apply();
            scene.Step(kDt);
        }
    }
    Vec3 Position(Entity e) { return world.GetComponent<Transform>(e)->position; }
    const VehicleState& State(Entity e) { return *world.GetComponent<VehicleState>(e); }
    void Throttle(Entity e, f32 throttle, f32 steer = 0.0f, f32 brake = 0.0f) { CHECK(system.Drive(e, throttle, steer, brake)); }
};

} // namespace

AETHER_TEST(Vehicles_ComponentsRegisterAndRoundTrip) {
    RegisterVehicleComponents();
    CHECK(FindComponentIdByName("Vehicle") != kInvalidComponentId && FindComponentIdByName("VehicleInput") != kInvalidComponentId &&
          FindComponentIdByName("VehicleState") != kInvalidComponentId);
    Vehicle v;
    v.enabled = false;
    v.drive = DriveType::AllWheel;
    v.wheel_radius = 0.5f;
    v.suspension_frequency = 2.5f;
    v.engine_torque = 900.0f;
    v.max_steer_degrees = 40.0f;
    v.grip = 1.5f;
    const reflect::Json saved = reflect::ToJson(v);
    Vehicle back;
    CHECK(reflect::FromJson(back, saved) && !back.enabled && back.drive == DriveType::AllWheel && Near(back.wheel_radius, 0.5f, 1e-5f));
    CHECK(Near(back.suspension_frequency, 2.5f, 1e-5f) && Near(back.engine_torque, 900.0f, 1e-3f) && Near(back.max_steer_degrees, 40.0f, 1e-4f) && Near(back.grip, 1.5f, 1e-5f));
    Vehicle loaded; // saved with nothing: a rear-wheel-drive car with the defaults
    CHECK(reflect::FromJson(loaded, reflect::Json::object()) && loaded.enabled && loaded.drive == DriveType::RearWheel && Near(loaded.wheel_radius, 0.35f, 1e-5f));
    VehicleInput in;
    in.throttle = 0.5f;
    in.steer = -1.0f;
    VehicleInput in_back;
    CHECK(reflect::FromJson(in_back, reflect::ToJson(in)) && Near(in_back.throttle, 0.5f, 1e-5f) && Near(in_back.steer, -1.0f, 1e-5f));
}

AETHER_TEST(Vehicles_ABuiltVehicleSettlesOnFourWheels) {
    Sim s;
    const Entity car = s.Car();
    s.Run(1);
    CHECK(s.system.VehicleCount() == 1);
    CHECK(s.world.HasComponent<VehicleInput>(car) && s.world.HasComponent<VehicleState>(car)); // added beside the Vehicle
    s.Run(180);
    CHECK(s.State(car).wheels_on_ground == 4 && !s.State(car).airborne);
    // The chassis bottom rests ground_clearance (0.3 m) above the ground, so its centre is clearance + half height (0.4) up, give or take the springs.
    const f32 y = s.Position(car).y;
    CHECK(y > 0.55f && y < 0.85f);
    CHECK(std::fabs(s.State(car).speed) < 0.1f);
    // The wheels sit at the corners: 0.9 * 0.9 = 0.81 m out, 0.7 * 2.0 = 1.4 m along, front is +Z.
    WheelInfo fl, fr, rl, rr;
    CHECK(s.system.Wheel(car, 0, fl) && s.system.Wheel(car, 1, fr) && s.system.Wheel(car, 2, rl) && s.system.Wheel(car, 3, rr));
    CHECK(fl.position.z > 1.0f && fr.position.z > 1.0f && rl.position.z < -1.0f && rr.position.z < -1.0f);
    CHECK(Near(std::fabs(fl.position.x), 0.81f, 0.08f) && Near(std::fabs(rr.position.x), 0.81f, 0.08f) && fl.position.x * fr.position.x < 0.0f);
    CHECK(fl.contact && rr.contact && fl.position.y > 0.2f && fl.position.y < 0.5f); // a wheel's centre is a radius (0.35) above the ground
    WheelInfo none;
    CHECK(!s.system.Wheel(car, 4, none) && !s.system.Wheel(kNullEntity, 0, none));
}

AETHER_TEST(Vehicles_ThrottleDrivesItForwardAndReverseBackAndTheBrakesStopIt) {
    Sim s;
    const Entity car = s.Car();
    s.Run(60);
    s.Throttle(car, 1.0f);
    s.Run(300);
    CHECK(s.State(car).speed > 8.0f);
    CHECK(s.Position(car).z > 15.0f && std::fabs(s.Position(car).x) < 1.0f); // along its nose, +Z, in a straight line
    CHECK(s.State(car).gear >= 1 && s.State(car).rpm > 1000.0f);
    CHECK(s.State(car).wheels_on_ground == 4);
    // Let go and brake hard: it stops.
    s.Throttle(car, 0.0f, 0.0f, 1.0f);
    s.Run(240);
    CHECK(std::fabs(s.State(car).speed) < 1.0f);

    // Another car in reverse goes the other way.
    Sim r;
    const Entity back = r.Car();
    r.Run(60);
    r.Throttle(back, -1.0f);
    r.Run(240);
    CHECK(r.State(back).speed < -2.0f && r.Position(back).z < -3.0f && r.State(back).gear < 0);
}

AETHER_TEST(Vehicles_SteeringTurnsItTheWayYouSteer) {
    const auto drive = [](f32 steer, f32* angle) {
        Sim s;
        const Entity car = s.Car();
        s.Run(60);
        s.Throttle(car, 0.6f, steer);
        s.Run(180);
        if (angle) *angle = s.State(car).steer_angle;
        return s.Position(car).x;
    };
    f32 angle_right = 0, angle_left = 0;
    const f32 right = drive(1.0f, &angle_right), left = drive(-1.0f, &angle_left), straight = drive(0.0f, nullptr);
    CHECK(std::fabs(straight) < 0.5f);
    CHECK(std::fabs(right) > 2.0f && std::fabs(left) > 2.0f && right * left < 0.0f); // opposite ways
    CHECK(angle_right * angle_left < 0.0f && std::fabs(angle_right) > 0.3f && std::fabs(angle_right) < 0.6f); // 30 degrees is 0.52 rad at full lock
}

AETHER_TEST(Vehicles_EveryDriveTypeMovesAndGripMatters) {
    for (const DriveType drive : {DriveType::FrontWheel, DriveType::RearWheel, DriveType::AllWheel}) {
        Sim s;
        Vehicle v;
        v.drive = drive;
        const Entity car = s.Car(v);
        s.Run(60);
        s.Throttle(car, 1.0f);
        s.Run(240);
        CHECK(s.State(car).speed > 5.0f);
    }
    // An icy tyre spins instead of pulling: the same car with a tenth of the grip is slower after the same time.
    const auto speed_with = [](f32 grip) {
        Sim s;
        Vehicle v;
        v.grip = grip;
        const Entity car = s.Car(v);
        s.Run(60);
        s.Throttle(car, 1.0f);
        s.Run(180);
        return s.State(car).speed;
    };
    CHECK(speed_with(0.1f) < speed_with(1.0f) * 0.7f);
}

AETHER_TEST(Vehicles_EditingOrDisablingOrDestroyingTearsTheVehicleDownFirst) {
    // A weak engine, edited into a strong one while playing: the vehicle is built again and pulls harder.
    Sim s;
    Vehicle weak;
    weak.engine_torque = 60.0f;
    const Entity car = s.Car(weak);
    s.Run(60);
    s.Throttle(car, 1.0f);
    s.Run(120);
    const f32 slow = s.State(car).speed;
    s.world.GetComponent<Vehicle>(car)->engine_torque = 900.0f;
    s.Run(120);
    CHECK(s.system.VehicleCount() == 1 && s.State(car).speed > slow + 3.0f);

    // Disabled: no vehicle any more (nothing drives it, it rolls to a stop on its own).
    s.world.GetComponent<Vehicle>(car)->enabled = false;
    s.Run(1);
    CHECK(s.system.VehicleCount() == 0);
    s.Run(60); // the chassis is a plain body now; stepping is safe

    // Destroyed: the constraint goes before the body does.
    Sim d;
    const Entity doomed = d.Car();
    const Entity other = d.Car(Vehicle{}, Vec3(10, 0.9f, 0));
    d.Run(30);
    CHECK(d.system.VehicleCount() == 2);
    d.world.DestroyEntity(doomed);
    d.Run(1);
    CHECK(d.system.VehicleCount() == 1);
    d.Throttle(other, 1.0f);
    d.Run(120);
    CHECK(d.State(other).speed > 3.0f);

    // A vehicle with no collider or a static body is left alone (nothing to build on).
    Sim n;
    n.world.CreateEntity(Transform{Vec3(0, 3, 0), Quaternion::Identity()}, Vehicle{});
    BoxCollider box;
    n.world.CreateEntity(Transform{Vec3(5, 3, 0), Quaternion::Identity()}, box, Vehicle{}); // no RigidBody: static
    n.Run(10);
    CHECK(n.system.VehicleCount() == 0);
}

AETHER_TEST(Vehicles_DriveClampsAndRefusesWhatItCannotUse) {
    Sim s;
    const Entity car = s.Car();
    CHECK(s.system.Drive(car, 5.0f, -9.0f, 3.0f, -2.0f));
    const VehicleInput* in = s.world.GetComponent<VehicleInput>(car);
    CHECK(in != nullptr && Near(in->throttle, 1.0f, 1e-6f) && Near(in->steer, -1.0f, 1e-6f) && Near(in->brake, 1.0f, 1e-6f) && Near(in->handbrake, 0.0f, 1e-6f));
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    CHECK(!s.system.Drive(car, nan, 0.0f) && !s.system.Drive(car, 0.0f, std::numeric_limits<f32>::infinity()));
    CHECK(Near(s.world.GetComponent<VehicleInput>(car)->throttle, 1.0f, 1e-6f)); // a refused call changes nothing
    const Entity plain = s.world.CreateEntity(Transform{Vec3(0, 0, 0), Quaternion::Identity()});
    CHECK(!s.system.Drive(plain, 1.0f, 0.0f) && !s.system.Drive(kNullEntity, 1.0f, 0.0f));
    // A NaN written straight into the component is ignored too: the car sits still instead of the physics going to pieces.
    s.world.GetComponent<VehicleInput>(car)->throttle = nan;
    s.Run(120);
    CHECK(std::isfinite(s.Position(car).y) && std::fabs(s.State(car).speed) < 0.5f);
}

AETHER_TEST(Vehicles_WheelsTurnAndSteerAndFlipPutsItBackOnItsWheels) {
    Sim s;
    const Entity car = s.Car();
    s.Run(60);
    WheelInfo before, after;
    CHECK(s.system.Wheel(car, 2, before));
    s.Throttle(car, 0.7f, 1.0f);
    s.Run(90);
    CHECK(s.system.Wheel(car, 2, after) && std::fabs(after.spin_angle - before.spin_angle) > 3.0f); // the rear wheel has turned
    WheelInfo front;
    CHECK(s.system.Wheel(car, 0, front) && std::fabs(front.steer_angle) > 0.3f && s.system.Wheel(car, 2, after) && Near(after.steer_angle, 0.0f, 1e-4f)); // only the front steers

    // Roll it onto its roof: Flip stands it up where it is, keeping its heading.
    Sim f;
    const Entity roll = f.Car(Vehicle{}, Vec3(0, 2.0f, 0));
    f.Run(1);
    JPH::BodyInterface& bodies = f.physics.BodyInterface();
    const JPH::Quat upside = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), 1.0f) * JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), 3.14159265f);
    bodies.SetRotation(f.scene.BodyOf(roll), upside, JPH::EActivation::Activate);
    f.world.GetComponent<Transform>(roll)->rotation = Quaternion(upside.GetX(), upside.GetY(), upside.GetZ(), upside.GetW());
    CHECK(f.system.Flip(roll));
    const Quaternion q = f.world.GetComponent<Transform>(roll)->rotation;
    const Vec3 up(2 * (q.x * q.y - q.w * q.z), 1 - 2 * (q.x * q.x + q.z * q.z), 2 * (q.y * q.z + q.w * q.x));
    const Vec3 nose(2 * (q.x * q.z + q.w * q.y), 2 * (q.y * q.z - q.w * q.x), 1 - 2 * (q.x * q.x + q.y * q.y));
    CHECK(up.y > 0.999f && Near(std::atan2(nose.x, nose.z), 1.0f, 0.01f)); // upright, still pointing the way it was (yaw 1 rad)
    f.Run(120);
    CHECK(f.State(roll).wheels_on_ground == 4);
    CHECK(!f.system.Flip(f.world.CreateEntity(Transform{Vec3(0, 0, 0), Quaternion::Identity()}))); // not a vehicle
}

AETHER_TEST(Vehicles_TheKitRegistersTheComponentsAndInstallsTheScriptTable) {
    const std::unique_ptr<kit::IKit> kit = MakeVehiclesKit();
    CHECK(std::string(kit->Name()) == "Vehicles" && kit->Deps().empty() && kit->Stages().empty());
    Sim s;
    kit::KitRuntimeContext context;
    context.world = &s.world;
    CHECK(kit->CreateRuntime(context) == nullptr);
    kit->RegisterComponents();
    CHECK(FindComponentIdByName("Vehicle") != kInvalidComponentId);
    std::string installed;
    kit::KitScriptApiContext api;
    api.install = [&](const std::string& name) { installed = name; };
    kit->InstallScriptApi(&api);
    CHECK(installed == "Vehicles");
    CHECK(VehicleSystem::Active() == &s.system);
}

namespace {

nlohmann::json AssetEntry(const char* path, const char* importer) {
    return {{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", path}, {"importer", importer}};
}

} // namespace

#if AETHER_TEST_HAS_SCRIPTING
// The real player: a script at the wheel drives a car down a road with Vehicles.Drive and reads Vehicles.Speed back.
AETHER_TEST(Vehicles_PlayerDrivesACarFromAScript) {
    if (!player::Game::HasPhysics() || !player::Game::HasScripting()) return;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_vehicles_player_tests";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string script_guid = assets::ToString(assets::NewAssetGuid());
    ScriptComponent component;
    assets::AssetGuid parsed;
    CHECK(assets::ParseAssetGuid(script_guid, parsed));
    component.script.guid = parsed;
    BoxCollider ground;
    ground.half_extents = Vec3(300, 0.5f, 300);
    BoxCollider chassis;
    chassis.half_extents = Vec3(0.9f, 0.4f, 2.0f);
    RigidBody rb;
    rb.mass = 1200.0f;
    rb.linear_damping = 0.0f;
    const auto at = [](f32 x, f32 y, f32 z) { return reflect::ToJson(Transform{Vec3(x, y, z), Quaternion::Identity()}); };
    const nlohmann::json scene = {{"$type", "Scene"}, {"$version", 1}, {"entities", nlohmann::json::array({
        {{"components", {{"Transform", at(0, -0.5f, 0)}, {"BoxCollider", reflect::ToJson(ground)}}}},
        {{"components", {{"Transform", at(0, 0.9f, 0)}, {"RigidBody", reflect::ToJson(rb)}, {"BoxCollider", reflect::ToJson(chassis)}, {"Vehicle", reflect::ToJson(Vehicle{})},
                         {"ScriptComponent", reflect::ToJson(component)}, {"Tags", reflect::Json{{"names", {"Car"}}}}}}}})}};
    const std::string script = "local S = {}\nfunction S:OnCreate()\n  self.t = 0\nend\n"
                               "function S:OnUpdate(dt)\n  self.t = self.t + dt\n"
                               "  if self.t > 0.5 then SaveGames.SetNumber('drive', Vehicles.Drive(self.entity, 1, 0) and 1 or 0) end\n"
                               "  SaveGames.SetNumber('speed', Vehicles.Speed(self.entity))\n"
                               "  SaveGames.SetNumber('wheels', Vehicles.WheelsOnGround(self.entity))\n"
                               "  local w = Vehicles.WheelPosition(self.entity, 0)\n  SaveGames.SetNumber('wheel_z', w.z)\n"
                               "  SaveGames.SetNumber('spin', Vehicles.WheelSpin(self.entity, 2))\nend\nreturn S\n";
    const nlohmann::json assets_json = nlohmann::json::array({AssetEntry("Scenes/start.ascene", "Scene"), {{"guid", script_guid}, {"path", "Scripts/Drive.luau"}, {"importer", "Script"}}});
    const nlohmann::json manifest = {{"$type", "CookManifest"}, {"$version", 1}, {"project", "Demo"}, {"configuration", "Development"},
                                     {"startup_scene", "Scenes/start.ascene"}, {"fixed_timestep_hz", 60.0}, {"gravity", {0.0, -9.81, 0.0}},
                                     {"layers", {"Default"}}, {"collision_matrix", nlohmann::json::array()}, {"assets", assets_json}, {"files", nlohmann::json::array()}};
    pak::PakWriter writer;
    CHECK(writer.Add("Manifest.json", manifest.dump(2)));
    CHECK(writer.Add("Content/Scenes/start.ascene", scene.dump()));
    CHECK(writer.Add("Content/Scripts/Drive.luau", script));
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
    for (int i = 0; i < 360; ++i) game.Tick(kDt);
    CHECK(game.ScriptErrors().empty());
    const std::vector<Entity> cars = FindEntitiesWithTag(game.GetWorld(), "Car");
    CHECK(cars.size() == 1);
    if (cars.size() != 1) return;
    const save::SaveBag& bag = game.Saves()->Bag();
    CHECK(bag.GetInt("drive") == 1 && bag.GetInt("wheels") == 4);
    CHECK(bag.GetFloat("speed") > 5.0f && game.GetWorld().GetComponent<Transform>(cars[0])->position.z > 8.0f);
    CHECK(bag.GetFloat("wheel_z") > 5.0f && std::fabs(bag.GetFloat("spin")) > 5.0f); // the front-left wheel is out in front, the rear one has been turning
}
#endif

#endif
