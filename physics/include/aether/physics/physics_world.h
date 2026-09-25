#pragma once

#include "aether/core/base.h"
#include "aether/ecs/world.h"
#include "aether/math/math.h"
#include "aether/physics/components.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <memory>

namespace aether {

// Owns a Jolt PhysicsSystem plus the allocator/job-system/layer-filter
// boilerplate Jolt requires, wrapped behind aether::Vec3/Quaternion so
// callers don't need to touch JPH:: types directly for the common case
// (create a body, step, read its transform back). Jolt's own thread pool
// drives the simulation internally — routing that through aether::JobSystem
// instead is future work, not done here, to keep this integration bounded.
class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    void SetGravity(const Vec3& gravity);

    // Static box, e.g. a ground plane. `half_extents` per Jolt convention
    // (half the box's size along each axis).
    JPH::BodyID CreateBox(const Vec3& position, const Vec3& half_extents);

    JPH::BodyID CreateSphere(const Vec3& position, f32 radius, f32 mass, bool is_static);

    void DestroyBody(JPH::BodyID id);

    void Step(f32 dt);

    Vec3 GetPosition(JPH::BodyID id) const;
    Quaternion GetRotation(JPH::BodyID id) const;

    JPH::PhysicsSystem& System() { return *physics_system_; }
    JPH::BodyInterface& BodyInterface();

private:
    struct Layers; // Jolt broadphase/object layer glue; defined in the .cpp

    std::unique_ptr<JPH::TempAllocatorImpl> temp_allocator_;
    std::unique_ptr<JPH::JobSystemThreadPool> job_system_;
    std::unique_ptr<Layers> layers_;
    std::unique_ptr<JPH::PhysicsSystem> physics_system_;
};

// Steps `physics` by `dt`, then writes each RigidBody entity's simulated
// position/rotation back into its Transform component. Entities need both
// components; one without the other is left untouched.
void SyncPhysicsToTransforms(World& world, PhysicsWorld& physics, f32 dt);

} // namespace aether
