#pragma once

#include "aether/physics/physics_world.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace aether {

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

    World& world_;
    PhysicsWorld& physics_;
    std::unordered_map<u64, Tracked> bodies_; // by EntityKey
    std::unordered_map<u32, u64> by_body_;    // BodyID index+sequence -> EntityKey
    std::unordered_map<u32, std::string> problems_;
};

} // namespace aether
