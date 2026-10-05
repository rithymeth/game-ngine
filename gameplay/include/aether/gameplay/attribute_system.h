#pragma once

#include "aether/ecs/world.h"
#include "aether/gameplay/attribute_set.h"

#include <string>
#include <vector>

namespace aether::gas {

// Something's current value changed (§30.2).
struct AttributeEvent {
    Entity entity;
    std::string name;
    f32 old_value = 0.0f;
    f32 new_value = 0.0f;
};

// Registers the gameplay components (AttributeSet, TagContainer) with the ECS (idempotent).
void RegisterGameplayComponents();

// The attributes of the entities in a world, and the changes made to them. All
// changes go through here so the game hears about them: an AttributeEvent is
// queued when an attribute's *current* value actually changes (once per
// change, never for a write that leaves it as it was), and the host drains
// them (the player sends each as Event.OnAttributeChanged to that entity's
// Blueprint). The Blueprint library acts on the active system, like the
// sequencer's.
class AttributeSystem {
public:
    static constexpr const char* kChangedEvent = "Event.OnAttributeChanged";

    explicit AttributeSystem(World& world);
    ~AttributeSystem();
    AttributeSystem(const AttributeSystem&) = delete;
    AttributeSystem& operator=(const AttributeSystem&) = delete;

    static AttributeSystem* Active();
    void MakeActive();

    // Current value; `fallback` for an entity without the attribute (or a dead one).
    f32 Get(Entity entity, const std::string& name, f32 fallback = 0.0f) const;
    bool Has(Entity entity, const std::string& name) const;
    f32 GetBase(Entity entity, const std::string& name, f32 fallback = 0.0f) const;
    f32 GetMin(Entity entity, const std::string& name, f32 fallback = 0.0f) const;
    f32 GetMax(Entity entity, const std::string& name, f32 fallback = 0.0f) const;

    // Creates the AttributeSet on the entity if it has none. False on a dead
    // entity or a bad name / NaN (nothing changes).
    bool Define(Entity entity, const std::string& name, f32 base, f32 min = -3.0e38f, f32 max = 3.0e38f);
    bool SetBase(Entity entity, const std::string& name, f32 base);
    bool AddBase(Entity entity, const std::string& name, f32 delta);

    const std::vector<AttributeEvent>& Events() const { return events_; }
    void ClearEvents() { events_.clear(); }

private:
    AttributeSet* SetOf(Entity entity) const;
    // Runs `change` on the attribute and queues an event if its current value moved.
    template <typename Fn>
    bool Mutate(Entity entity, const std::string& name, Fn&& change);

    World& world_;
    std::vector<AttributeEvent> events_;
};

} // namespace aether::gas
