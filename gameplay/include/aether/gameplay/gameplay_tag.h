#pragma once

#include "aether/core/base.h"
#include "aether/reflection/reflection.h"

#include <string>
#include <string_view>

namespace aether::gas {

// A hierarchical name like "State.Stunned" or "Damage.Fire.Burning" (Phase 30,
// §30.1). Segments are separated by '.', each non-empty and made of letters,
// digits and '_'. A tag *matches* itself and every tag below it, so a check for
// "Damage.Fire" is satisfied by "Damage.Fire.Burning" (but "Damage.Fir" or
// "Damage.Fire2" are not under it). An invalid name is the empty tag, which
// matches nothing.
struct GameplayTag {
    std::string name;

    // Whether `name` is a valid tag name.
    static bool ValidName(std::string_view name);
    // The tag for `name`, or the empty tag if it isn't valid.
    static GameplayTag Make(std::string_view name) { return ValidName(name) ? GameplayTag{std::string(name)} : GameplayTag{}; }

    bool IsValid() const { return !name.empty(); }
    // "State.Stunned.Frozen" -> "State.Stunned"; a top-level tag has no parent (the empty tag).
    GameplayTag Parent() const;
    // "State" is 1, "State.Stunned" is 2; the empty tag is 0.
    i32 Depth() const;
    // True if this tag is `other` or lies under it (hierarchical).
    bool Matches(const GameplayTag& other) const;
    bool MatchesExact(const GameplayTag& other) const { return IsValid() && name == other.name; }

    bool operator==(const GameplayTag& other) const = default;
    bool operator<(const GameplayTag& other) const { return name < other.name; }
};

} // namespace aether::gas

AETHER_REFLECT(aether::gas::GameplayTag, 1, AETHER_FIELD(name, Field_EditAnywhere, {.tooltip = "A dotted name such as State.Stunned"}))
