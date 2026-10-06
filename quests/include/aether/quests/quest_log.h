#pragma once

#include "aether/core/base.h"
#include "aether/reflection/reflection.h"

#include <string>
#include <vector>

namespace aether::quest {

// Where one objective of a quest stands.
struct ObjectiveState {
    std::string id;
    i32 progress = 0;
    bool done = false;
};

// One quest in an entity's log. `state` is a QuestStatus value (kept as a number so it saves plainly).
struct QuestState {
    std::string name;
    i32 state = 0;
    std::vector<ObjectiveState> objectives;
};

enum QuestStatus : i32 { kInactive = 0, kActive = 1, kCompleted = 2, kFailed = 3 };

// An entity's quests (Phase 30 step 7c, §30.10): a component, saved with the entity. Quests and
// objectives are saved by name, so an edited definition still loads.
struct QuestLog {
    std::vector<QuestState> quests; // sorted by name

    QuestState* Find(const std::string& name);
    const QuestState* Find(const std::string& name) const;
};

} // namespace aether::quest

AETHER_REFLECT(aether::quest::ObjectiveState, 1, AETHER_FIELD(id, Field_EditAnywhere), AETHER_FIELD(progress, Field_EditAnywhere), AETHER_FIELD(done, Field_EditAnywhere))
AETHER_REFLECT(aether::quest::QuestState, 1,
    AETHER_FIELD(name, Field_EditAnywhere), AETHER_FIELD(state, Field_EditAnywhere, {.tooltip = "0 inactive, 1 active, 2 completed, 3 failed"}), AETHER_FIELD(objectives, Field_EditAnywhere))
AETHER_REFLECT(aether::quest::QuestLog, 1, AETHER_FIELD(quests, Field_EditAnywhere))
