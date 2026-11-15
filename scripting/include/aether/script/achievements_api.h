#pragma once

#include "aether/script/luau_host.h"

// Luau's `Achievements` table: goals (`.aachievement`) with progress kept in the game's save bag, so it is saved and loaded with the slot and a
// scene change does not touch it. Present only when the achievements kit is built.
//
//   Achievements.Add (name [, amount])  -> "ok", or why not: "unknown_achievement", "bad_amount" (1 to 1000000; default 1). Reaching the target unlocks it
//   Achievements.Unlock (name)          -> "ok" or "unknown_achievement": unlocks at once
//   Achievements.Reset (name)           -> "ok" or "unknown_achievement": back to no progress
//   Achievements.IsUnlocked (name)      -> boolean
//   Achievements.GetProgress (name)     -> number (0 for an unknown one)
//   Achievements.GetTarget (name)       -> number (0 for an unknown one)
//   Achievements.GetTitle (name) / GetDescription (name) -> string (the title falls back to the name; "" for an unknown one)
//   Achievements.Exists (name)          -> boolean
//   Achievements.IsHidden (name)        -> boolean
//   Achievements.UnlockedCount ()       -> number of achievements unlocked
//   Achievements.NextUnlocked ()        -> the name of an achievement unlocked since it was last called, or "" when there is none (call it until "": the toasts)
//
// A call with a missing or wrong-typed argument raises a script error; without an active system the functions return false / 0 / "" /
// "unknown_achievement".

namespace aether::script {

void InstallAchievementsApi(LuauHost& host); // does nothing when the kit isn't built

} // namespace aether::script
