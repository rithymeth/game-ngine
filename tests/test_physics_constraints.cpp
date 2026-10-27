#include "aether/job/job_system.h"
#include "aether/physics/constraints.h"
#include "aether/physics/physics_scene.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/serialization.h"
#include "test_framework.h"

#if AETHER_TEST_HAS_SCRIPTING
#include "aether/script/script_system.h"
#endif

#include <Jolt/Physics/Body/BodyInterface.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

using namespace aether;

// Physics constraints (docs/design/PHYSICS_CONSTRAINTS.md): PhysicsJoint,
// runtime grabs, breaks, lifetimes, serialization, determinism and the
// network snapshot.

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

struct Sim {
    JobSystem jobs{2};
    World world;
    PhysicsWorld physics{jobs};
    PhysicsScene scene{world, physics};
    u64 next_guid = 1;

    Sim() {
        RegisterPhysicsComponentSerializers();
        (void)GetComponentId<PhysicsJoint>();
    }
    EntityGuid Guid() { return EntityGuid{0xA37E5ull, next_guid++}; }
    Entity Floor() { // top at y = 0.5
        BoxCollider box;
        box.half_extents = Vec3(20, 0.5f, 20);
        return world.CreateEntity(Transform{Vec3(0, 0, 0), Quaternion::Identity()}, box, IdComponent{Guid()});
    }
    Entity Box(const Vec3& at, const Vec3& half, f32 mass, BodyMotion motion = BodyMotion::Dynamic) {
        RigidBody rb;
        rb.motion = motion;
        rb.mass = mass;
        rb.linear_damping = 0.0f;
        rb.angular_damping = 0.0f;
        BoxCollider box;
        box.half_extents = half;
        return world.CreateEntity(Transform{at, Quaternion::Identity()}, rb, box, IdComponent{Guid()});
    }
    Entity Ball(const Vec3& at, f32 mass) {
        RigidBody rb;
        rb.mass = mass;
        rb.linear_damping = 0.0f;
        rb.angular_damping = 0.0f;
        SphereCollider s;
        s.radius = 0.2f;
        return world.CreateEntity(Transform{at, Quaternion::Identity()}, rb, s, IdComponent{Guid()});
    }
    EntityGuid GuidOf(Entity e) { return world.GetComponent<IdComponent>(e)->guid; }
    PhysicsJoint& Joint(Entity e) { return *world.GetComponent<PhysicsJoint>(e); }
    void Run(int steps) {
        for (int i = 0; i < steps; ++i) scene.Step(kDt);
    }
    Vec3 Pos(Entity e) { return world.GetComponent<Transform>(e)->position; }
    JPH::BodyInterface& Bodies() { return physics.BodyInterface(); }
    JPH::BodyID Body(Entity e) { return scene.BodyOf(e); }
};

f32 Length(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

// A 1 x 2 x 0.1 door hinged to the world along its left edge (x = 0).
Entity Door(Sim& sim, f32 min_angle = -180.0f, f32 max_angle = 180.0f) {
    const Entity door = sim.Box(Vec3(0.5f, 2.0f, 0.0f), Vec3(0.5f, 1.0f, 0.05f), 10.0f);
    PhysicsJoint joint;
    joint.type = JointType::Hinge;
    joint.anchor = Vec3(-0.5f, 0.0f, 0.0f);
    joint.axis = Vec3(0, 1, 0);
    joint.min_angle = min_angle;
    joint.max_angle = max_angle;
    sim.world.AddComponent(door, joint);
    return door;
}

} // namespace

AETHER_TEST(PhysicsConstraints_HingeRespectsLimits) {
    Sim sim;
    sim.physics.SetGravity(Vec3(0, 0, 0));
    const Entity door = Door(sim, -30.0f, 45.0f);
    sim.scene.Sync();
    AETHER_CHECK(sim.scene.JointCount() == 1);
    f32 highest = -1000.0f, lowest = 1000.0f;
    for (int i = 0; i < 90; ++i) {
        sim.Bodies().AddTorque(sim.Body(door), JPH::Vec3(0, 40, 0));
        sim.Run(1);
        highest = std::max(highest, sim.Joint(door).angle);
    }
    AETHER_CHECK(highest < 47.0f && highest > 40.0f);
    AETHER_CHECK(std::abs(sim.Joint(door).angle - 45.0f) < 2.0f);
    for (int i = 0; i < 120; ++i) {
        sim.Bodies().AddTorque(sim.Body(door), JPH::Vec3(0, -40, 0));
        sim.Run(1);
        lowest = std::min(lowest, sim.Joint(door).angle);
    }
    AETHER_CHECK(lowest > -32.0f && std::abs(sim.Joint(door).angle + 30.0f) < 2.0f);
    // The hinge pivot stays put.
    const Vec3 edge = sim.Pos(door);
    AETHER_CHECK(std::abs(edge.y - 2.0f) < 0.01f);
}

AETHER_TEST(PhysicsConstraints_HingeMotorDrivesAndRetargets) {
    Sim sim;
    sim.physics.SetGravity(Vec3(0, 0, 0));
    const Entity door = Door(sim, -90.0f, 90.0f);
    PhysicsJoint& joint = sim.Joint(door);
    joint.motor = JointMotor::Position;
    joint.motor_target = 60.0f;
    joint.motor_max_torque = 500.0f;
    sim.Run(180);
    AETHER_CHECK(std::abs(sim.Joint(door).angle - 60.0f) < 2.0f);
    sim.Joint(door).motor_target = -45.0f; // live: no rebuild
    sim.Run(180);
    AETHER_CHECK(std::abs(sim.Joint(door).angle + 45.0f) < 2.0f);
    AETHER_CHECK(sim.scene.JointCount() == 1);

    // A velocity motor turns at its target rate.
    Sim spin;
    spin.physics.SetGravity(Vec3(0, 0, 0));
    const Entity wheel = Door(spin);
    spin.Joint(wheel).motor = JointMotor::Velocity;
    spin.Joint(wheel).motor_target = 90.0f;
    spin.Joint(wheel).motor_max_torque = 500.0f;
    spin.Run(60);
    const f32 rate = spin.Bodies().GetAngularVelocity(spin.Body(wheel)).GetY();
    AETHER_CHECK(std::abs(rate - 1.5708f) < 0.05f);
}

AETHER_TEST(PhysicsConstraints_LockedDoorHoldsUnlocksAndRelocks) {
    Sim sim;
    sim.physics.SetGravity(Vec3(0, 0, 0));
    const Entity door = Door(sim, -90.0f, 90.0f);
    sim.Joint(door).locked = true;
    for (int i = 0; i < 60; ++i) {
        sim.Bodies().AddTorque(sim.Body(door), JPH::Vec3(0, 30, 0));
        sim.Run(1);
    }
    AETHER_CHECK(std::abs(sim.Joint(door).angle) < 1.0f);
    sim.Joint(door).locked = false;
    for (int i = 0; i < 30; ++i) {
        sim.Bodies().AddTorque(sim.Body(door), JPH::Vec3(0, 30, 0));
        sim.Run(1);
    }
    const f32 opened = sim.Joint(door).angle;
    AETHER_CHECK(opened > 20.0f);
    sim.Bodies().SetAngularVelocity(sim.Body(door), JPH::Vec3::sZero());
    sim.Joint(door).locked = true; // braced where it stands
    for (int i = 0; i < 60; ++i) {
        sim.Bodies().AddTorque(sim.Body(door), JPH::Vec3(0, -30, 0));
        sim.Run(1);
    }
    AETHER_CHECK(std::abs(sim.Joint(door).angle - opened) < 2.0f);
    AETHER_CHECK(sim.scene.ConstraintBreaks().empty());
}

AETHER_TEST(PhysicsConstraints_DistanceTetherCapsSeparationAndSprings) {
    for (int soft = 0; soft < 2; ++soft) {
        Sim sim;
        const Entity ball = sim.Ball(Vec3(0, 9.5f, 0), 2.0f);
        PhysicsJoint tether;
        tether.type = JointType::Distance;
        tether.connected_anchor = Vec3(0, 10, 0); // the world
        tether.max_distance = 2.0f;
        if (soft) {
            tether.spring_frequency = 1.0f;
            tether.spring_damping = 0.1f;
        }
        sim.world.AddComponent(ball, tether);
        f32 farthest = 0.0f;
        for (int i = 0; i < 240; ++i) {
            sim.Run(1);
            farthest = std::max(farthest, Length(sim.Pos(ball) - Vec3(0, 10, 0)));
        }
        AETHER_CHECK(std::abs(sim.Joint(ball).angle - Length(sim.Pos(ball) - Vec3(0, 10, 0))) < 1e-3f);
        if (!soft) {
            AETHER_CHECK(farthest < 2.03f);
            AETHER_CHECK(std::abs(sim.Pos(ball).y - 8.0f) < 0.03f);
        } else {
            AETHER_CHECK(farthest > 2.1f); // a spring stretches
        }
    }
    // A negative max is the length at creation.
    Sim sim;
    const Entity a = sim.Ball(Vec3(0, 5, 0), 1.0f);
    const Entity anchor = sim.Box(Vec3(3, 5, 0), Vec3(0.1f, 0.1f, 0.1f), 1.0f, BodyMotion::Static);
    PhysicsJoint tether;
    tether.type = JointType::Distance;
    tether.connected = sim.GuidOf(anchor);
    sim.world.AddComponent(a, tether);
    sim.Run(240);
    AETHER_CHECK(Length(sim.Pos(a) - Vec3(3, 5, 0)) < 3.03f);
}

AETHER_TEST(PhysicsConstraints_FixedAndPointJoints) {
    Sim sim;
    const Entity a = sim.Box(Vec3(0, 5, 0), Vec3(0.2f, 0.2f, 0.2f), 2.0f);
    const Entity b = sim.Box(Vec3(0.6f, 5, 0), Vec3(0.2f, 0.2f, 0.2f), 2.0f);
    PhysicsJoint pin;
    pin.type = JointType::Point;
    pin.anchor = Vec3(0, 0.5f, 0); // a pendulum pivot above a
    sim.world.AddComponent(a, pin);
    PhysicsJoint weld;
    weld.type = JointType::Fixed;
    weld.connected = sim.GuidOf(a);
    sim.world.AddComponent(b, weld);
    sim.Run(1);
    AETHER_CHECK(sim.scene.JointCount() == 2);
    for (int i = 0; i < 180; ++i) {
        sim.Run(1);
        // b stays 0.6 m from a, and a's pivot stays at (0, 5.5, 0).
        const JPH::RVec3 pa = sim.Bodies().GetPosition(sim.Body(a));
        const JPH::Quat ra = sim.Bodies().GetRotation(sim.Body(a));
        const JPH::RVec3 pivot = pa + ra * JPH::Vec3(0, 0.5f, 0);
        AETHER_CHECK(std::abs(pivot.GetY() - 5.5f) < 0.02f && std::abs(pivot.GetX()) < 0.02f);
        AETHER_CHECK(std::abs(Length(sim.Pos(b) - sim.Pos(a)) - 0.6f) < 0.01f);
    }
    // It swung: the pendulum isn't hanging straight under the pivot yet.
    AETHER_CHECK(sim.Pos(a).y < 5.4f);
}

AETHER_TEST(PhysicsConstraints_TwoHoldersCarryWhatOneCannot) {
    for (int holders = 1; holders <= 2; ++holders) {
        Sim sim;
        sim.Floor();
        const Entity crate = sim.Box(Vec3(0, 1.0f, 0), Vec3(0.5f, 0.5f, 0.5f), 50.0f); // 490 N
        const Entity alice = sim.world.CreateEntity(), bob = sim.world.CreateEntity();
        sim.Run(10);
        GrabSettings grip;
        grip.max_force = 300.0f;
        GrabId ga = sim.scene.Grab(crate, alice, Vec3(-0.5f, 1.5f, 0), grip);
        GrabId gb = holders == 2 ? sim.scene.Grab(crate, bob, Vec3(0.5f, 1.5f, 0), grip) : kInvalidGrab;
        AETHER_CHECK(ga != kInvalidGrab && sim.scene.IsGrabActive(ga));
        AETHER_CHECK(sim.scene.GrabCount(crate) == static_cast<usize>(holders));
        sim.scene.SetGrabTarget(ga, Vec3(-0.5f, 2.5f, 0));
        if (gb != kInvalidGrab) sim.scene.SetGrabTarget(gb, Vec3(0.5f, 2.5f, 0));
        sim.Run(120);
        if (holders == 1) {
            AETHER_CHECK(sim.Pos(crate).y < 1.3f); // too heavy for one
            continue;
        }
        AETHER_CHECK(std::abs(sim.Pos(crate).y - 2.0f) < 0.1f);
        // Sideways carry.
        for (int i = 1; i <= 120; ++i) {
            const f32 x = 2.0f * static_cast<f32>(i) / 120.0f;
            sim.scene.SetGrabTarget(ga, Vec3(x - 0.5f, 2.5f, 0));
            sim.scene.SetGrabTarget(gb, Vec3(x + 0.5f, 2.5f, 0));
            sim.Run(1);
        }
        sim.Run(60);
        AETHER_CHECK(std::abs(sim.Pos(crate).x - 2.0f) < 0.15f && std::abs(sim.Pos(crate).y - 2.0f) < 0.15f);
        // Bob lets go: Alice alone can't hold it.
        AETHER_CHECK(sim.scene.ReleaseHolder(bob) == 1);
        AETHER_CHECK(!sim.scene.IsGrabActive(gb) && sim.scene.GrabCount(crate) == 1);
        sim.Run(120);
        AETHER_CHECK(sim.Pos(crate).y < 1.3f);
        AETHER_CHECK(sim.scene.Release(ga) && !sim.scene.Release(ga));
        AETHER_CHECK(sim.scene.GrabCount(crate) == 0);
    }
}

AETHER_TEST(PhysicsConstraints_BreaksFireExactlyOnce) {
    Sim sim;
    std::vector<ConstraintBreakEvent> seen;
    sim.scene.SetConstraintBreakHandler([&](const ConstraintBreakEvent& e) { seen.push_back(e); });

    // A joint: a 5 kg weight on a 100 N chain holds, then a 2000 N yank snaps it.
    const Entity weight = sim.Ball(Vec3(-5, 5, 0), 5.0f);
    PhysicsJoint chain;
    chain.type = JointType::Point;
    chain.anchor = Vec3(0, 0.5f, 0);
    chain.break_force = 100.0f;
    sim.world.AddComponent(weight, chain);

    // A lock: a braced door that a strong shove breaks open (the hinge stays).
    const Entity door = sim.Box(Vec3(5.5f, 2.0f, 0.0f), Vec3(0.5f, 1.0f, 0.05f), 10.0f);
    PhysicsJoint hinge;
    hinge.type = JointType::Hinge;
    hinge.anchor = Vec3(-0.5f, 0, 0);
    hinge.locked = true;
    hinge.lock_break_force = 150.0f;
    sim.world.AddComponent(door, hinge);

    // A grab: a 50 kg crate pulled far away, breaking at 600 N.
    const Entity crate = sim.Box(Vec3(0, 10, 10), Vec3(0.5f, 0.5f, 0.5f), 50.0f);
    const Entity holder = sim.world.CreateEntity();

    sim.scene.Sync();
    GrabSettings grip;
    grip.max_force = 1000.0f;
    grip.break_force = 600.0f;
    const GrabId grab = sim.scene.Grab(crate, holder, Vec3(0, 10.5f, 10), grip); // holds its 490 N
    sim.Run(30);
    AETHER_CHECK(seen.empty() && sim.scene.JointCount() == 2);
    AETHER_CHECK(sim.scene.IsGrabActive(grab));

    sim.Bodies().AddForce(sim.Body(weight), JPH::Vec3(2000, 0, 0));
    sim.Bodies().AddTorque(sim.Body(door), JPH::Vec3(0, 4000, 0));
    sim.scene.SetGrabTarget(grab, Vec3(0, 20, 10));
    sim.Run(1);
    for (int i = 0; i < 60; ++i) {
        sim.Bodies().AddTorque(sim.Body(door), JPH::Vec3(0, 4000, 0));
        sim.Run(1);
    }
    usize joints = 0, locks = 0, grabs = 0;
    for (const ConstraintBreakEvent& e : seen) {
        if (e.kind == ConstraintBreakKind::Joint) {
            ++joints;
            AETHER_CHECK(e.entity == weight && e.other.IsNull() && e.force > 100.0f);
        } else if (e.kind == ConstraintBreakKind::Lock) {
            ++locks;
            AETHER_CHECK(e.entity == door && e.force > 150.0f);
        } else {
            ++grabs;
            AETHER_CHECK(e.entity == crate && e.other == holder && e.grab == grab && e.force >= 600.0f);
        }
    }
    AETHER_CHECK(joints == 1 && locks == 1 && grabs == 1);
    AETHER_CHECK(sim.Joint(weight).broken && !sim.Joint(door).locked && !sim.Joint(door).broken);
    AETHER_CHECK(!sim.scene.IsGrabActive(grab));
    AETHER_CHECK(sim.scene.JointCount() == 1); // the door's hinge survives its lock
    AETHER_CHECK(std::abs(sim.Joint(door).angle) > 10.0f);

    // Clearing `broken` re-arms it.
    sim.Bodies().SetLinearAndAngularVelocity(sim.Body(weight), JPH::Vec3::sZero(), JPH::Vec3::sZero()); // caught
    sim.Joint(weight).broken = false;
    sim.Run(1);
    AETHER_CHECK(sim.scene.JointCount() == 2 && seen.size() == 3);
}

AETHER_TEST(PhysicsConstraints_Lifetimes) {
    Sim sim;
    const Entity a = sim.Box(Vec3(0, 5, 0), Vec3(0.2f, 0.2f, 0.2f), 1.0f);
    PhysicsJoint joint;
    joint.type = JointType::Point;
    joint.connected = EntityGuid{0xBEEF, 1}; // not there yet
    sim.world.AddComponent(a, joint);
    sim.Run(1);
    AETHER_CHECK(sim.scene.JointCount() == 0);

    // The connected entity appears: the joint is made.
    RigidBody rb;
    BoxCollider box;
    box.half_extents = Vec3(0.2f, 0.2f, 0.2f);
    const Entity b = sim.world.CreateEntity(Transform{Vec3(1, 5, 0), Quaternion::Identity()}, rb, box, IdComponent{EntityGuid{0xBEEF, 1}});
    sim.Run(1);
    AETHER_CHECK(sim.scene.JointCount() == 1);

    // A settings change or a body rebuild keeps one joint.
    sim.Joint(a).type = JointType::Fixed;
    sim.Run(1);
    AETHER_CHECK(sim.scene.JointCount() == 1);
    sim.world.GetComponent<BoxCollider>(b)->half_extents = Vec3(0.3f, 0.3f, 0.3f);
    sim.Run(1);
    AETHER_CHECK(sim.scene.JointCount() == 1);
    // Live fields don't rebuild either.
    sim.Joint(a).enabled = false;
    sim.Run(1);
    AETHER_CHECK(sim.scene.JointCount() == 1);
    sim.Joint(a).enabled = true;

    // Destroying the connected entity removes the joint; so does removing the component.
    sim.world.DestroyEntity(b);
    sim.Run(1);
    AETHER_CHECK(sim.scene.JointCount() == 0);
    const Entity c = sim.Box(Vec3(2, 5, 0), Vec3(0.2f, 0.2f, 0.2f), 1.0f);
    sim.Joint(a).connected = sim.GuidOf(c);
    sim.Run(1);
    AETHER_CHECK(sim.scene.JointCount() == 1);
    sim.world.RemoveComponent<PhysicsJoint>(a);
    sim.Run(1);
    AETHER_CHECK(sim.scene.JointCount() == 0);

    // Grabs end with their body.
    const Entity holder = sim.world.CreateEntity();
    const GrabId g = sim.scene.Grab(c, holder, Vec3(2, 5, 0));
    AETHER_CHECK(sim.scene.IsGrabActive(g));
    AETHER_CHECK(sim.scene.Grab(holder, holder, Vec3(0, 0, 0)) == kInvalidGrab); // no body
    sim.world.DestroyEntity(c);
    sim.Run(1);
    AETHER_CHECK(!sim.scene.IsGrabActive(g) && sim.scene.GrabCount(c) == 0);

    // The scene's destructor removes live constraints before their bodies.
    {
        PhysicsScene other(sim.world, sim.physics);
        PhysicsJoint again;
        again.type = JointType::Hinge;
        sim.world.AddComponent(a, again);
        other.Sync();
        AETHER_CHECK(other.JointCount() == 1);
        (void)other.Grab(a, holder, Vec3(0, 5, 0));
        sim.world.RemoveComponent<PhysicsJoint>(a);
    }
    AETHER_CHECK(sim.physics.System().GetConstraints().size() == 0);
}

AETHER_TEST(PhysicsConstraints_SerializationRoundTrips) {
    RegisterPhysicsComponentSerializers();
    PhysicsJoint joint;
    joint.type = JointType::Distance;
    joint.connected = EntityGuid{0x1234, 0x5678};
    joint.anchor = Vec3(1, 2, 3);
    joint.connected_anchor = Vec3(-1, 0, 4);
    joint.min_distance = 0.5f;
    joint.max_distance = 3.0f;
    joint.spring_frequency = 2.0f;
    joint.break_force = 900.0f;
    joint.lock_break_force = 40.0f;
    joint.motor = JointMotor::Position;
    joint.enabled = false;
    joint.locked = true;
    joint.broken = true;
    joint.motor_target = 12.0f;
    joint.angle = 1.25f;
    auto same = [&](const PhysicsJoint& o) {
        return o.type == joint.type && o.connected == joint.connected && o.anchor.z == 3.0f && o.connected_anchor.z == 4.0f &&
               o.min_distance == 0.5f && o.max_distance == 3.0f && o.spring_frequency == 2.0f && o.break_force == 900.0f &&
               o.lock_break_force == 40.0f && o.motor == JointMotor::Position && !o.enabled && o.locked && o.broken &&
               o.motor_target == 12.0f && o.angle == 1.25f;
    };

    // Binary.
    PhysicsJoint from_binary;
    AETHER_CHECK(reflect::LoadBinary(from_binary, reflect::SaveBinary(joint)));
    AETHER_CHECK(same(from_binary));
    // JSON (the GUID as text).
    const reflect::Json json = reflect::ToJson(joint);
    AETHER_CHECK(json["connected"].is_string());
    PhysicsJoint from_json;
    AETHER_CHECK(reflect::FromJson(from_json, json));
    AETHER_CHECK(same(from_json));
    // A cooked (binary) scene.
    World world;
    world.CreateEntity(Transform{}, joint, IdComponent{EntityGuid{9, 9}});
    const std::vector<u8> cooked = SaveSceneToMemory(world);
    World loaded;
    AETHER_CHECK(LoadSceneFromMemory(loaded, cooked));
    usize found = 0;
    loaded.ForEachArchetype([&](Archetype& archetype) {
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            for (usize i = 0; i < archetype.ChunkEntityCount(c); ++i) {
                if (const PhysicsJoint* j = loaded.GetComponent<PhysicsJoint>(archetype.EntityArray(c)[i])) {
                    found += same(*j) ? 1 : 0;
                }
            }
        }
    });
    AETHER_CHECK(found == 1);

    // Exactly the live state replicates.
    std::set<std::string> replicated;
    for (const reflect::FieldInfo& field : reflect::Reflect<PhysicsJoint>().fields) {
        if (field.HasFlag(reflect::Field_Replicated)) replicated.insert(field.name);
    }
    AETHER_CHECK((replicated == std::set<std::string>{"enabled", "locked", "broken", "angle"}));
}

namespace {

// A small constrained scene: a motorized door, a tethered weight, a crate
// carried by two holders and a pinned pendulum.
struct Props {
    Entity door, weight, crate, pendulum, alice, bob;
    GrabId ga = kInvalidGrab, gb = kInvalidGrab;
};

Props Build(Sim& sim) {
    Props p;
    sim.Floor();
    p.door = Door(sim, -90.0f, 90.0f);
    sim.Joint(p.door).motor = JointMotor::Position;
    sim.Joint(p.door).motor_max_torque = 300.0f;
    p.weight = sim.Ball(Vec3(3, 6, 0), 3.0f);
    PhysicsJoint tether;
    tether.type = JointType::Distance;
    tether.connected_anchor = Vec3(4, 7, 0);
    tether.max_distance = 1.5f;
    sim.world.AddComponent(p.weight, tether);
    p.crate = sim.Box(Vec3(-3, 1.0f, 0), Vec3(0.5f, 0.5f, 0.5f), 50.0f);
    p.pendulum = sim.Box(Vec3(0, 6, 3), Vec3(0.2f, 0.2f, 0.2f), 2.0f);
    PhysicsJoint pin;
    pin.type = JointType::Point;
    pin.anchor = Vec3(0.6f, 0.4f, 0);
    sim.world.AddComponent(p.pendulum, pin);
    p.alice = sim.world.CreateEntity(IdComponent{sim.Guid()});
    p.bob = sim.world.CreateEntity(IdComponent{sim.Guid()});
    return p;
}

// The host's gameplay at step `i`.
void Play(Sim& sim, Props& p, int i) {
    if (i == 20) {
        GrabSettings grip;
        grip.max_force = 300.0f;
        p.ga = sim.scene.Grab(p.crate, p.alice, Vec3(-3.5f, 1.5f, 0), grip);
        p.gb = sim.scene.Grab(p.crate, p.bob, Vec3(-2.5f, 1.5f, 0), grip);
    }
    if (i > 20 && i <= 140) {
        const f32 t = static_cast<f32>(i - 20) / 120.0f;
        sim.scene.SetGrabTarget(p.ga, Vec3(-3.5f + 2.0f * t, 1.5f + t, 0));
        sim.scene.SetGrabTarget(p.gb, Vec3(-2.5f + 2.0f * t, 1.5f + t, 0));
    }
    if (i == 160) sim.scene.ReleaseHolder(p.alice);
    if (i == 170) sim.scene.ReleaseHolder(p.bob);
    if (i == 30) sim.Joint(p.door).motor_target = 70.0f;
    if (i == 120) sim.Joint(p.door).motor_target = -20.0f;
    if (i == 200) sim.Joint(p.door).locked = true;
}

bool SameBits(const Vec3& a, const Vec3& b) { return std::memcmp(&a, &b, sizeof(Vec3)) == 0; }

} // namespace

AETHER_TEST(PhysicsConstraints_DeterministicAcrossWorlds) {
    Sim a, b;
    Props pa = Build(a), pb = Build(b);
    bool identical = true;
    for (int i = 0; i < 240; ++i) {
        Play(a, pa, i);
        Play(b, pb, i);
        a.Run(1);
        b.Run(1);
        for (Entity e : {pa.door, pa.weight, pa.crate, pa.pendulum}) {
            identical = identical && SameBits(a.Pos(e), b.Pos(e));
        }
        identical = identical && a.Joint(pa.door).angle == b.Joint(pb.door).angle;
    }
    AETHER_CHECK(identical);
    AETHER_CHECK(SerializeConstraintSnapshot(a.scene.CaptureConstraintState()) ==
                 SerializeConstraintSnapshot(b.scene.CaptureConstraintState()));
    AETHER_CHECK(a.Pos(pa.crate).x > -2.5f); // it was carried
}

AETHER_TEST(PhysicsConstraints_SnapshotFormatIsValidated) {
    Sim sim;
    Props p = Build(sim);
    for (int i = 0; i < 60; ++i) {
        Play(sim, p, i);
        sim.Run(1);
    }
    const ConstraintSnapshot snapshot = sim.scene.CaptureConstraintState();
    AETHER_CHECK(snapshot.joints.size() == 3 && snapshot.bodies.size() == 4); // door, weight, pendulum (+ crate, grabbed)
    const std::vector<u8> bytes = SerializeConstraintSnapshot(snapshot);
    ConstraintSnapshot back;
    AETHER_CHECK(DeserializeConstraintSnapshot(bytes, back));
    AETHER_CHECK(SerializeConstraintSnapshot(back) == bytes);
    AETHER_CHECK(back.joints[0].entity == snapshot.joints[0].entity && back.bodies[3].position.x == snapshot.bodies[3].position.x);

    std::vector<u8> bad = bytes;
    bad[0] = 'X';
    AETHER_CHECK(!DeserializeConstraintSnapshot(bad, back));
    bad = bytes;
    bad[4] = 2; // version
    AETHER_CHECK(!DeserializeConstraintSnapshot(bad, back));
    bad = bytes;
    bad.pop_back();
    AETHER_CHECK(!DeserializeConstraintSnapshot(bad, back));
    bad = bytes;
    bad.push_back(0);
    AETHER_CHECK(!DeserializeConstraintSnapshot(bad, back));
    bad = bytes;
    bad[4 + 2 + 4 + 16] = 0xF0; // flags
    AETHER_CHECK(!DeserializeConstraintSnapshot(bad, back));
    bad = bytes;
    const f32 nan = std::nanf("");
    std::memcpy(bad.data() + 4 + 2 + 4 + 17, &nan, 4); // angle
    AETHER_CHECK(!DeserializeConstraintSnapshot(bad, back));
    AETHER_CHECK(!DeserializeConstraintSnapshot({}, back));
}

AETHER_TEST(PhysicsConstraints_ClientConvergesUnderSnapshotLoss) {
    Sim host, client;
    Props ph = Build(host), pc = Build(client);
    u32 rng = 12345u;
    auto lost = [&] {
        rng = rng * 1664525u + 1013904223u;
        return (rng >> 24) < 64; // 25%
    };
    usize sent = 0, applied = 0;
    f32 worst_late = 0.0f;
    const int kSteps = 420;
    for (int i = 0; i < kSteps; ++i) {
        Play(host, ph, i); // only the host plays; the client just simulates and corrects
        host.Run(1);
        client.Run(1);
        if (i % 3 == 2) {
            ++sent;
            const std::vector<u8> wire = SerializeConstraintSnapshot(host.scene.CaptureConstraintState());
            if (!lost()) {
                ConstraintSnapshot snapshot;
                AETHER_CHECK(DeserializeConstraintSnapshot(wire, snapshot));
                client.scene.ApplyConstraintState(snapshot);
                ++applied;
            }
        }
        if (i >= kSteps - 30) {
            for (Entity e : {ph.door, ph.weight, ph.crate, ph.pendulum}) {
                // The same entity index on both sides (built identically).
                worst_late = std::max(worst_late, Length(host.Pos(e) - client.Pos(e)));
            }
        }
    }
    AETHER_CHECK(applied > 0 && applied < sent);
    AETHER_CHECK(worst_late < 1e-4f);
    AETHER_CHECK(client.Joint(pc.door).locked && std::abs(client.Joint(pc.door).angle - host.Joint(ph.door).angle) < 1e-2f);
}

#if AETHER_TEST_HAS_SCRIPTING
AETHER_TEST(PhysicsConstraints_LuauBindings) {
    using namespace aether::script;
    Sim sim;
    sim.physics.SetGravity(Vec3(0, 0, 0));
    const Entity door = Door(sim, -90.0f, 90.0f);
    sim.Joint(door).motor = JointMotor::Position;
    const Entity crate = sim.Box(Vec3(5, 1, 0), Vec3(0.5f, 0.5f, 0.5f), 5.0f);
    const Entity holder = sim.world.CreateEntity(IdComponent{sim.Guid()});
    GuidIndex guids;
    guids.Rebuild(sim.world);
    LuauHost host;
    ScriptSystem scripts(host, sim.world, guids, [](const assets::AssetGuid&, std::string&, std::string&) { return false; });
    scripts.BindPhysicsScene(&sim.scene);
    sim.Run(1);
    host.SetGlobal("door", EntityRef{door});
    host.SetGlobal("crate", EntityRef{crate});
    host.SetGlobal("holder", EntityRef{holder});
    ScriptResult r = host.Run(R"(
        grab = Physics.Grab(crate, holder, vector.create(5, 1.5, 0), 500)
        assert(grab > 0 and Physics.IsGrabActive(grab))
        assert(Physics.SetGrabTarget(grab, vector.create(5, 3, 0)))
        assert(Physics.SetJointMotorTarget(door, 40))
        assert(Physics.SetJointEnabled(door, true))
        assert(Physics.SetJointLocked(door, false))
        assert(not Physics.RepairJoint(door))
        local enabled, locked, broken, angle = Physics.GetJointState(door)
        assert(enabled == true and locked == false and broken == false and type(angle) == "number")
        assert(Physics.GetJointState(crate) == nil)
        return grab
    )");
    AETHER_CHECK(r.ok);
    if (!r.ok) std::printf("%s\n", r.error.c_str());
    sim.Run(120);
    r = host.Run(R"(
        local _, _, _, angle = Physics.GetJointState(door)
        local released = Physics.Release(grab)
        return angle, released, Physics.IsGrabActive(grab)
    )");
    AETHER_CHECK(r.ok && r.values.size() == 3);
    if (r.ok && r.values.size() == 3) {
        AETHER_CHECK(std::abs(std::get<f64>(r.values[0]) - 40.0) < 3.0);
        AETHER_CHECK(std::get<bool>(r.values[1]) && !std::get<bool>(r.values[2]));
    }
    AETHER_CHECK(sim.Pos(crate).y > 2.3f);
    AETHER_CHECK(!host.Run("Physics.Grab(1, 2)").ok); // bad arguments fail
}
#endif
