#pragma once

#include "aether/core/base.h"
#include "aether/ecs/entity.h"
#include "aether/reflection/reflection.h"

#include <string>

// Blueprint function library for gameplay effects (Phase 30 step 3b, §30.3),
// acting on EffectSystem::Active(); without one everything returns 0 / false.
// `Event.OnEffectApplied` and `Event.OnEffectRemoved` (effect name, handle) are
// what an entity's Blueprint hears when a lasting effect starts or ends.

namespace aether::gas {

struct Effects {
    // The new effect's handle (> 0) for a lasting effect; 0 if it was an instant
    // effect (already done) or was rejected (see HasActiveEffect to tell).
    static i32 ApplyEffect(const Entity& target, const std::string& effect, const Entity& source);
    static bool RemoveEffect(i32 handle);
    // Removes the target's effects that grant the tag (or one under it); how many.
    static i32 RemoveEffectsByTag(const Entity& target, const std::string& tag);
    static bool HasActiveEffect(const Entity& target, const std::string& effect);
    static i32 GetActiveEffectCount(const Entity& target);
};

} // namespace aether::gas

AETHER_REFLECT(aether::gas::Effects, 1,
    AETHER_METHOD(ApplyEffect, Fn_BlueprintCallable, {"target", "effect", "source"}),
    AETHER_METHOD(RemoveEffect, Fn_BlueprintCallable, {"handle"}),
    AETHER_METHOD(RemoveEffectsByTag, Fn_BlueprintCallable, {"target", "tag"}),
    AETHER_METHOD(HasActiveEffect, Fn_BlueprintCallable | Fn_Pure, {"target", "effect"}),
    AETHER_METHOD(GetActiveEffectCount, Fn_BlueprintCallable | Fn_Pure, {"target"}))
