#pragma once

#include "aether/core/base.h"
#include "aether/ecs/entity.h"
#include "aether/math/vec.h"
#include "aether/reflection/reflection.h"

#include <string>

// Blueprint function library for interaction (Phase 30 step 7b, §30.8), acting on
// InteractionSystem::Active(); without one everything returns null / false / "".
// A target's Blueprint hears `Event.OnInteract` (interactor); a user's hears
// `Event.OnInteractFailed` (target, reason), the reason a name such as "out_of_range".

namespace aether::interact {

struct Interaction {
    // What the interactor can use from `from`, looking along `forward` (a zero vector looks all round); null if nothing.
    static Entity FindInteractable(const Entity& interactor, const Vec3& from, const Vec3& forward);
    static std::string GetInteractionPrompt(const Entity& target);
    // The target's prompt as a user at the entity's own position would be shown it (a name, "" when it can't be used now).
    static std::string GetPromptFor(const Entity& interactor, const Entity& target);
    static bool CanInteract(const Entity& interactor, const Entity& target);
    // True if it was used.
    static bool Interact(const Entity& interactor, const Entity& target);
    static bool SetInteractable(const Entity& target, bool enabled);
    static bool ResetInteractable(const Entity& target);
};

} // namespace aether::interact

AETHER_REFLECT(aether::interact::Interaction, 1,
    AETHER_METHOD(FindInteractable, Fn_BlueprintCallable | Fn_Pure, {"interactor", "from", "forward"}),
    AETHER_METHOD(GetInteractionPrompt, Fn_BlueprintCallable | Fn_Pure, {"target"}),
    AETHER_METHOD(GetPromptFor, Fn_BlueprintCallable | Fn_Pure, {"interactor", "target"}),
    AETHER_METHOD(CanInteract, Fn_BlueprintCallable | Fn_Pure, {"interactor", "target"}),
    AETHER_METHOD(Interact, Fn_BlueprintCallable, {"interactor", "target"}),
    AETHER_METHOD(SetInteractable, Fn_BlueprintCallable, {"target", "enabled"}),
    AETHER_METHOD(ResetInteractable, Fn_BlueprintCallable, {"target"}))
