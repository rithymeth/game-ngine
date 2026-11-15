#include "test_framework.h"

#ifdef AETHER_TEST_HAS_FORCEFIELDS

#include "aether/assets/asset_guid.h"
#include "aether/ecs/world.h"
#include "aether/forcefields/force_field_system.h"
#include "aether/forcefields/forcefields_kit.h"
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

#include <cmath>
#include <filesystem>
#include <limits>

// The force-fields kit: ForceField components push the dynamic bodies inside their volume (wind, gravity wells, whirlpools, water, drag), and Explode pushes
// everything near a point. The maths is checked on its own and then on real Jolt bodies.

using namespace aether;
using namespace aether::ff;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

bool Near(f32 a, f32 b, f32 eps) { return std::fabs(a - b) <= eps; }
bool NearRel(f32 a, f32 expected, f32 fraction) { return std::fabs(a - expected) <= std::fabs(expected) * fraction + 1e-3f; }

struct Sim {
    JobSystem jobs{2};
    World world;
    PhysicsWorld physics{jobs};
    PhysicsScene scene{world, physics};
    ForceFieldSystem system{world, scene};

    explicit Sim(Vec3 gravity = Vec3(0, 0, 0)) {
        RegisterPhysicsComponentSerializers();
        physics.SetGravity(gravity);
    }
    Entity Ball(const Vec3& at, f32 mass = 1.0f, BodyMotion motion = BodyMotion::Dynamic) {
        RigidBody rb;
        rb.motion = motion;
        rb.mass = mass;
        rb.linear_damping = 0.0f; // nothing but the fields acts on it
        SphereCollider sphere;
        sphere.radius = 0.5f;
        return world.CreateEntity(Transform{at, Quaternion::Identity()}, rb, sphere);
    }
    Entity Field(const ForceField& f, const Vec3& at = Vec3(0, 0, 0), const Quaternion& q = Quaternion::Identity()) { return world.CreateEntity(Transform{at, q}, f); }
    Vec3 Velocity(Entity e) {
        const JPH::Vec3 v = physics.BodyInterface().GetLinearVelocity(scene.BodyOf(e));
        return Vec3(v.GetX(), v.GetY(), v.GetZ());
    }
    void SetVelocity(Entity e, const Vec3& v) { physics.BodyInterface().SetLinearVelocity(scene.BodyOf(e), JPH::Vec3(v.x, v.y, v.z)); }
    // The bodies are made on a Sync; then each step applies the fields and moves the bodies.
    void Run(int steps) {
        scene.Sync();
        for (int i = 0; i < steps; ++i) {
            system.Apply();
            scene.Step(kDt);
        }
    }
};

ForceField Wind(f32 strength, Vec3 direction = Vec3(1, 0, 0), f32 radius = 5.0f) {
    ForceField f;
    f.mode = FieldMode::Directional;
    f.strength = strength;
    f.direction = direction;
    f.radius = radius;
    return f;
}

} // namespace

AETHER_TEST(ForceFields_ComponentRegistersAndRoundTrips) {
    RegisterForceFieldComponents();
    CHECK(FindComponentIdByName("ForceField") != kInvalidComponentId);
    ForceField f = Wind(7.5f, Vec3(0, 0, -1), 3.0f);
    f.shape = FieldShape::Box;
    f.half_extents = Vec3(1, 2, 3);
    f.mode = FieldMode::Vortex;
    f.falloff = 2.0f;
    f.mass_independent = false;
    f.fluid_drag = 0.5f;
    f.enabled = false;
    const reflect::Json saved = reflect::ToJson(f);
    ForceField back;
    CHECK(reflect::FromJson(back, saved) && back.shape == FieldShape::Box && back.mode == FieldMode::Vortex && Near(back.strength, 7.5f, 1e-5f) && !back.enabled && !back.mass_independent);
    CHECK(Near(back.half_extents.z, 3.0f, 1e-5f) && Near(back.direction.z, -1.0f, 1e-5f) && Near(back.falloff, 2.0f, 1e-5f) && Near(back.fluid_drag, 0.5f, 1e-5f) && Near(back.radius, 3.0f, 1e-5f));
    // Saved with only a mode: a sphere of 5 m, on, with the defaults for the rest.
    ForceField loaded;
    CHECK(reflect::FromJson(loaded, reflect::Json{{"mode", "Drag"}}) && loaded.mode == FieldMode::Drag && loaded.enabled && loaded.shape == FieldShape::Sphere && Near(loaded.radius, 5.0f, 1e-5f) && loaded.mass_independent);
}

AETHER_TEST(ForceFields_TheMathsOfEachMode) {
    const Vec3 gravity(0, -10, 0);
    const Quaternion none = Quaternion::Identity();
    const Vec3 zero(0, 0, 0);
    const auto push = [&](const ForceField& f, Vec3 at, Vec3 velocity = Vec3(0, 0, 0), f32 submersion = 0.0f, Quaternion q = Quaternion::Identity()) {
        return FieldPush(f, zero, q, at, velocity, gravity, submersion);
    };

    // Directional: along the direction whatever its length, by the strength; nothing for no direction or a strength that is not a number.
    const Vec3 wind = push(Wind(6.0f, Vec3(0, 0, 5)), Vec3(1, 1, 1));
    CHECK(Near(wind.x, 0.0f, 1e-5f) && Near(wind.z, 6.0f, 1e-5f));
    CHECK(push(Wind(6.0f, Vec3(0, 0, 0)), Vec3(1, 0, 0)).LengthSq() == 0.0f && push(Wind(std::numeric_limits<f32>::quiet_NaN()), Vec3(1, 0, 0)).LengthSq() == 0.0f);
    CHECK(push(Wind(6.0f, Vec3(std::numeric_limits<f32>::infinity(), 0, 0)), Vec3(1, 0, 0)).LengthSq() == 0.0f);

    // Radial: toward the centre for a positive strength, away for a negative one; the fade is 1 at the middle of the radius' scale, 0 at the edge.
    ForceField well;
    well.mode = FieldMode::Radial;
    well.radius = 10.0f;
    well.strength = 8.0f;
    well.falloff = 0.0f;
    CHECK(Near(push(well, Vec3(5, 0, 0)).x, -8.0f, 1e-4f)); // constant everywhere, pulling toward the centre
    well.falloff = 1.0f;
    CHECK(Near(push(well, Vec3(5, 0, 0)).x, -4.0f, 1e-4f) && Near(push(well, Vec3(10, 0, 0)).x, 0.0f, 1e-4f) && Near(push(well, Vec3(2, 0, 0)).x, -6.4f, 1e-4f));
    well.falloff = 2.0f;
    CHECK(Near(push(well, Vec3(5, 0, 0)).x, -2.0f, 1e-4f)); // 0.5^2 of 8
    well.strength = -8.0f;
    CHECK(push(well, Vec3(5, 0, 0)).x > 0.0f); // repels
    CHECK(push(well, zero).LengthSq() == 0.0f); // on the centre there is no direction to push

    // Vortex: around the up axis, which turns with the field.
    ForceField swirl;
    swirl.mode = FieldMode::Vortex;
    swirl.radius = 10.0f;
    swirl.strength = 5.0f;
    swirl.falloff = 0.0f;
    const Vec3 around = push(swirl, Vec3(2, 3, 0)); // height along the axis does not matter
    CHECK(Near(around.x, 0.0f, 1e-4f) && Near(around.y, 0.0f, 1e-4f) && Near(around.z, -5.0f, 1e-4f));
    const Vec3 turned = push(swirl, Vec3(2, 3, 0), Vec3(0, 0, 0), 0.0f, Quaternion::FromAxisAngle(Vec3(1, 0, 0), 3.14159265f)); // upside down: the other way round
    CHECK(Near(turned.z, 5.0f, 1e-3f));
    CHECK(push(swirl, Vec3(0, 4, 0)).LengthSq() == 0.0f); // on the axis

    // Buoyancy: against gravity by the strength times how submerged it is, and against the motion by fluid_drag times the same.
    ForceField water;
    water.mode = FieldMode::Buoyancy;
    water.shape = FieldShape::Box;
    water.strength = 1.0f;
    CHECK(Near(push(water, Vec3(0, -1, 0), Vec3(0, 0, 0), 1.0f).y, 10.0f, 1e-4f) && Near(push(water, Vec3(0, -1, 0), Vec3(0, 0, 0), 0.5f).y, 5.0f, 1e-4f));
    CHECK(push(water, Vec3(0, 9, 0), Vec3(0, 0, 0), 0.0f).LengthSq() == 0.0f);
    CHECK(Near(push(water, Vec3(0, -1, 0), Vec3(0, 0, 0), 4.0f).y, 10.0f, 1e-4f)); // submersion is held to 0..1
    water.fluid_drag = 2.0f;
    const Vec3 slowed = push(water, Vec3(0, -1, 0), Vec3(3, 0, 0), 1.0f);
    CHECK(Near(slowed.x, -6.0f, 1e-4f) && Near(slowed.y, 10.0f, 1e-4f));
    water.half_extents = Vec3(4, 2.5f, 4);
    CHECK(Near(FieldSurfaceHeight(water, Vec3(0, 10, 0)), 12.5f, 1e-4f)); // the top of the box
    ForceField ball;
    ball.radius = 3.0f;
    CHECK(Near(FieldSurfaceHeight(ball, Vec3(0, 1, 0)), 4.0f, 1e-4f));

    // Drag: against the velocity by the coefficient; a negative coefficient does nothing (it would speed things up).
    ForceField mud;
    mud.mode = FieldMode::Drag;
    mud.strength = 2.0f;
    CHECK(Near(push(mud, zero, Vec3(3, 0, 0)).x, -6.0f, 1e-4f));
    mud.strength = -2.0f;
    CHECK(push(mud, zero, Vec3(3, 0, 0)).LengthSq() == 0.0f);
    // Rotated: a quaternion turns a vector as expected.
    const Vec3 r = Rotated(Quaternion::FromAxisAngle(Vec3(0, 1, 0), 1.57079633f), Vec3(1, 0, 0));
    CHECK(Near(r.x, 0.0f, 1e-4f) && Near(r.z, -1.0f, 1e-4f));
    (void)none;
}

AETHER_TEST(ForceFields_WindPushesDynamicBodiesInsideTheField) {
    Sim s;
    CHECK(ForceFieldSystem::Active() == &s.system);
    s.Field(Wind(10.0f, Vec3(1, 0, 0), 50.0f)); // bigger than anything travels in the second the test runs
    const Entity light = s.Ball(Vec3(0, 0, 0), 1.0f), heavy = s.Ball(Vec3(0, 0, 3), 4.0f);
    // The static and kinematic bodies are inside the field but off to the side, out of the way of the balls it carries.
    const Entity outside = s.Ball(Vec3(200, 0, 0)), fixed = s.Ball(Vec3(2, 6, 0), 1.0f, BodyMotion::Static), kinematic = s.Ball(Vec3(-2, 6, 0), 1.0f, BodyMotion::Kinematic);
    s.Run(60);
    // An acceleration: the heavy body gains speed like the light one (10 m/s after a second).
    CHECK(NearRel(s.Velocity(light).x, 10.0f, 0.05f) && NearRel(s.Velocity(heavy).x, 10.0f, 0.05f));
    CHECK(Near(s.Velocity(outside).x, 0.0f, 1e-4f)); // outside the volume
    CHECK(Near(s.world.GetComponent<Transform>(fixed)->position.x, 2.0f, 1e-4f) && Near(s.world.GetComponent<Transform>(kinematic)->position.x, -2.0f, 1e-4f)); // not pushed
    CHECK(Near(s.Velocity(fixed).x, 0.0f, 1e-6f) && Near(s.Velocity(kinematic).x, 0.0f, 1e-6f));
}

AETHER_TEST(ForceFields_AForceFieldDependsOnMassAndTheSwitches) {
    Sim s;
    ForceField f = Wind(10.0f, Vec3(1, 0, 0), 50.0f);
    f.mass_independent = false; // newtons: a body of mass 4 gains a quarter of the speed
    const Entity field = s.Field(f);
    const Entity light = s.Ball(Vec3(0, 0, 0), 1.0f), heavy = s.Ball(Vec3(0, 0, 3), 4.0f);
    s.Run(60);
    CHECK(NearRel(s.Velocity(light).x, 10.0f, 0.05f) && NearRel(s.Velocity(heavy).x, 2.5f, 0.05f));
    // Disabled: nothing more; switched on again: it pushes again.
    s.world.GetComponent<ForceField>(field)->enabled = false;
    const f32 before = s.Velocity(light).x;
    s.Run(30);
    CHECK(NearRel(s.Velocity(light).x, before, 0.01f));
    s.world.GetComponent<ForceField>(field)->enabled = true;
    s.Run(30);
    CHECK(s.Velocity(light).x > before + 4.0f);
}

AETHER_TEST(ForceFields_BoxesTurnWithTheirEntityAndBadSizesAreHeld) {
    Sim s;
    ForceField box = Wind(10.0f);
    box.shape = FieldShape::Box;
    box.half_extents = Vec3(5.0f, 1.0f, 1.0f); // long in x ...
    s.Field(box, Vec3(0, 0, 0), Quaternion::FromAxisAngle(Vec3(0, 1, 0), 1.57079633f)); // ... turned to be long in z
    const Entity along = s.Ball(Vec3(0, 0, 4)), across = s.Ball(Vec3(4, 0, 0));
    s.Run(30);
    CHECK(s.Velocity(along).x > 3.0f && Near(s.Velocity(across).x, 0.0f, 1e-4f));
    // A radius that is not a number or not positive is held to the minimum size: nothing near is inside it.
    Sim t;
    ForceField bad = Wind(10.0f);
    bad.radius = std::numeric_limits<f32>::quiet_NaN();
    t.Field(bad, Vec3(0, 0, 0));
    ForceField none = Wind(10.0f);
    none.radius = -4.0f;
    t.Field(none, Vec3(0, 0, 0));
    const Entity near_ball = t.Ball(Vec3(3, 0, 0));
    t.Run(30);
    CHECK(Near(t.Velocity(near_ball).x, 0.0f, 1e-4f));
}

AETHER_TEST(ForceFields_RadialAndVortexFieldsBendTheMotion) {
    Sim s;
    ForceField well;
    well.mode = FieldMode::Radial;
    well.radius = 10.0f;
    well.strength = 8.0f;
    well.falloff = 0.0f;
    const Entity pulled = s.Ball(Vec3(3, 0, 0));
    s.Field(well);
    s.Run(30); // half a second at 8 m/s^2: 4 m/s toward the centre
    CHECK(NearRel(s.Velocity(pulled).x, -4.0f, 0.06f) && Near(s.Velocity(pulled).y, 0.0f, 1e-3f));

    Sim t;
    ForceField repel = well;
    repel.strength = -8.0f;
    const Entity pushed = t.Ball(Vec3(3, 0, 0));
    t.Field(repel);
    t.Run(30);
    CHECK(NearRel(t.Velocity(pushed).x, 4.0f, 0.06f));

    Sim u;
    ForceField swirl = well;
    swirl.mode = FieldMode::Vortex;
    const Entity circling = u.Ball(Vec3(3, 0, 0));
    u.Field(swirl);
    u.Run(30);
    CHECK(NearRel(u.Velocity(circling).z, -4.0f, 0.06f) && Near(u.Velocity(circling).y, 0.0f, 1e-3f)); // around Y: along -z at +x
}

AETHER_TEST(ForceFields_WaterHoldsBodiesUpAndSlowsThem) {
    // Strength 1 cancels gravity for a body fully under the surface (top of the box at y = 2): it stays where it is.
    Sim s(Vec3(0, -10, 0));
    ForceField water;
    water.mode = FieldMode::Buoyancy;
    water.shape = FieldShape::Box;
    water.half_extents = Vec3(10, 2, 10);
    water.strength = 1.0f;
    s.Field(water);
    const Entity held = s.Ball(Vec3(0, 0, 0));
    s.Run(60);
    CHECK(Near(s.Velocity(held).y, 0.0f, 0.1f) && Near(s.world.GetComponent<Transform>(held)->position.y, 0.0f, 0.1f));

    // Strength 2 floats it up (a net +10 m/s^2: 5 m/s after half a second); a body with no water around it falls as usual.
    Sim t(Vec3(0, -10, 0));
    water.strength = 2.0f;
    t.Field(water);
    const Entity rising = t.Ball(Vec3(0, 0, 0)), dry = t.Ball(Vec3(0, 30, 0));
    t.Run(30);
    CHECK(NearRel(t.Velocity(rising).y, 5.0f, 0.15f) && NearRel(t.Velocity(dry).y, -5.0f, 0.1f));

    // Only partly under the surface it is held up only partly: the body's bounds run from y = 1.9 to 2.9 here, a tenth below the surface, so a tenth of gravity
    // is cancelled (-9 m/s^2: about -0.9 m/s after a tenth of a second, measured before it has sunk far enough to be held up more).
    Sim u(Vec3(0, -10, 0));
    water.strength = 1.0f;
    u.Field(water);
    const Entity surface = u.Ball(Vec3(0, 2.4f, 0));
    u.Run(6);
    CHECK(NearRel(u.Velocity(surface).y, -0.9f, 0.25f));
    Sim w(Vec3(0, -10, 0)); // and not held up at all by the same water when it only reaches the surface from above: free fall
    w.Field(water);
    const Entity floating = w.Ball(Vec3(0, 2.95f, 0));
    w.Run(6);
    CHECK(NearRel(w.Velocity(floating).y, -0.95f, 0.25f) && w.Velocity(floating).y < u.Velocity(surface).y); // falls faster than the half-held one

    // Fluid drag slows what moves through it: 4 per second takes 5 m/s to about 0.7 m/s in half a second (and gravity is still held off).
    Sim v(Vec3(0, -10, 0));
    water.fluid_drag = 4.0f;
    v.Field(water);
    const Entity swimmer = v.Ball(Vec3(0, 0, 0));
    v.scene.Sync();
    v.SetVelocity(swimmer, Vec3(5, 0, 0));
    v.Run(30);
    CHECK(NearRel(v.Velocity(swimmer).x, 5.0f * std::exp(-2.0f), 0.2f) && Near(v.Velocity(swimmer).y, 0.0f, 0.15f));
}

AETHER_TEST(ForceFields_DragSlowsWhatMovesThrough) {
    Sim s;
    ForceField mud;
    mud.mode = FieldMode::Drag;
    mud.strength = 2.0f;
    mud.radius = 10.0f;
    s.Field(mud);
    const Entity moving = s.Ball(Vec3(0, 0, 0)), far = s.Ball(Vec3(40, 0, 0));
    s.scene.Sync();
    s.SetVelocity(moving, Vec3(10, 0, 0));
    s.SetVelocity(far, Vec3(0, 0, 10));
    s.Run(60); // 10 e^-2 is 1.35 after a second
    CHECK(NearRel(s.Velocity(moving).x, 10.0f * std::exp(-2.0f), 0.1f));
    CHECK(Near(s.Velocity(far).z, 10.0f, 1e-3f)); // outside the field: no drag at all
}

AETHER_TEST(ForceFields_ExplosionsPushEverythingAwayAndFade) {
    Sim s;
    const Entity east = s.Ball(Vec3(2, 0, 0), 1.0f), west = s.Ball(Vec3(-2, 0, 0), 5.0f), centre = s.Ball(Vec3(0, 0, 0), 1.0f);
    const Entity edge = s.Ball(Vec3(10, 0, 0)), outside = s.Ball(Vec3(30, 0, 0));
    const Entity fixed = s.Ball(Vec3(0, 0, 3), 1.0f, BodyMotion::Static), kinematic = s.Ball(Vec3(0, 0, -3), 1.0f, BodyMotion::Kinematic);
    s.scene.Sync();
    // East, west and the one on the centre: the one whose centre is on the edge gets nothing (the blast has faded to 0 there), nor do the static and the kinematic.
    CHECK(s.system.Explode(Vec3(0, 0, 0), 10.0f, 10.0f) == 3);
    CHECK(NearRel(s.Velocity(east).x, 8.0f, 0.02f) && Near(s.Velocity(east).y, 0.0f, 1e-3f)); // 10 m/s faded to 0.8 at 2 m of 10
    CHECK(NearRel(s.Velocity(west).x, -8.0f, 0.02f)); // a body of mass 5 gets the same change of velocity
    CHECK(Near(s.Velocity(centre).x, 0.0f, 1e-3f) && NearRel(s.Velocity(centre).y, 10.0f, 0.02f)); // dead centre: straight up
    CHECK(Near(s.Velocity(edge).x, 0.0f, 1e-4f) && Near(s.Velocity(outside).x, 0.0f, 1e-4f) && Near(s.Velocity(fixed).x, 0.0f, 1e-6f) && Near(s.Velocity(kinematic).x, 0.0f, 1e-6f));

    // `upward` adds a lift: at 1 the direction is 45 degrees up and the change of velocity keeps its size.
    Sim t;
    const Entity lifted = t.Ball(Vec3(2, 0, 0));
    t.scene.Sync();
    CHECK(t.system.Explode(Vec3(0, 0, 0), 10.0f, 10.0f, 1.0f) == 1);
    CHECK(NearRel(t.Velocity(lifted).y, t.Velocity(lifted).x, 0.02f) && t.Velocity(lifted).y > 4.0f && NearRel(t.Velocity(lifted).Length(), 8.0f, 0.02f));

    // What it refuses: a radius that is not positive or not a number, a strength or lift that is not a number, a centre that is not a number.
    Sim u;
    const Entity probe = u.Ball(Vec3(1, 0, 0));
    u.scene.Sync();
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    CHECK(u.system.Explode(Vec3(0, 0, 0), 0.0f, 10.0f) == 0 && u.system.Explode(Vec3(0, 0, 0), -5.0f, 10.0f) == 0 && u.system.Explode(Vec3(0, 0, 0), nan, 10.0f) == 0);
    CHECK(u.system.Explode(Vec3(0, 0, 0), 5.0f, nan) == 0 && u.system.Explode(Vec3(0, 0, 0), 5.0f, 10.0f, nan) == 0 && u.system.Explode(Vec3(nan, 0, 0), 5.0f, 10.0f) == 0);
    CHECK(u.Velocity(probe).LengthSq() < 1e-8f); // none of the refused calls touched the body
}

AETHER_TEST(ForceFields_TheKitRegistersTheComponentAndInstallsTheScriptTable) {
    const std::unique_ptr<kit::IKit> kit = MakeForceFieldsKit();
    CHECK(std::string(kit->Name()) == "ForceFields" && kit->Deps().empty() && kit->Stages().empty());
    // It makes no runtime: the system points at the physics scene, which the host owns it beside (see Game::Physics).
    Sim s;
    kit::KitRuntimeContext context;
    context.world = &s.world;
    CHECK(kit->CreateRuntime(context) == nullptr);
    kit->RegisterComponents();
    CHECK(FindComponentIdByName("ForceField") != kInvalidComponentId);
    std::string installed;
    kit::KitScriptApiContext api;
    api.install = [&](const std::string& name) { installed = name; };
    kit->InstallScriptApi(&api);
    CHECK(installed == "ForceFields");

    // The system itself: one push per field per body, counted.
    s.Field(Wind(10.0f, Vec3(1, 0, 0), 50.0f));
    s.Field(Wind(1.0f, Vec3(0, 0, 1), 50.0f));
    const Entity ball = s.Ball(Vec3(0, 0, 0));
    s.scene.Sync();
    CHECK(s.system.Apply() == 2); // two fields, one body: two pushes (they add up)
    s.Run(30);
    CHECK(s.Velocity(ball).x > 4.0f && s.Velocity(ball).z > 0.3f);
}

namespace {

nlohmann::json AssetEntry(const char* path, const char* importer) {
    return {{"guid", assets::ToString(assets::NewAssetGuid())}, {"path", path}, {"importer", importer}};
}

} // namespace

#if AETHER_TEST_HAS_SCRIPTING
// The real player: a wind in the scene pushes a ball every physics step (the step applies it), and a script's Forces.Explode pushes a ball far from the wind.
AETHER_TEST(ForceFields_PlayerAppliesFieldsAndAScriptExplodes) {
    if (!player::Game::HasPhysics() || !player::Game::HasScripting()) return;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_forcefields_player_tests";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string script_guid = assets::ToString(assets::NewAssetGuid());
    ScriptComponent component;
    assets::AssetGuid parsed;
    CHECK(assets::ParseAssetGuid(script_guid, parsed));
    component.script.guid = parsed;
    RigidBody rb;
    rb.linear_damping = 0.0f;
    SphereCollider sphere;
    sphere.radius = 0.5f;
    const auto at = [](f32 x, f32 y, f32 z) { return reflect::ToJson(Transform{Vec3(x, y, z), Quaternion::Identity()}); };
    const nlohmann::json scene = {{"$type", "Scene"}, {"$version", 1}, {"entities", nlohmann::json::array({
        {{"components", {{"Transform", at(0, 0, 0)}, {"ForceField", reflect::ToJson(Wind(10.0f, Vec3(1, 0, 0), 6.0f))}}}},
        {{"components", {{"Transform", at(1, 0, 0)}, {"RigidBody", reflect::ToJson(rb)}, {"SphereCollider", reflect::ToJson(sphere)}, {"Tags", reflect::Json{{"names", {"InWind"}}}}}}},
        {{"components", {{"Transform", at(50, 0, 0)}, {"RigidBody", reflect::ToJson(rb)}, {"SphereCollider", reflect::ToJson(sphere)}, {"Tags", reflect::Json{{"names", {"Far"}}}}}}},
        {{"components", {{"ScriptComponent", reflect::ToJson(component)}}}}})}};
    const std::string script = "local S = {}\nfunction S:OnUpdate(dt)\n  if self.done then return end\n  self.done = true\n"
                               "  SaveGames.SetNumber('pushed', Forces.Explode(vector.create(50, 0, 0), 4, 6))\nend\nreturn S\n";
    const nlohmann::json assets_json = nlohmann::json::array({AssetEntry("Scenes/start.ascene", "Scene"), {{"guid", script_guid}, {"path", "Scripts/Boom.luau"}, {"importer", "Script"}}});
    const nlohmann::json manifest = {{"$type", "CookManifest"}, {"$version", 1}, {"project", "Demo"}, {"configuration", "Development"},
                                     {"startup_scene", "Scenes/start.ascene"}, {"fixed_timestep_hz", 60.0}, {"gravity", {0.0, 0.0, 0.0}},
                                     {"layers", {"Default"}}, {"collision_matrix", nlohmann::json::array()}, {"assets", assets_json}, {"files", nlohmann::json::array()}};
    pak::PakWriter writer;
    CHECK(writer.Add("Manifest.json", manifest.dump(2)));
    CHECK(writer.Add("Content/Scenes/start.ascene", scene.dump()));
    CHECK(writer.Add("Content/Scripts/Boom.luau", script));
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
    for (int i = 0; i < 30; ++i) game.Tick(1.0f / 60.0f);
    CHECK(game.ScriptErrors().empty());
    const auto by_tag = [&](const char* tag) { return FindEntitiesWithTag(game.GetWorld(), tag); };
    const std::vector<Entity> in_wind = by_tag("InWind"), far = by_tag("Far");
    CHECK(in_wind.size() == 1 && far.size() == 1);
    if (in_wind.size() != 1 || far.size() != 1) return;
    JPH::BodyInterface& bodies = game.PhysicsQueries()->Physics().BodyInterface();
    // Half a second in the wind: ~5 m/s along x, and the ball has moved; the script's blast pushed the far ball away from its centre at 6 m/s (it sits on it:
    // straight up).
    const JPH::Vec3 wind_v = bodies.GetLinearVelocity(game.PhysicsQueries()->BodyOf(in_wind[0]));
    CHECK(wind_v.GetX() > 3.5f && wind_v.GetX() < 6.5f);
    CHECK(game.GetWorld().GetComponent<Transform>(in_wind[0])->position.x > 1.3f);
    CHECK(game.Saves()->Bag().GetInt("pushed") == 1);
    const JPH::Vec3 far_v = bodies.GetLinearVelocity(game.PhysicsQueries()->BodyOf(far[0]));
    CHECK(far_v.GetY() > 5.0f && std::fabs(far_v.GetX()) < 0.1f);
}
#endif

#endif
