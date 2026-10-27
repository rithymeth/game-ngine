#pragma once

#include "aether/physics/physics_scene.h"

namespace aether {

// Server-side bridge. Instantiate with net::NetPhysicsState in the game/network integration layer.
// This physics module must not depend on the higher-level net module.
// Call after PhysicsScene::Step to copy live Jolt
// velocities/activation state into the ECS component replicated by net.
// Does not create components, mutate Jolt, or run client-side simulation.
// Returns the number of dynamic bodies updated.
template <typename MotionState>
inline usize CaptureAuthoritativePhysicsState(World& world, PhysicsScene& scene) {
    usize updated = 0;
    world.ForEach<RigidBody, MotionState>(
        [&](RigidBody& body, MotionState& state) {
            if (body.motion != BodyMotion::Dynamic || body.body_id.IsInvalid()) return;
            const auto& interface = scene.Physics().BodyInterface();
            const auto velocity = interface.GetLinearVelocity(body.body_id);
            const auto angular = interface.GetAngularVelocity(body.body_id);
            state.linear_velocity = Vec3{velocity.GetX(), velocity.GetY(), velocity.GetZ()};
            state.angular_velocity = Vec3{angular.GetX(), angular.GetY(), angular.GetZ()};
            state.sleeping = !interface.IsActive(body.body_id);
            ++updated;
        });
    return updated;
}

} // namespace aether
