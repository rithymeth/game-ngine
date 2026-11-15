#pragma once

#include "aether/core/base.h"
#include "aether/reflection/serialize.h"

#include <map>
#include <string>
#include <vector>

namespace aether::ach {

// An achievement definition (`.aachievement`): a goal with a count to reach. A one-off goal has a target of 1; "kill 50 sentinels" has 50.
struct AchievementDef {
    static constexpr i32 kMaxTarget = 1000000;

    std::string name;
    std::string title;       // shown text (empty: the name)
    std::string description; // what it asks for
    i32 target = 1;          // 1 to kMaxTarget
    bool hidden = false;     // a game may keep it out of the list until it is unlocked

    // Empty string if valid, else a named error ("achievement.name_empty", ...).
    std::string Validate() const;
    bool operator==(const AchievementDef&) const = default;
};

reflect::Json AchievementToJson(const AchievementDef& achievement);
// False (with `error`, "achievement.<code>: message") for bad JSON or an invalid achievement.
bool AchievementFromJson(const std::string& text, AchievementDef& out, std::string* error = nullptr);

// Achievements by name: filled from cooked `.aachievement` assets by the player, or by code.
class AchievementLibrary {
public:
    bool Register(AchievementDef achievement, std::string* error = nullptr);
    const AchievementDef* Find(const std::string& name) const;
    void Clear() { achievements_.clear(); }
    usize Size() const { return achievements_.size(); }
    std::vector<std::string> Names() const; // sorted

private:
    std::map<std::string, AchievementDef> achievements_;
};

} // namespace aether::ach
