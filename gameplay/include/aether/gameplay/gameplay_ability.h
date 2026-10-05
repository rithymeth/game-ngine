#pragma once

#include "aether/core/base.h"
#include "aether/gameplay/gameplay_effect.h"
#include "aether/gameplay/tag_query.h"

#include <map>
#include <string>
#include <vector>

namespace aether::gas {

// A gameplay ability definition (Phase 30 step 4, §30.4, `.aability`): something
// an entity can do (a jump, a fireball, a dodge), gated by tags, paid for with
// an effect and put on cooldown by another.
struct GameplayAbility {
    std::string name;
    std::vector<GameplayTag> tags;           // what this ability is, for others' cancel / block queries
    TagQuery activation_required;            // the owner must satisfy (empty = none)
    TagQuery activation_blocked;             // the owner must not satisfy (empty = none)
    TagQuery cancel_abilities_with_tags;     // activating cancels the owner's active abilities whose tags match
    TagQuery block_abilities_with_tags;      // while active, abilities whose tags match can't activate
    std::vector<GameplayTag> activation_owned_tags; // on the owner while active
    std::string cost;                        // an instant effect, applied on commit (optional)
    std::string cooldown;                    // a timed effect granting the cooldown tag (optional)
    f32 max_duration = 0.0f;                 // > 0: ends by itself after this many seconds
    bool commit_on_activate = true;          // else the ability commits when it says so

    // Empty string if valid, else a named error ("ability.name_empty", ...).
    std::string Validate() const;
    bool operator==(const GameplayAbility&) const = default;
};

reflect::Json AbilityToJson(const GameplayAbility& ability);
// False (with `error`, "ability.<code>: message") for bad JSON or an invalid ability.
bool AbilityFromJson(const std::string& text, GameplayAbility& out, std::string* error = nullptr);

// Abilities by name: filled from cooked `.aability` assets by the player, or by code.
class AbilityLibrary {
public:
    // False for an invalid ability (see Validate); replaces one of the same name.
    bool Register(GameplayAbility ability, std::string* error = nullptr);
    const GameplayAbility* Find(const std::string& name) const;
    void Clear() { abilities_.clear(); }
    usize Size() const { return abilities_.size(); }

    // Checks the effects an ability names: "ability.unknown_effect", "ability.cost_not_instant"
    // (the cost is not an instant effect), "ability.cooldown_not_timed" (the cooldown is not a
    // timed effect that grants a tag). Empty string if fine.
    static std::string CheckEffects(const GameplayAbility& ability, const EffectLibrary& effects);

private:
    std::map<std::string, GameplayAbility> abilities_;
};

} // namespace aether::gas
