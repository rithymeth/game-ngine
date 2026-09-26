#pragma once

#include "aether/math/math.h"
#include "aether/reflection/reflection.h"

// Jolt.h must be included before any other Jolt header.
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>

namespace aether {

// World-space transform. Plain data — round-trips through the default
// raw-byte component serializer with no special handling.
struct Transform {
    Vec3 position;
    Quaternion rotation;
};

// A dynamic or static physics body. `body_id` is a live Jolt handle, not
// meaningful across a save/load — RegisterPhysicsComponentSerializers()
// installs a custom serializer that (de)serializes shape/mass instead, and
// PhysicsWorld re-creates the Jolt body on load.
struct RigidBody {
    JPH::BodyID body_id;
    f32 radius = 0.5f;
    f32 mass = 1.0f;
    bool is_static = false;
};

// Installs RigidBody's custom (de)serializer (SetComponentSerializer<RigidBody>).
// Call once before any SaveScene/LoadScene that may touch RigidBody entities.
void RegisterPhysicsComponentSerializers();

} // namespace aether

// Reflection (Phase 6): drives the generic Inspector and JSON scenes.
// RigidBody's live `body_id` is deliberately not reflected. Its binary scene
// serializer stays the custom one installed by
// RegisterPhysicsComponentSerializers(), so existing .aesc files keep
// loading unchanged.
AETHER_REFLECT(aether::Transform, 1,
    AETHER_FIELD(position, Field_EditAnywhere, {.units = "m"}),
    AETHER_FIELD(rotation, Field_EditAnywhere)
)

AETHER_REFLECT(aether::RigidBody, 1,
    AETHER_FIELD(radius, Field_EditAnywhere, {.range_min = 0.01, .range_max = 100.0, .units = "m"}),
    AETHER_FIELD(mass, Field_EditAnywhere, {.range_min = 0.0, .range_max = 10000.0, .units = "kg"}),
    AETHER_FIELD(is_static, Field_EditAnywhere)
)
