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

#include <map>
#include <memory>
#include <vector>

namespace aether {

// Owns a Jolt PhysicsSystem plus the allocator/layer-filter boilerplate Jolt
// requires, wrapped behind aether::Vec3/Quaternion so callers don't need to
// touch JPH:: types directly for the common case (create a body, step, read
// its transform back). Simulation jobs run on the aether::JobSystem passed
// in — via JoltJobSystemAdapter — rather than a Jolt-owned thread pool, so
// physics shares the same worker threads as the rest of the engine. Step()
// (and anything else that reaches JPH::PhysicsSystem::Update) must be called
// from a thread registered with that JobSystem; see JoltJobSystemAdapter.
// A contact between two bodies, reported after a Step (Phase 13 step 3).
// Body 1 has the lower BodyID. Begin and End come once per body pair (a
// compound's several touching parts count once); Stay comes every step
// while touching, only for bodies with SetReportStay. `trigger` is set when
// either body is a sensor (triggers never Stay). `normal` points from body 1
// to body 2 (the way to push body 2 out) and `approach_speed` is how fast
// they were closing along it at Begin, in m/s (0 for End).
enum class ContactType : u8 { Begin, Stay, End };
struct ContactEvent {
    JPH::BodyID body1, body2;
    ContactType type = ContactType::Begin;
    bool trigger = false;
    Vec3 point{0, 0, 0};
    Vec3 normal{0, 0, 0};
    f32 approach_speed = 0.0f;
};

// Scene queries (Phase 13 step 4). `layers` is a mask of game layers to
// hit; `ignore` lists bodies to skip. Triggers (sensors) are skipped unless
// `include_triggers` is set.
struct QueryFilter {
    LayerMask layers = kAllLayers;
    std::vector<JPH::BodyID> ignore;
    bool include_triggers = false;
};
struct QueryHit {
    bool hit = false;
    JPH::BodyID body;
    Vec3 point{0, 0, 0};  // on the surface hit
    Vec3 normal{0, 0, 0}; // the surface's, pointing back toward the query
    f32 distance = 0.0f;  // from the start, along the path
};

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

    // --- Queries (Phase 13 step 4) --------------------------------------------
    // Safe at any time outside Step. The nearest hit along `from` -> `to`.
    QueryHit RayCast(const Vec3& from, const Vec3& to, const QueryFilter& filter = {}) const;
    // Sweeps `shape` (at `rotation`) from `from` to `to`; the first hit.
    // Starting inside something counts as a hit at distance 0.
    QueryHit ShapeCast(const JPH::Shape& shape, const Vec3& from, const Vec3& to, const Quaternion& rotation = Quaternion::Identity(),
                       const QueryFilter& filter = {}) const;
    QueryHit SphereCast(f32 radius, const Vec3& from, const Vec3& to, const QueryFilter& filter = {}) const;
    // Every body touching `shape` placed at `position` / `rotation`, sorted by BodyID.
    std::vector<JPH::BodyID> Overlap(const JPH::Shape& shape, const Vec3& position, const Quaternion& rotation = Quaternion::Identity(),
                                     const QueryFilter& filter = {}) const;
    std::vector<JPH::BodyID> OverlapSphere(const Vec3& center, f32 radius, const QueryFilter& filter = {}) const;

    // --- Contacts (Phase 13 step 3) ---------------------------------------
    // Jolt reports contacts from worker threads during Step. They go into a
    // fixed-capacity lock-free buffer (no allocation; overflow is counted
    // and logged), and after Step they're sorted into a deterministic order
    // and turned into Begin/Stay/End per body pair here, on the calling
    // thread. Sleeping bodies keep their contacts (no End while a pile
    // rests); a destroyed body's contacts End at the next Step.
    const std::vector<ContactEvent>& Contacts() const { return contacts_; }
    void SetReportStay(JPH::BodyID body, bool report);
    u64 DroppedContacts() const; // total contact callbacks that didn't fit the buffer

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

    struct ContactQueue; // the Jolt listener and its buffer; defined in the .cpp
    struct PairState {
        u32 touching = 0; // sub-shape contacts currently reported
        bool trigger = false;
        bool dormant = false; // both bodies asleep: still in contact, no events
        u64 stay_step = 0;
    };
    void ProcessContacts();
    std::unique_ptr<ContactQueue> contact_queue_;
    std::map<u64, PairState> pairs_; // by (body1, body2) ids; ordered, for deterministic Ends
    std::vector<ContactEvent> contacts_;
    u64 step_ = 0;
};

// Steps `physics` by `dt`, then writes each RigidBody entity's simulated
// position/rotation back into its Transform component. Entities need both
// components; one without the other is left untouched.
void SyncPhysicsToTransforms(World& world, PhysicsWorld& physics, f32 dt);

} // namespace aether
