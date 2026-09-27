#pragma once

#include "aether/physics/physics_world.h"

#include <functional>
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

    bool Build(Entity entity, Tracked& tracked, std::string& problem);
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
};

} // namespace aether
