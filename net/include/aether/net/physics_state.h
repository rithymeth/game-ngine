#pragma once

#include "aether/math/math.h"
#include "aether/reflection/reflection.h"

namespace aether::net {

// Replicated motion state for server-simulated physics props. Pair with
// NetIdentity and Transform: Transform carries position and rotation while
// this component carries velocities and sleep state. No Jolt BodyID crosses
// the wire (handles are process-local). The server remains authoritative;
// client-side smoothing and PhysicsScene integration are separate steps.
struct NetPhysicsState {
    Vec3 linear_velocity{0.0f, 0.0f, 0.0f};
    Vec3 angular_velocity{0.0f, 0.0f, 0.0f};
    bool sleeping = false;
};

} // namespace aether::net

AETHER_REFLECT(aether::net::NetPhysicsState, 1,
    AETHER_FIELD(linear_velocity, Field_Replicated),
    AETHER_FIELD(angular_velocity, Field_Replicated),
    AETHER_FIELD(sleeping, Field_Replicated)
)
