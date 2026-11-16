#pragma once

#include "aether/destruction/destructible.h"
#include "aether/ecs/entity.h"
#include "aether/ecs/world.h"
#include "aether/physics/physics_scene.h"

#include <vector>

namespace aether::destruction {

// How many pieces a Destructible makes: each axis clamped to 1..Destructible::kMaxPieces. Exposed so it can be tested on its own.
usize PieceCount(const Destructible& d);

// Breaks Destructibles and keeps the debris tidy. Feed it every physics event (OnEvent) and call Update once a frame: a hard hit or enough damage marks a
// Destructible, and Update breaks the marked ones, so nothing is created or destroyed while the physics events are still being delivered.
// Deterministic: breaks go in the order they were marked, pieces in grid order.
class DestructionSystem {
public:
    static constexpr usize kDefaultMaxDebris = 400;

    DestructionSystem(World& world, PhysicsScene& scene);
    ~DestructionSystem();
    DestructionSystem(const DestructionSystem&) = delete;
    DestructionSystem& operator=(const DestructionSystem&) = delete;

    static DestructionSystem* Active();
    void MakeActive();

    // A physics event: a CollisionBegin on a Destructible at or above its break_speed marks it to break (the point of contact is where it bursts from).
    void OnEvent(const PhysicsEvent& event);

    // Damage: takes `amount` off the health of a Destructible with health, and marks it to break when none is left. `point` is where the pieces burst
    // from (the entity's own position if null). Returns true if it is now marked to break. False for an entity that is not an enabled Destructible,
    // that has no health to lose, or for an amount that is not a positive number.
    bool Damage(Entity entity, f32 amount, const Vec3* point = nullptr);

    // Breaks it at the next Update regardless of health or speed. False for an entity that is not an enabled Destructible.
    bool Shatter(Entity entity, const Vec3* point = nullptr);

    // Breaks the marked Destructibles, ages the debris and removes what has expired or is over the cap. Returns the pieces made this call.
    usize Update(f32 dt);

    // Breaks one now, replacing the entity with its pieces; returns them (empty if it cannot break: not a Destructible, or no BoxCollider).
    std::vector<Entity> Break(Entity entity, const Vec3& from);

    usize DebrisCount() const;
    usize Pending() const { return pending_.size(); }
    void SetMaxDebris(usize cap) { max_debris_ = cap; }

private:
    struct Mark {
        Entity entity;
        Vec3 from;
    };
    bool MarkBreak(Entity entity, const Vec3& from);

    World& world_;
    PhysicsScene& scene_;
    std::vector<Mark> pending_;
    usize max_debris_ = kDefaultMaxDebris;
    u64 next_serial_ = 1;
};

} // namespace aether::destruction
