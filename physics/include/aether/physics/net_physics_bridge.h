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

// Reconcile a replicated authoritative pose with an existing client Jolt
// dynamic body. PhysicsScene updates its tracked pose even for tiny errors,
// avoiding an unwanted wake/teleport during the next Step. Jolt activation
// is left untouched here; apply NetPhysicsState with the motion helper after.
// The ECS Transform remains authoritative and visual interpolation is separate.
template <typename MotionState>
inline usize ReconcileAuthoritativePhysicsPose(World& world, PhysicsScene& scene,
                                               f32 position_tolerance = 0.01f,
                                               f32 rotation_tolerance_radians = 0.01f) {
    usize corrected = 0;
    world.ForEach<Transform, RigidBody, MotionState>(
        [&](Transform&, RigidBody& body, MotionState&) {
            if (body.motion != BodyMotion::Dynamic || body.body_id.IsInvalid()) return;
            const Entity entity = scene.EntityOf(body.body_id);
            if (entity.IsNull() || scene.BodyOf(entity) != body.body_id) return;
            if (scene.ReconcileAuthoritativePose(entity, position_tolerance, rotation_tolerance_radians))
                ++corrected;
        });
    return corrected;
}

// Compatibility entry point: authoritative snapshots contain orientation as
// well as position. Use the pose helper directly for an explicit angle limit.
template <typename MotionState>
inline usize ReconcileAuthoritativePhysicsPosition(World& world, PhysicsScene& scene, f32 tolerance = 0.01f) {
    return ReconcileAuthoritativePhysicsPose<MotionState>(world, scene, tolerance);
}

} // namespace aether
