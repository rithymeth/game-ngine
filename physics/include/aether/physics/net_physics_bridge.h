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

// Client-side motion correction after receiving a server snapshot.
// The authoritative Transform is applied separately by replication; this
// helper only reconciles Jolt velocities and activation for existing dynamic
// bodies. Call outside PhysicsScene::Step, after applying the snapshot.
// Returns the number of bodies updated. Does not create or destroy bodies.
template <typename MotionState>
inline usize ApplyAuthoritativePhysicsMotion(World& world, PhysicsScene& scene) {
    usize updated = 0;
    world.ForEach<RigidBody, MotionState>(
        [&](RigidBody& body, MotionState& state) {
            if (body.motion != BodyMotion::Dynamic || body.body_id.IsInvalid()) return;
            auto& interface = scene.Physics().BodyInterface();
            const JPH::BodyID id = body.body_id;
            if (scene.BodyOf(scene.EntityOf(id)) != id) return;
            interface.SetLinearAndAngularVelocity(
                id,
                JPH::Vec3(state.linear_velocity.x, state.linear_velocity.y, state.linear_velocity.z),
                JPH::Vec3(state.angular_velocity.x, state.angular_velocity.y, state.angular_velocity.z));
            if (state.sleeping) interface.DeactivateBody(id);
            else interface.ActivateBody(id);
            ++updated;
        });
    return updated;
}

// Reconcile the Jolt pose with a Transform already updated by authoritative
// replication. Unlike gameplay motion, this is a snapshot correction: it
// teleports only existing dynamic bodies when position error exceeds the
// caller's tolerance. This helper does not interpolate or alter the ECS
// Transform. Call after snapshot application and before the next physics step.
template <typename MotionState>
inline usize ReconcileAuthoritativePhysicsPosition(World& world, PhysicsScene& scene, f32 tolerance = 0.01f) {
    usize corrected = 0;
    const f32 threshold = tolerance > 0.0f ? tolerance : 0.0f;
    world.ForEach<Transform, RigidBody, MotionState>(
        [&](Transform& transform, RigidBody& body, MotionState&) {
            if (body.motion != BodyMotion::Dynamic || body.body_id.IsInvalid()) return;
            const JPH::BodyID id = body.body_id;
            if (scene.BodyOf(scene.EntityOf(id)) != id) return;
            const Vec3 error = scene.Physics().GetPosition(id) - transform.position;
            if (error.Length() <= threshold) return;
            scene.Physics().SetPosition(id, transform.position);
            ++corrected;
        });
    return corrected;
}

} // namespace aether
