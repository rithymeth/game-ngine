#pragma once

#include "aether/core/base.h"
#include "aether/gameplay/gameplay_effect.h"

#include <map>
#include <string>
#include <vector>

namespace aether::quest {

// A quest definition (Phase 30 step 7c, §30.10, `.aquest`): what to do, what must come first,
// and what it pays.
struct Objective {
    enum class Kind : u8 { Count, Tag, Flag };
    std::string id;       // unique in the quest
    std::string text;     // shown text (when there is no key or translation)
    std::string text_key; // localization key
    Kind kind = Kind::Count;
    std::string target;   // what is counted: an item name, a tag, a free key; Notify matches kind and target
    i32 required = 1;
    bool optional = false;
    bool operator==(const Objective&) const = default;
};

struct RewardItem {
    std::string item;
    i32 count = 1;
    bool operator==(const RewardItem&) const = default;
};

struct QuestDef {
    std::string name;
    std::string title, title_key;
    std::string description, description_key;
    std::vector<Objective> objectives;
    std::vector<std::string> prerequisites; // quests that must be completed first
    std::vector<std::string> reward_effects; // applied to the player on completion
    std::vector<RewardItem> reward_items;    // handed to the host in the Completed event (the player gives them to the inventory kit)

    // Empty string if valid, else a named error ("quest.no_name", ...).
    std::string Validate() const;
    bool operator==(const QuestDef&) const = default;
};

reflect::Json QuestToJson(const QuestDef& quest);
// False (with `error`, "quest.<code>: message") for bad JSON or an invalid quest.
bool QuestFromJson(const std::string& text, QuestDef& out, std::string* error = nullptr);

// Quests by name: filled from cooked `.aquest` assets by the player, or by code.
class QuestLibrary {
public:
    bool Register(QuestDef quest, std::string* error = nullptr);
    const QuestDef* Find(const std::string& name) const;
    void Clear() { quests_.clear(); }
    usize Size() const { return quests_.size(); }

    // What is wrong across the library once everything is loaded: a prerequisite that isn't a quest
    // ("quest.unknown_prereq"), a cycle of prerequisites ("quest.prereq_cycle"), a reward effect that
    // isn't an effect ("quest.unknown_effect"; skipped if `effects` is null). One message per problem.
    std::vector<std::string> Check(const gas::EffectLibrary* effects) const;

private:
    std::map<std::string, QuestDef> quests_;
};

} // namespace aether::quest
