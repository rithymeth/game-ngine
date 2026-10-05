#pragma once

#include "aether/ecs/world.h"
#include "aether/gameplay/ability_system.h"
#include "aether/gameplay/effect_system.h"
#include "aether/interaction/interactable.h"
#include "aether/math/vec.h"

#include <string>
#include <vector>

namespace aether::interact {

// Registers the interaction component with the ECS (idempotent).
void RegisterInteractionComponents();

enum class Reason : u8 { None, DeadEntity, NotInteractable, Disabled, OutOfRange, MissingTag, BlockedTag, Cooldown, Used, ActionFailed };
// "out_of_range", "missing_tag", ...: what a Blueprint's Event.OnInteractFailed hears.
const char* ReasonName(Reason reason);

// Something was used, or couldn't be (§30.8).
struct InteractionEvent {
    enum class Kind : u8 { Interacted, Failed };
    Entity target;     // the thing used
    Entity interactor; // who used it
    Kind kind = Kind::Interacted;
    Reason reason = Reason::None; // for Failed
};

// What the interactor can use now.
struct FocusResult {
    Entity target;      // null if nothing
    f32 distance = 0.0f;
    std::string prompt; // the localized prompt
};

// Finds what an entity can use and lets it use it. A target is usable when it is enabled, in
// range, not on cooldown or spent (one shot), and the user has its required tags and none of
// its blocked ones. Using it applies its effect and activates its ability on the user (through
// the gameplay systems, when given), starts the cooldown, and queues an event the host hands to
// the target's script and Blueprint (OnInteract (interactor)) or the user's (OnInteractFailed
// (target, reason)). Positions are Transform positions (world space for entities without a parent).
// Deterministic. The Blueprint library acts on the active system.
class InteractionSystem {
public:
    static constexpr const char* kInteractEvent = "Event.OnInteract";
    static constexpr const char* kFailedEvent = "Event.OnInteractFailed";

    // The systems may be null: the effect and ability of a target are then skipped.
    InteractionSystem(World& world, gas::EffectSystem* effects, gas::AbilitySystem* abilities);
    ~InteractionSystem();
    InteractionSystem(const InteractionSystem&) = delete;
    InteractionSystem& operator=(const InteractionSystem&) = delete;

    static InteractionSystem* Active();
    void MakeActive();

    // Whether `interactor`, standing at `from`, can use `target` now.
    Reason Check(Entity interactor, Entity target, const Vec3& from) const;
    // The same from the interactor's own position.
    Reason Check(Entity interactor, Entity target) const;
    // The nearest usable target within its range, in front of `forward` (a zero `forward` looks all round).
    FocusResult Focus(Entity interactor, const Vec3& from, const Vec3& forward) const;
    // Uses `target`; the reason it couldn't, or None. Queues an event either way.
    Reason Interact(Entity interactor, Entity target);
    std::string Prompt(Entity target) const;
    bool SetEnabled(Entity target, bool enabled);
    // Makes a spent one-shot, or a cooling one, usable again.
    bool Reset(Entity target);

    // Counts cooldowns down.
    void Update(f32 dt);

    const std::vector<InteractionEvent>& Events() const { return events_; }
    void ClearEvents() { events_.clear(); }

private:
    std::vector<Entity> Targets() const;
    Reason CheckAt(Entity interactor, Entity target, const Vec3* from) const;

    World& world_;
    gas::EffectSystem* effects_;
    gas::AbilitySystem* abilities_;
    std::vector<InteractionEvent> events_;
};

} // namespace aether::interact
