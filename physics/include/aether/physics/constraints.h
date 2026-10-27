#pragma once

#include "aether/math/math.h"
#include "aether/physics/components.h"
#include "aether/reflection/reflection.h"
#include "aether/scene/entity_guid.h"

#include <optional>
#include <span>
#include <vector>

namespace aether {

// Physics constraints (docs/design/PHYSICS_CONSTRAINTS.md): authored joints
// between two bodies, runtime grabs (a holder pulling a body toward a target)
// and the break events and network snapshots both produce. Game-agnostic:
// doors, tethers, carried furniture and breakable props are all built from
// these pieces.

enum class JointType : u8 {
    Fixed,    // welds the two bodies at their current relative pose
    Point,    // ball-and-socket at `anchor`
    Hinge,    // rotates about `axis` through `anchor`, optionally limited and motorized
    Distance, // keeps `anchor` and `connected_anchor` between min and max distance apart
};

enum class JointMotor : u8 {
    None,
    Velocity, // drives the hinge at `motor_target` degrees per second
    Position, // drives the hinge toward `motor_target` degrees
};

// A joint from this entity's body to `connected`'s body (a null GUID: the
// world). The connected entity is found by its IdComponent. PhysicsScene
// creates the Jolt constraint once both bodies exist, rebuilds it when the
// settings below or either body change, and removes it with the component,
// either entity, or either body. The joint is set up in world space from the
// bodies' poses when it is created: that pose is the hinge's angle 0.
//
// `enabled`, `locked`, `broken`, `motor_target` and `angle` are live: changing
// them never rebuilds the joint.
struct PhysicsJoint {
    JointType type = JointType::Hinge;
    EntityGuid connected;                   // null = the world
    Vec3 anchor{0.0f, 0.0f, 0.0f};          // in this entity's space
    Vec3 connected_anchor{0.0f, 0.0f, 0.0f}; // Distance: in `connected`'s space (world space when it is the world)
    Vec3 axis{0.0f, 1.0f, 0.0f};            // Hinge: in this entity's space

    // Hinge. min <= 0 <= max, degrees; -180/180 is unlimited.
    f32 min_angle = -180.0f;
    f32 max_angle = 180.0f;
    JointMotor motor = JointMotor::None;
    f32 motor_max_torque = 100.0f; // N m
    f32 motor_frequency = 2.0f;    // Position motor spring, Hz
    f32 motor_damping = 1.0f;      // Position motor spring damping ratio

    // Distance. A negative max is the distance at creation.
    f32 min_distance = 0.0f;
    f32 max_distance = -1.0f;
    f32 spring_frequency = 0.0f; // > 0 makes the limits a spring (Hz)
    f32 spring_damping = 0.0f;

    // Breaking: 0 never breaks. A joint breaks when the force (N) or torque
    // (N m) it carried in a step exceeds these; the lock (below) breaks when
    // its force or torque exceeds lock_break_force, leaving the joint itself.
    f32 break_force = 0.0f;
    f32 break_torque = 0.0f;
    f32 lock_break_force = 0.0f;

    // Live state.
    bool enabled = true;
    bool locked = false;    // adds a rigid weld at the current pose (a braced door)
    bool broken = false;    // set when it breaks; clear it to re-arm
    f32 motor_target = 0.0f; // degrees (Position) or degrees per second (Velocity)
    f32 angle = 0.0f;       // read back each step: hinge degrees, distance metres
};

// Runtime grabs: a holder pulls a body point toward a target with a spring
// whose force is capped at max_force, so several holders' forces add up.
using GrabId = u32;
inline constexpr GrabId kInvalidGrab = 0;
struct GrabSettings {
    f32 max_force = 400.0f;  // N
    f32 frequency = 4.0f;    // spring, Hz
    f32 damping = 1.0f;      // damping ratio
    f32 break_force = 0.0f;  // > 0: released once the pull reaches this (N)
    f32 break_distance = 0.0f; // > 0: released once the point is this far from the target (m)
};

enum class ConstraintBreakKind : u8 { Joint, Lock, Grab };
// Reported once per break, after the step it happened in. Joint/Lock:
// `entity` owns the PhysicsJoint and `other` is the connected entity (null
// for the world). Grab: `entity` is the grabbed body's, `other` the holder.
struct ConstraintBreakEvent {
    ConstraintBreakKind kind = ConstraintBreakKind::Joint;
    Entity entity;
    Entity other;
    GrabId grab = kInvalidGrab;
    f32 force = 0.0f; // the force (N) or torque (N m) that broke it
};

// What a host sends clients about constraints: each joint's live state and
// the pose and velocity of every jointed or grabbed body, keyed by GUID
// (entities without an IdComponent are left out).
struct JointStateRecord {
    EntityGuid entity;
    bool enabled = true, locked = false, broken = false;
    f32 angle = 0.0f;
    f32 motor_target = 0.0f;
};
struct BodyStateRecord {
    EntityGuid entity;
    Vec3 position{0, 0, 0};
    Quaternion rotation = Quaternion::Identity();
    Vec3 linear_velocity{0, 0, 0};
    Vec3 angular_velocity{0, 0, 0};
};
struct ConstraintSnapshot {
    std::vector<JointStateRecord> joints; // sorted by GUID
    std::vector<BodyStateRecord> bodies;  // sorted by GUID
};

// Binary form: "AECS", u16 version 1, then counted joint and body records,
// little-endian. Deserialize rejects anything malformed (wrong magic or
// version, truncated or trailing bytes, bad flags, non-finite numbers).
std::vector<u8> SerializeConstraintSnapshot(const ConstraintSnapshot& snapshot);
[[nodiscard]] bool DeserializeConstraintSnapshot(std::span<const u8> bytes, ConstraintSnapshot& out);

} // namespace aether

AETHER_ENUM(aether::JointType, 1, AETHER_ENUM_VALUE(Fixed), AETHER_ENUM_VALUE(Point), AETHER_ENUM_VALUE(Hinge), AETHER_ENUM_VALUE(Distance))
AETHER_ENUM(aether::JointMotor, 1, AETHER_ENUM_VALUE(None), AETHER_ENUM_VALUE(Velocity), AETHER_ENUM_VALUE(Position))

AETHER_REFLECT(aether::PhysicsJoint, 1,
    AETHER_FIELD(type, Field_EditAnywhere),
    AETHER_FIELD(connected, Field_EditAnywhere, {.tooltip = "The entity this joint connects to; none = the world"}),
    AETHER_FIELD(anchor, Field_EditAnywhere, {.tooltip = "Joint point in this entity's space", .units = "m"}),
    AETHER_FIELD(connected_anchor, Field_EditAnywhere, {.tooltip = "Distance: the point on the connected entity (world space for the world)", .units = "m"}),
    AETHER_FIELD(axis, Field_EditAnywhere, {.tooltip = "Hinge axis in this entity's space"}),
    AETHER_FIELD(min_angle, Field_EditAnywhere, {.range_min = -180.0, .range_max = 0.0, .units = "deg"}),
    AETHER_FIELD(max_angle, Field_EditAnywhere, {.range_min = 0.0, .range_max = 180.0, .units = "deg"}),
    AETHER_FIELD(motor, Field_EditAnywhere),
    AETHER_FIELD(motor_max_torque, Field_EditAnywhere, {.range_min = 0.0, .range_max = 100000.0, .units = "N m"}),
    AETHER_FIELD(motor_frequency, Field_EditAnywhere, {.range_min = 0.0, .range_max = 60.0, .units = "Hz"}),
    AETHER_FIELD(motor_damping, Field_EditAnywhere, {.range_min = 0.0, .range_max = 10.0}),
    AETHER_FIELD(min_distance, Field_EditAnywhere, {.range_min = 0.0, .range_max = 1000.0, .units = "m"}),
    AETHER_FIELD(max_distance, Field_EditAnywhere, {.tooltip = "Negative: the distance when the joint is created", .range_min = -1.0, .range_max = 1000.0, .units = "m"}),
    AETHER_FIELD(spring_frequency, Field_EditAnywhere, {.tooltip = "0 = hard limits", .range_min = 0.0, .range_max = 60.0, .units = "Hz"}),
    AETHER_FIELD(spring_damping, Field_EditAnywhere, {.range_min = 0.0, .range_max = 10.0}),
    AETHER_FIELD(break_force, Field_EditAnywhere, {.tooltip = "0 = unbreakable", .range_min = 0.0, .range_max = 1000000.0, .units = "N"}),
    AETHER_FIELD(break_torque, Field_EditAnywhere, {.tooltip = "0 = unbreakable", .range_min = 0.0, .range_max = 1000000.0, .units = "N m"}),
    AETHER_FIELD(lock_break_force, Field_EditAnywhere, {.tooltip = "Force/torque that breaks the lock only; 0 = unbreakable", .range_min = 0.0, .range_max = 1000000.0}),
    AETHER_FIELD(enabled, Field_EditAnywhere | Field_Replicated),
    AETHER_FIELD(locked, Field_EditAnywhere | Field_Replicated, {.tooltip = "Welds the joint at its current pose"}),
    AETHER_FIELD(broken, Field_EditAnywhere | Field_Replicated, {.tooltip = "Set when it breaks; clear to repair"}),
    AETHER_FIELD(motor_target, Field_EditAnywhere, {.tooltip = "Degrees (Position) or degrees per second (Velocity)"}),
    AETHER_FIELD(angle, Field_ReadOnly | Field_Replicated, {.tooltip = "Hinge degrees / distance metres, read back each step"})
)
