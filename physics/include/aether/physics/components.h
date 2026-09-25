#pragma once

#include "aether/math/math.h"

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
