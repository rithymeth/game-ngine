#pragma once

#include "aether/physics/constraints.h"
#include "aether/physics/physics_world.h"

#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Constraints/TwoBodyConstraint.h>

#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace aether {

// Physics events per entity (Phase 13 step 3), delivered after each Step on
// the calling thread: each contact reaches both entities, `self` and
// `other`. `normal` points from `other` toward `self`. Solid bodies get
// CollisionBegin/Stay/End (Stay only when a collider asks for it), triggers
// get TriggerEnter/Exit. `other` may already be destroyed in an End or Exit.
enum class PhysicsEventType : u8 { CollisionBegin, CollisionStay, CollisionEnd, TriggerEnter, TriggerExit };
struct PhysicsEvent {
    Entity self, other;
    PhysicsEventType type = PhysicsEventType::CollisionBegin;
    Vec3 point{0, 0, 0};
    Vec3 normal{0, 0, 0};
    f32 approach_speed = 0.0f; // m/s along the normal, at Begin/Enter
};
// The Blueprint and script event for a type ("Event.OnTriggerEnter").
const char* PhysicsEventName(PhysicsEventType type);

// A query hit in entity terms (Phase 13 step 4). `entity` is null for a
// body that isn't one of the scene's (e.g. made directly on the PhysicsWorld).
struct SceneHit {
    bool hit = false;
    Entity entity;
    Vec3 point{0, 0, 0};
    Vec3 normal{0, 0, 0};
    f32 distance = 0.0f;
};

// Keeps a PhysicsWorld's bodies in step with the ECS (Phase 13 step 1):
// every entity with a collider has one Jolt body, shaped by its colliders
// (several make a compound) and moved as its RigidBody says (none: static).
//
//   Sync()     creates bodies for new entities, rebuilds the ones whose
//              collider or RigidBody settings changed, and destroys the ones
//              whose entity or colliders are gone;
//   Step(dt)   Syncs, pushes gameplay's Transform changes into the bodies
//              (kinematic bodies are moved there over the step; static and
//              dynamic ones are teleported), steps the simulation, then
//              writes dynamic bodies back to their Transforms.
//
// Each body's user data is its entity (EntityOf), for the contact events
// of step 3.
class PhysicsScene {
public:
    PhysicsScene(World& world, PhysicsWorld& physics);
    ~PhysicsScene(); // destroys the bodies it made

    PhysicsScene(const PhysicsScene&) = delete;
    PhysicsScene& operator=(const PhysicsScene&) = delete;

    void Sync();
    void Step(f32 dt);

    // Called for each event after the step, in a deterministic order (the
    // body pairs sorted, then body 1's entity before body 2's). An event
    // whose `self` was destroyed earlier in the same dispatch (by a handler)
    // is skipped.
    using EventHandler = std::function<void(const PhysicsEvent&)>;
    void SetEventHandler(EventHandler handler) { handler_ = std::move(handler); }
    const std::vector<PhysicsEvent>& Events() const { return events_; } // the last Step's delivered events

    // Queries (Phase 13 step 4), on the bodies as of the last Sync or Step.
    // `ignore` skips one entity's body (usually the caller's own).
    SceneHit RayCast(const Vec3& from, const Vec3& to, LayerMask layers = kAllLayers, Entity ignore = kNullEntity,
                     bool include_triggers = false) const;
    std::vector<SceneHit> RayCastAll(const Vec3& from, const Vec3& to, LayerMask layers = kAllLayers,
                                     Entity ignore = kNullEntity, bool include_triggers = false,
                                     usize max_hits = 64) const;
    SceneHit SphereCast(f32 radius, const Vec3& from, const Vec3& to, LayerMask layers = kAllLayers, Entity ignore = kNullEntity,
                        bool include_triggers = false) const;
    SceneHit CapsuleCast(f32 radius, f32 height, const Vec3& from, const Vec3& to, LayerMask layers = kAllLayers,
                         Entity ignore = kNullEntity, bool include_triggers = false) const;
    SceneHit OrientedCapsuleCast(f32 radius, f32 height, const Quaternion& rotation, const Vec3& from, const Vec3& to,
                                 LayerMask layers = kAllLayers, Entity ignore = kNullEntity,
                                 bool include_triggers = false) const;
    // Entities whose bodies touch the sphere, in BodyID order.
    std::vector<Entity> OverlapSphere(const Vec3& center, f32 radius, LayerMask layers = kAllLayers, Entity ignore = kNullEntity,
                                      bool include_triggers = false) const;
    std::vector<Entity> OverlapBox(const Vec3& center, const Vec3& half_extents, LayerMask layers = kAllLayers,
                                   Entity ignore = kNullEntity, bool include_triggers = false) const;
    std::vector<Entity> OverlapOrientedBox(const Vec3& center, const Vec3& half_extents, const Quaternion& rotation,
                                           LayerMask layers = kAllLayers, Entity ignore = kNullEntity,
                                           bool include_triggers = false) const;
    std::vector<Entity> OverlapCapsule(const Vec3& center, f32 radius, f32 height, LayerMask layers = kAllLayers,
                                       Entity ignore = kNullEntity, bool include_triggers = false) const;
    std::vector<Entity> OverlapOrientedCapsule(const Vec3& center, f32 radius, f32 height, const Quaternion& rotation,
                                               LayerMask layers = kAllLayers, Entity ignore = kNullEntity,
                                               bool include_triggers = false) const;

    // A body made elsewhere (a character's inner body) that events and
    // queries should name as `entity`. Release it before destroying it; its
    // Ends still reach the entity.
    void AdoptBody(JPH::BodyID body, Entity entity);
    void ReleaseBody(JPH::BodyID body);

    // --- Constraints (docs/design/PHYSICS_CONSTRAINTS.md) -------------------
    // PhysicsJoint components are kept in step by Sync, like bodies. Runtime
    // grabs: `holder` pulls `object`'s body at `world_point` toward a target
    // (initially that point) with a spring capped at settings.max_force;
    // several grabs on one body add up. Returns kInvalidGrab when the object
    // has no dynamic body. A grab ends with Release, ReleaseHolder, a break,
    // or its body going away.
    GrabId Grab(Entity object, Entity holder, const Vec3& world_point, const GrabSettings& settings = {});
    bool SetGrabTarget(GrabId grab, const Vec3& target);
    bool Release(GrabId grab);
    usize ReleaseHolder(Entity holder); // every grab the holder has; returns how many
    usize GrabCount(Entity object) const;
    bool IsGrabActive(GrabId grab) const;
    usize JointCount() const; // joints with a live Jolt constraint

    // Called for each break after the step (before contact events), in a
    // deterministic order: locks then joints by entity index, then grabs by id.
    using ConstraintBreakHandler = std::function<void(const ConstraintBreakEvent&)>;
    void SetConstraintBreakHandler(ConstraintBreakHandler handler) { break_handler_ = std::move(handler); }
    const std::vector<ConstraintBreakEvent>& ConstraintBreaks() const { return breaks_; } // the last Step's

    // Networking: the joints' live state plus the poses and velocities of
    // the bodies they (and grabs) move. Apply sets the PhysicsJoint fields and
    // the bodies at once; the next Step applies the joint changes.
    ConstraintSnapshot CaptureConstraintState() const;
    void ApplyConstraintState(const ConstraintSnapshot& snapshot);

    World& EcsWorld() const { return world_; }
    PhysicsWorld& Physics() const { return physics_; }
    JPH::BodyID BodyOf(Entity entity) const; // invalid if it has none
    Entity EntityOf(JPH::BodyID body) const; // null if it isn't one of ours
    usize BodyCount() const { return bodies_.size(); }
    // Why an entity's colliders couldn't make a body (a degenerate hull, an
    // empty mesh), keyed by entity index; cleared when it succeeds.
    const std::unordered_map<u32, std::string>& Problems() const { return problems_; }

    // Entity -> body user data and back.
    static u64 EntityKey(Entity e) { return (static_cast<u64>(e.generation) << 32) | e.index; }
    static Entity EntityFromKey(u64 key) { return Entity{static_cast<u32>(key), static_cast<u32>(key >> 32)}; }

private:
    struct Tracked {
        Entity entity;
        JPH::BodyID body;
        std::vector<u8> settings; // colliders and RigidBody, serialized: a change rebuilds the body
        Vec3 position;            // the Transform last pushed to or read from the body
        Quaternion rotation;
        BodyMotion motion = BodyMotion::Static;
    };

    struct JointTracked {
        Entity entity;
        JPH::Ref<JPH::TwoBodyConstraint> constraint;
        JPH::Ref<JPH::TwoBodyConstraint> lock;
        JPH::BodyID body1, body2; // body1 is the connected one (invalid: the world)
        Entity other;
        std::vector<u8> settings;
        bool enabled = true;
        f32 motor_target = 0.0f;
    };
    struct GrabTracked {
        Entity object, holder;
        JPH::BodyID body;
        JPH::Ref<JPH::SixDOFConstraint> constraint;
        Vec3 origin; // constraint-space origin (the first grab point), world space
        Vec3 target;
        Vec3 local_point; // the grabbed point in the body's space
        GrabSettings settings;
    };

    bool Build(Entity entity, Tracked& tracked, std::string& problem);
    void SyncJoints();
    bool CreateJoint(JointTracked& joint, const PhysicsJoint& component);
    void RemoveJointConstraints(JointTracked& joint);
    void RemoveLock(JointTracked& joint);
    void DetachBody(JPH::BodyID body); // removes every constraint on it, before it is destroyed
    void RemoveAllConstraints();
    void CheckBreaks(f32 dt);
    Entity FindByGuid(const EntityGuid& guid) const;
    void Destroy(Tracked& tracked);
    void Dispatch();
    Entity Owner(JPH::BodyID body) const; // ours, or destroyed since the last dispatch

    World& world_;
    PhysicsWorld& physics_;
    std::unordered_map<u64, Tracked> bodies_; // by EntityKey
    std::unordered_map<u32, u64> by_body_;    // BodyID index+sequence -> EntityKey
    std::unordered_map<u32, std::string> problems_;
    std::unordered_map<u32, u64> graveyard_; // destroyed bodies -> entity, until their End is dispatched
    EventHandler handler_;
    std::vector<PhysicsEvent> events_;
    std::map<u32, JointTracked> joints_; // by entity index: deterministic order
    std::map<GrabId, GrabTracked> grabs_;
    // Bodies released from grabs stay in snapshots until they have slept for
    // a while, so clients see where they land: EntityKey -> quiet steps.
    std::map<u64, u32> released_;
    GrabId next_grab_ = 1;
    std::unordered_map<EntityGuid, Entity> guids_; // rebuilt each Sync for joints
    ConstraintBreakHandler break_handler_;
    std::vector<ConstraintBreakEvent> breaks_;
};

} // namespace aether
