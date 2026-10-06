#pragma once

#include "aether/core/base.h"
#include "aether/gameplay/tag_query.h"

#include <map>
#include <string>
#include <vector>

namespace aether::gas {

// A gameplay effect definition (Phase 30 step 3, §30.3, `.aeffect`): a change to
// an entity's attributes, instant or lasting, with tags it needs and grants.
struct GameplayEffect {
    enum class Duration : u8 { Instant, Timed, Infinite };
    enum class Op : u8 { Add, Multiply, Override };
    enum class Stacking : u8 { None, Refresh, StackCount };

    struct Modifier {
        std::string attribute;
        Op op = Op::Add;
        f32 magnitude = 0.0f;
        bool operator==(const Modifier&) const = default;
    };

    std::string name;
    Duration duration_policy = Duration::Instant;
    f32 duration = 0.0f;             // Timed: seconds
    std::vector<Modifier> modifiers;
    Stacking stacking = Stacking::None;
    i32 max_stacks = 1;              // StackCount
    bool per_source = false;         // stacks / refreshes are kept per applying entity
    f32 period = 0.0f;               // > 0: the modifiers apply to the base every `period` s instead of continuously
    bool execute_on_apply = false;   // periodic: tick once when applied
    TagQuery require;                // the target must satisfy (empty = no requirement)
    TagQuery blocked;                // the target must not satisfy (empty = no restriction)
    std::vector<GameplayTag> granted_tags;   // on the target while active
    std::vector<GameplayTag> remove_on_tags; // removed when the target gains any of these

    // Empty string if valid, else a named error ("effect.name_empty", ...).
    std::string Validate() const;
    bool operator==(const GameplayEffect&) const = default;
};

reflect::Json EffectToJson(const GameplayEffect& effect);
// False (with `error`, "effect.<name>: message") for bad JSON or an invalid effect.
bool EffectFromJson(const std::string& text, GameplayEffect& out, std::string* error = nullptr);

// Effects by name: filled from cooked `.aeffect` assets by the player, or by code.
class EffectLibrary {
public:
    // False for an invalid effect (see Validate); replaces one of the same name.
    bool Register(GameplayEffect effect, std::string* error = nullptr);
    const GameplayEffect* Find(const std::string& name) const;
    void Clear() { effects_.clear(); }
    usize Size() const { return effects_.size(); }

private:
    std::map<std::string, GameplayEffect> effects_;
};

} // namespace aether::gas
