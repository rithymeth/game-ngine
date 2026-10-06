#pragma once

#include "aether/script/luau_host.h"

// Luau's `SaveGames` table (Phase 28 step 4, docs/design/PHASE_SPECS.md §28.4):
// the same functions as the Blueprint library (aether::save::SaveGames), on
// the active SaveSystem:
//
//   SaveGames.SetNumber / SetString / SetBool (key, value)
//   SaveGames.SetVector (key, x, y, z)
//   SaveGames.GetNumber / GetString / GetBool (key, default)
//   SaveGames.GetVector (key, dx, dy, dz)      -> x, y, z
//   SaveGames.HasKey / RemoveKey (key)
//   SaveGames.CreateSaveObject ()
//   SaveGames.SaveToSlot / LoadFromSlot / DoesSaveExist / DeleteSave (slot)  -> boolean
//   SaveGames.SaveToSlotAsync (slot)
//   SaveGames.GetSlotCount () / GetSlotName (index) / GetLastError ()
//   SaveGames.CaptureWorld () / RestoreWorld ()                              -> boolean
//
// A number is stored as a float unless it has no fractional part and fits an
// int, then as an int, so GetNumber reads either (a bag keeps one kind per
// key: Blueprint's Get Int on a number a script saved as 3 gives 3).
// A call with a missing or wrong-typed argument raises a script error.

namespace aether::script {

void InstallSaveApi(LuauHost& host);

} // namespace aether::script
