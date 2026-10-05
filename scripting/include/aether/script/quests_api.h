#pragma once

#include "aether/script/luau_host.h"

// Luau's `Quests` table (Phase 30 step 7c, docs/design/PHASE_SPECS.md §30.10): the same functions
// as the `Quests` Blueprint library, on the active QuestSystem. Present only when the quests kit is built.
//
//   Quests.Start / Abandon / Fail (owner, quest)                 -> boolean (true if it worked)
//   Quests.Progress (owner, quest, objective, amount)            -> boolean
//   Quests.Notify (owner, kind, target, amount)                  -> number of objectives that moved
//        kind: "count", "tag" or "flag"; what the game reports (an enemy killed, a place reached)
//   Quests.IsActive / IsCompleted (owner, quest)                 -> boolean
//   Quests.GetState (owner, quest)                               -> "inactive", "active", "completed" or "failed"
//   Quests.GetProgress (owner, quest, objective)                 -> number
//   Quests.GetTitle (quest)                                      -> string (translated when the quest has a key)
//
// An entity's script hears OnQuestStarted (quest), OnQuestProgress (quest, objective, progress, required),
// OnQuestObjectiveCompleted (quest, objective), OnQuestCompleted (quest), OnQuestFailed (quest) and
// OnQuestAbandoned (quest). A call with a missing or wrong-typed argument raises a script error;
// without an active system the functions return false / 0 / "".

namespace aether::script {

void InstallQuestsApi(LuauHost& host); // does nothing when the kit isn't built

} // namespace aether::script
