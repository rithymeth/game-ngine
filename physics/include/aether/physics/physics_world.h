#pragma once

#include "aether/core/base.h"
#include "aether/ecs/world.h"
#include "aether/job/job_system.h"
#include "aether/math/math.h"
#include "aether/physics/components.h"
#include "aether/scene/gameplay.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystem.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <memory>

namespace aether {

// Owns a Jolt PhysicsSystem plus the allocator/layer-filter boilerplate Jolt
// requires, wrapped behind aether::Vec3/Quaternion so callers don't need to
// touch JPH:: types directly for the common case (create a body, step, read
// its transform back). Simulation jobs run on the aether::JobSystem passed
// in — via JoltJobSystemAdapter — rather than a Jolt-owned thread pool, so
// physics shares the same worker threads as the rest of the engine. Step()
// (and anything else that reaches JPH::PhysicsSystem::Update) must be called
// from a thread registered with that JobSystem; see JoltJobSystemAdapter.
class PhysicsWorld {
public:
    explicit PhysicsWorld(JobSystem& job_system);
    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    void SetGravity(const Vec3& gravity);

    // Static box, e.g. a ground plane. `half_extents` per Jolt convention
    // (half the box's size along each axis).
    JPH::BodyID CreateBox(const Vec3& position, const Vec3& half_extents);

    JPH::BodyID CreateSphere(const Vec3& position, f32 radius, f32 mass, bool is_static);

    void DestroyBody(JPH::BodyID id);

    // Collision layers (Phase 13 step 2): which of the 32 game layers
    // collide, usually MakeCollisionMatrix(project). All collide by default.
    void SetCollisionMatrix(const CollisionMatrix& matrix);
    const CollisionMatrix& GetCollisionMatrix() const;

    // For PhysicsScene: the Jolt object layer for a body on game `layer`
    // (moving or not) and back, and adding a body from full Jolt settings.
    JPH::ObjectLayer ObjectLayerFor(bool moving, u8 layer = 0) const;
    static u8 GameLayerOf(JPH::ObjectLayer layer);
    JPH::BodyID CreateBody(const JPH::BodyCreationSettings& settings, bool activate);

    void Step(f32 dt);

    Vec3 GetPosition(JPH::BodyID id) const;
    Quaternion GetRotation(JPH::BodyID id) const;

    // Teleports a body to `position` (e.g. an editor drag-edit), waking it if
    // it was asleep. This bypasses the simulation's own integration for one
    // frame — fine for an editor nudging a body, not a substitute for
    // applying forces/velocity in gameplay code.
    void SetPosition(JPH::BodyID id, const Vec3& position);

    JPH::PhysicsSystem& System() { return *physics_system_; }
    JPH::BodyInterface& BodyInterface();

private:
    struct Layers; // Jolt broadphase/object layer glue; defined in the .cpp

    std::unique_ptr<JPH::TempAllocatorImpl> temp_allocator_;
    std::unique_ptr<JPH::JobSystem> jolt_job_system_;
    std::unique_ptr<Layers> layers_;
    std::unique_ptr<JPH::PhysicsSystem> physics_system_;
};

// Steps `physics` by `dt`, then writes each RigidBody entity's simulated
// position/rotation back into its Transform component. Entities need both
// components; one without the other is left untouched.
void SyncPhysicsToTransforms(World& world, PhysicsWorld& physics, f32 dt);

} // namespace aether
