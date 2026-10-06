#pragma once

#include "aether/ecs/world.h"
#include "aether/gameplay/attribute_system.h"
#include "aether/gameplay/gameplay_effect.h"

#include <map>
#include <string>
#include <vector>

namespace aether::gas {

using EffectHandle = u32; // 0 is no effect

// One lasting effect on a target (§30.3). `source` is only a runtime handle (it
// is not a saved reference), used to tell stacks apart when a definition is per_source.
struct ActiveEffect {
    EffectHandle handle = 0;
    std::string effect;
    Entity source;
    f32 remaining = 0.0f; // Timed: seconds left
    i32 stacks = 1;
    f32 period_timer = 0.0f;
    u32 seq = 0;          // application order; the latest Override wins
};

// The lasting effects on an entity: a component, so they are saved with it.
struct EffectContainer {
    std::vector<ActiveEffect> active;
};

// An effect was applied to / removed from an entity (§30.3).
struct EffectEvent {
    Entity entity;
    std::string effect;
    EffectHandle handle = 0;
    bool applied = true;
};

// Applies effects to entities' attributes and tags, and ticks them. Instant
// effects change the base (through the AttributeSystem, so events fire).
// Timed / infinite effects keep modifiers on `current` while active:
//   current = clamp( latest Override, or (base + sum(Add)) * product(Multiply) )
// unless periodic, which change the base every period instead. Updates are
// deterministic: entities, then effects, in handle order. The Blueprint library
// acts on the active system.
class EffectSystem {
public:
    static constexpr const char* kAppliedEvent = "Event.OnEffectApplied";
    static constexpr const char* kRemovedEvent = "Event.OnEffectRemoved";

    enum class Status : u8 { Rejected, Applied, Instant };
    struct ApplyResult {
        Status status = Status::Rejected;
        EffectHandle handle = 0; // 0 for Rejected and Instant
    };

    EffectSystem(World& world, AttributeSystem& attributes, const EffectLibrary& library);
    ~EffectSystem();
    EffectSystem(const EffectSystem&) = delete;
    EffectSystem& operator=(const EffectSystem&) = delete;

    static EffectSystem* Active();
    void MakeActive();

    // Rejected: dead target, unknown effect, a modifier on an attribute the
    // target lacks, unmet require / blocked tags, or None-stacking already applied.
    ApplyResult Apply(Entity target, const std::string& effect, Entity source = kNullEntity);
    bool Remove(EffectHandle handle);
    // Removes the target's effects that grant `tag` (or a tag under it); how many.
    i32 RemoveByTag(Entity target, const GameplayTag& tag);

    // Steps time by `dt` seconds.
    void Update(f32 dt);
    // Re-derives modifiers, the handle index and tags' consistency after a world was loaded.
    void Rebuild();

    bool HasEffect(Entity target, const std::string& effect) const;
    i32 ActiveCount(Entity target) const;

    const std::vector<EffectEvent>& Events() const { return events_; }
    void ClearEvents() { events_.clear(); }

private:
    EffectContainer* ContainerOf(Entity entity) const;
    std::vector<Entity> Targets() const;
    void Recompute(Entity target);
    void ApplyInstant(Entity target, const GameplayEffect& effect, i32 stacks);
    void RemoveAt(Entity target, usize index);

    World& world_;
    AttributeSystem& attributes_;
    const EffectLibrary& library_;
    std::map<EffectHandle, Entity> handles_;
    std::vector<EffectEvent> events_;
    EffectHandle next_handle_ = 1;
    u32 next_seq_ = 1;
};

} // namespace aether::gas

AETHER_REFLECT(aether::gas::ActiveEffect, 1,
    AETHER_FIELD(handle, Field_EditAnywhere), AETHER_FIELD(effect, Field_EditAnywhere), AETHER_FIELD(source, Field_EditAnywhere),
    AETHER_FIELD(remaining, Field_EditAnywhere), AETHER_FIELD(stacks, Field_EditAnywhere), AETHER_FIELD(period_timer, Field_EditAnywhere),
    AETHER_FIELD(seq, Field_EditAnywhere))
AETHER_REFLECT(aether::gas::EffectContainer, 1, AETHER_FIELD(active, Field_EditAnywhere, {.tooltip = "The lasting effects on this entity"}))
