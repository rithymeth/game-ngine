#pragma once

#include "aether/ecs/world.h"
#include "aether/gameplay/effect_system.h"
#include "aether/gameplay/gameplay_ability.h"

#include <map>
#include <string>
#include <vector>

namespace aether::gas {

using AbilityHandle = u32; // 0 is no ability

// One running ability on an owner (§30.4).
struct ActiveAbility {
    AbilityHandle handle = 0;
    std::string ability;
    f32 elapsed = 0.0f;
    bool committed = false;
    u32 seq = 0;
};

// The abilities an entity has and is running: a component, saved with it.
struct AbilityContainer {
    std::vector<std::string> granted; // sorted
    std::vector<ActiveAbility> active;
};

enum class FailReason : u8 {
    None, UnknownAbility, NotGranted, DeadOwner, AlreadyActive, MissingRequired, Blocked, BlockedByAbility, Cooldown, CannotAfford
};

// Something happened to an ability (§30.4). `reason` is set for Failed;
// `timed_out` for an Ended that hit its max_duration.
struct AbilityEvent {
    enum class Kind : u8 { Activated, Ended, Cancelled, Failed };
    Entity entity;
    std::string ability;
    AbilityHandle handle = 0;
    Kind kind = Kind::Activated;
    FailReason reason = FailReason::None;
    bool timed_out = false;
};

// Runs entities' abilities. Activation checks tags (required, blocked, cooldown,
// other abilities' blocks) and affordability (a dry run of the cost), cancels the
// abilities it says it cancels, adds the tags it owns while active, and commits
// (applies the cost and cooldown effects through the EffectSystem), at once or
// later. End and Cancel release exactly what activation added. Deterministic.
class AbilitySystem {
public:
    static constexpr const char* kActivatedEvent = "Event.OnAbilityActivated";
    static constexpr const char* kEndedEvent = "Event.OnAbilityEnded";

    struct ActivateResult {
        AbilityHandle handle = 0;
        FailReason reason = FailReason::None; // None when handle != 0
    };

    AbilitySystem(World& world, AttributeSystem& attributes, EffectSystem& effects, const AbilityLibrary& abilities, const EffectLibrary& effect_library);
    ~AbilitySystem();
    AbilitySystem(const AbilitySystem&) = delete;
    AbilitySystem& operator=(const AbilitySystem&) = delete;

    static AbilitySystem* Active();
    void MakeActive();

    // False for a dead owner or an unknown ability. Granting twice is fine.
    bool Grant(Entity owner, const std::string& ability);
    // Takes the ability away, cancelling it if it runs. False if it wasn't granted.
    bool Revoke(Entity owner, const std::string& ability);
    bool IsGranted(Entity owner, const std::string& ability) const;

    FailReason CanActivate(Entity owner, const std::string& ability) const;
    ActivateResult TryActivate(Entity owner, const std::string& ability);
    // Applies the cost and cooldown (once; later calls return true and do nothing).
    // False (and a Failed event) if it can no longer be afforded.
    bool Commit(AbilityHandle handle);
    bool End(AbilityHandle handle);    // finished normally
    bool Cancel(AbilityHandle handle); // interrupted
    // Cancels the owner's active abilities with a tag matching `tag` (hierarchically); how many.
    i32 CancelByTag(Entity owner, const GameplayTag& tag);

    void Update(f32 dt);
    // Re-derives the handle index after a world was loaded.
    void Rebuild();

    bool IsActive(Entity owner, const std::string& ability) const;
    i32 ActiveCount(Entity owner) const;

    const std::vector<AbilityEvent>& Events() const { return events_; }
    void ClearEvents() { events_.clear(); }

private:
    AbilityContainer* ContainerOf(Entity entity) const;
    std::vector<Entity> Targets() const;
    bool Affordable(Entity owner, const GameplayAbility& def) const;
    bool Finish(AbilityHandle handle, bool cancelled, bool timed_out);
    void Fail(Entity owner, const std::string& ability, FailReason reason);

    World& world_;
    AttributeSystem& attributes_;
    EffectSystem& effects_;
    const AbilityLibrary& abilities_;
    const EffectLibrary& effect_library_;
    std::map<AbilityHandle, Entity> handles_;
    std::vector<AbilityEvent> events_;
    AbilityHandle next_handle_ = 1;
    u32 next_seq_ = 1;
};

} // namespace aether::gas

AETHER_REFLECT(aether::gas::ActiveAbility, 1,
    AETHER_FIELD(handle, Field_EditAnywhere), AETHER_FIELD(ability, Field_EditAnywhere), AETHER_FIELD(elapsed, Field_EditAnywhere),
    AETHER_FIELD(committed, Field_EditAnywhere), AETHER_FIELD(seq, Field_EditAnywhere))
AETHER_REFLECT(aether::gas::AbilityContainer, 1,
    AETHER_FIELD(granted, Field_EditAnywhere, {.tooltip = "The abilities this entity can use"}),
    AETHER_FIELD(active, Field_EditAnywhere, {.tooltip = "The abilities running now"}))
