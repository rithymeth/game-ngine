#pragma once

#include "aether/core/base.h"
#include "aether/reflection/reflection.h"

#include <string>

// Blueprint function library for gameplay tags (Phase 30 step 1, §30.1): the
// pure functions on tag names. (Tags on an entity come with the attribute and
// ability systems.) Tags are plain strings in Blueprints; an invalid name matches nothing.

namespace aether::gas {

struct GameplayTags {
    static bool IsValid(const std::string& tag);
    // True if `tag` is `parent` or under it ("Damage.Fire.Burning" matches "Damage.Fire").
    static bool Matches(const std::string& tag, const std::string& parent);
    static bool MatchesExact(const std::string& tag, const std::string& other);
    // "State.Stunned.Frozen" -> "State.Stunned"; "" for a top-level or invalid tag.
    static std::string GetParent(const std::string& tag);
    static i32 GetDepth(const std::string& tag);
};

} // namespace aether::gas

AETHER_REFLECT(aether::gas::GameplayTags, 1,
    AETHER_METHOD(IsValid, Fn_BlueprintCallable | Fn_Pure, {"tag"}),
    AETHER_METHOD(Matches, Fn_BlueprintCallable | Fn_Pure, {"tag", "parent"}),
    AETHER_METHOD(MatchesExact, Fn_BlueprintCallable | Fn_Pure, {"tag", "other"}),
    AETHER_METHOD(GetParent, Fn_BlueprintCallable | Fn_Pure, {"tag"}),
    AETHER_METHOD(GetDepth, Fn_BlueprintCallable | Fn_Pure, {"tag"})
)
