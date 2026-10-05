#pragma once

#include "aether/core/base.h"
#include "aether/ecs/entity.h"
#include "aether/reflection/reflection.h"

#include <string>

// Blueprint function library for gameplay abilities (Phase 30 step 4b, §30.4),
// acting on AbilitySystem::Active(); without one everything returns 0 / false.
// An ability's logic lives in the owner's Blueprint: it hears
// `Event.OnAbilityActivated` (ability, handle), does its work (latent nodes
// included) and calls EndAbility. `Event.OnAbilityEnded` (ability, handle,
// cancelled) and `Event.OnAbilityFailed` (ability, reason) report the rest.

namespace aether::gas {

struct Abilities {
    static bool GrantAbility(const Entity& owner, const std::string& ability);
    static bool RevokeAbility(const Entity& owner, const std::string& ability);
    // The running ability's handle (> 0), or 0 if it couldn't activate (CanActivateAbility says why not yet).
    static i32 TryActivateAbility(const Entity& owner, const std::string& ability);
    static bool CanActivateAbility(const Entity& owner, const std::string& ability);
    // Applies the cost and cooldown of an ability activated without commit_on_activate.
    static bool CommitAbility(i32 handle);
    static bool EndAbility(i32 handle);
    static bool CancelAbility(i32 handle);
    static bool IsAbilityActive(const Entity& owner, const std::string& ability);
    static bool IsAbilityGranted(const Entity& owner, const std::string& ability);
};

} // namespace aether::gas

AETHER_REFLECT(aether::gas::Abilities, 1,
    AETHER_METHOD(GrantAbility, Fn_BlueprintCallable, {"owner", "ability"}),
    AETHER_METHOD(RevokeAbility, Fn_BlueprintCallable, {"owner", "ability"}),
    AETHER_METHOD(TryActivateAbility, Fn_BlueprintCallable, {"owner", "ability"}),
    AETHER_METHOD(CanActivateAbility, Fn_BlueprintCallable | Fn_Pure, {"owner", "ability"}),
    AETHER_METHOD(CommitAbility, Fn_BlueprintCallable, {"handle"}),
    AETHER_METHOD(EndAbility, Fn_BlueprintCallable, {"handle"}),
    AETHER_METHOD(CancelAbility, Fn_BlueprintCallable, {"handle"}),
    AETHER_METHOD(IsAbilityActive, Fn_BlueprintCallable | Fn_Pure, {"owner", "ability"}),
    AETHER_METHOD(IsAbilityGranted, Fn_BlueprintCallable | Fn_Pure, {"owner", "ability"}))
