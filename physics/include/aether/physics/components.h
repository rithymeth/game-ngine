#pragma once

#include "aether/math/math.h"
#include "aether/reflection/reflection.h"
#include "aether/scene/components.h" // Transform

// Jolt.h must be included before any other Jolt header.
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>

#include <vector>

namespace aether {

class World;

// Physics components (Phase 13 step 1, docs/design/PHASE_SPECS.md §13):
// colliders give an entity its shape, RigidBody says how it moves. An
// entity with colliders and no RigidBody is static. Several colliders on
// one entity make one compound body.

enum class BodyMotion : u8 {
    Static,    // never moves (floors, walls)
    Kinematic, // moved by gameplay (Transform), pushes dynamic bodies
    Dynamic,   // moved by the simulation
};

// How a body moves: motion only, no shape (that's the colliders').
// `body_id` is a live Jolt handle, not meaningful across a save/load;
// PhysicsScene (or the editor) re-creates the body.
struct RigidBody {
    JPH::BodyID body_id;
    BodyMotion motion = BodyMotion::Dynamic;
    f32 mass = 1.0f; // Dynamic only
    f32 linear_damping = 0.05f;
    f32 angular_damping = 0.05f;
    f32 gravity_scale = 1.0f;
    bool continuous_collision = false; // CCD, for fast bodies that could tunnel through thin ones
    bool lock_rotation_x = false, lock_rotation_y = false, lock_rotation_z = false;

    // Scenes saved before the collider split kept a sphere's radius here.
    // Loading puts it back here; MigrateLegacyRigidBodies turns it into a
    // SphereCollider. 0 = none. Hidden in the Inspector.
    f32 legacy_radius = 0.0f;
};

// Every collider has a local offset from the entity's origin, a trigger
// flag (a sensor: reports overlaps, doesn't collide), and surface friction
// and restitution. A body's friction and restitution come from its first
// collider (Phase 13's physics materials refine this).
struct BoxCollider {
    Vec3 half_extents{0.5f, 0.5f, 0.5f};
    Vec3 center{0.0f, 0.0f, 0.0f};
    bool is_trigger = false;
    f32 friction = 0.5f;
    f32 restitution = 0.0f;
    bool report_stay = false; // OnCollisionStay every step while touching (off by default: it's every step)
};

struct SphereCollider {
    f32 radius = 0.5f;
    Vec3 center{0.0f, 0.0f, 0.0f};
    bool is_trigger = false;
    f32 friction = 0.5f;
    f32 restitution = 0.0f;
    bool report_stay = false; // OnCollisionStay every step while touching (off by default: it's every step)
};

// Upright (along Y). `height` is the whole capsule, caps included.
struct CapsuleCollider {
    f32 radius = 0.35f;
    f32 height = 1.8f;
    Vec3 center{0.0f, 0.0f, 0.0f};
    bool is_trigger = false;
    f32 friction = 0.5f;
    f32 restitution = 0.0f;
    bool report_stay = false; // OnCollisionStay every step while touching (off by default: it's every step)
};

// The convex hull of a point cloud (a rock, a crate with bevels).
struct ConvexCollider {
    std::vector<Vec3> points;
    Vec3 center{0.0f, 0.0f, 0.0f};
    bool is_trigger = false;
    f32 friction = 0.5f;
    f32 restitution = 0.0f;
    bool report_stay = false; // OnCollisionStay every step while touching (off by default: it's every step)
};

// A triangle mesh (level geometry). Only static bodies can use the triangles
// themselves; a kinematic or dynamic body gets the convex hull of the
// vertices instead, as in Unity and Unreal.
struct MeshCollider {
    std::vector<Vec3> vertices;
    std::vector<u32> indices; // three per triangle
    Vec3 center{0.0f, 0.0f, 0.0f};
    bool is_trigger = false;
    f32 friction = 0.5f;
    f32 restitution = 0.0f;
    bool report_stay = false; // OnCollisionStay every step while touching (off by default: it's every step)
};

// Installs RigidBody's custom binary (de)serializer and its JSON migration
// from the pre-split format. Call once before any SaveScene/LoadScene that
// may touch RigidBody entities.
void RegisterPhysicsComponentSerializers();

// Gives every RigidBody that came from a pre-split scene (a legacy radius,
// no collider) a SphereCollider of that radius, and clears the legacy
// field. Returns how many were migrated. Call after loading a scene.
usize MigrateLegacyRigidBodies(World& world);

// True if the entity has any collider component.
bool HasCollider(const World& world, Entity entity);

} // namespace aether

// Reflection (Phase 6): drives the generic Inspector and JSON scenes.
// RigidBody's live `body_id` is deliberately not reflected. Version 2 is the
// motion-only RigidBody; version 1 (radius, mass, is_static) migrates.
AETHER_ENUM(aether::BodyMotion, 1, AETHER_ENUM_VALUE(Static), AETHER_ENUM_VALUE(Kinematic), AETHER_ENUM_VALUE(Dynamic))

AETHER_REFLECT(aether::RigidBody, 2,
    AETHER_FIELD(motion, Field_EditAnywhere, {.tooltip = "Static never moves; Kinematic is moved by gameplay; Dynamic by the simulation"}),
    // A zero-mass dynamic body isn't valid in Jolt.
    AETHER_FIELD(mass, Field_EditAnywhere, {.range_min = 0.01, .range_max = 10000.0, .units = "kg"}),
    AETHER_FIELD(linear_damping, Field_EditAnywhere, {.range_min = 0.0, .range_max = 10.0}),
    AETHER_FIELD(angular_damping, Field_EditAnywhere, {.range_min = 0.0, .range_max = 10.0}),
    AETHER_FIELD(gravity_scale, Field_EditAnywhere, {.range_min = -10.0, .range_max = 10.0}),
    AETHER_FIELD(continuous_collision, Field_EditAnywhere, {.tooltip = "Continuous collision detection, for fast bodies"}),
    AETHER_FIELD(lock_rotation_x, Field_EditAnywhere),
    AETHER_FIELD(lock_rotation_y, Field_EditAnywhere),
    AETHER_FIELD(lock_rotation_z, Field_EditAnywhere),
    AETHER_FIELD(legacy_radius, Field_None)
)

AETHER_REFLECT(aether::BoxCollider, 1,
    AETHER_FIELD(half_extents, Field_EditAnywhere, {.tooltip = "Half the box's size along each axis", .units = "m"}),
    AETHER_FIELD(center, Field_EditAnywhere, {.units = "m"}),
    AETHER_FIELD(is_trigger, Field_EditAnywhere, {.tooltip = "Reports overlaps (OnTriggerEnter/Exit) instead of colliding"}),
    AETHER_FIELD(friction, Field_EditAnywhere, {.range_min = 0.0, .range_max = 2.0}),
    AETHER_FIELD(restitution, Field_EditAnywhere, {.range_min = 0.0, .range_max = 1.0}),
    AETHER_FIELD(report_stay, Field_EditAnywhere, {.tooltip = "Send OnCollisionStay every step while touching"})
)

AETHER_REFLECT(aether::SphereCollider, 1,
    AETHER_FIELD(radius, Field_EditAnywhere, {.range_min = 0.01, .range_max = 100.0, .units = "m"}),
    AETHER_FIELD(center, Field_EditAnywhere, {.units = "m"}),
    AETHER_FIELD(is_trigger, Field_EditAnywhere, {.tooltip = "Reports overlaps (OnTriggerEnter/Exit) instead of colliding"}),
    AETHER_FIELD(friction, Field_EditAnywhere, {.range_min = 0.0, .range_max = 2.0}),
    AETHER_FIELD(restitution, Field_EditAnywhere, {.range_min = 0.0, .range_max = 1.0}),
    AETHER_FIELD(report_stay, Field_EditAnywhere, {.tooltip = "Send OnCollisionStay every step while touching"})
)

AETHER_REFLECT(aether::CapsuleCollider, 1,
    AETHER_FIELD(radius, Field_EditAnywhere, {.range_min = 0.01, .range_max = 100.0, .units = "m"}),
    AETHER_FIELD(height, Field_EditAnywhere, {.tooltip = "The whole capsule, caps included", .range_min = 0.02, .range_max = 200.0, .units = "m"}),
    AETHER_FIELD(center, Field_EditAnywhere, {.units = "m"}),
    AETHER_FIELD(is_trigger, Field_EditAnywhere, {.tooltip = "Reports overlaps (OnTriggerEnter/Exit) instead of colliding"}),
    AETHER_FIELD(friction, Field_EditAnywhere, {.range_min = 0.0, .range_max = 2.0}),
    AETHER_FIELD(restitution, Field_EditAnywhere, {.range_min = 0.0, .range_max = 1.0}),
    AETHER_FIELD(report_stay, Field_EditAnywhere, {.tooltip = "Send OnCollisionStay every step while touching"})
)

AETHER_REFLECT(aether::ConvexCollider, 1,
    AETHER_FIELD(points, Field_EditAnywhere, {.tooltip = "The hull is built around these points", .units = "m"}),
    AETHER_FIELD(center, Field_EditAnywhere, {.units = "m"}),
    AETHER_FIELD(is_trigger, Field_EditAnywhere, {.tooltip = "Reports overlaps (OnTriggerEnter/Exit) instead of colliding"}),
    AETHER_FIELD(friction, Field_EditAnywhere, {.range_min = 0.0, .range_max = 2.0}),
    AETHER_FIELD(restitution, Field_EditAnywhere, {.range_min = 0.0, .range_max = 1.0}),
    AETHER_FIELD(report_stay, Field_EditAnywhere, {.tooltip = "Send OnCollisionStay every step while touching"})
)

AETHER_REFLECT(aether::MeshCollider, 1,
    AETHER_FIELD(vertices, Field_ReadOnly, {.units = "m"}),
    AETHER_FIELD(indices, Field_ReadOnly, {.tooltip = "Three per triangle"}),
    AETHER_FIELD(center, Field_EditAnywhere, {.units = "m"}),
    AETHER_FIELD(is_trigger, Field_EditAnywhere, {.tooltip = "Reports overlaps (OnTriggerEnter/Exit) instead of colliding"}),
    AETHER_FIELD(friction, Field_EditAnywhere, {.range_min = 0.0, .range_max = 2.0}),
    AETHER_FIELD(restitution, Field_EditAnywhere, {.range_min = 0.0, .range_max = 1.0}),
    AETHER_FIELD(report_stay, Field_EditAnywhere, {.tooltip = "Send OnCollisionStay every step while touching"})
)
