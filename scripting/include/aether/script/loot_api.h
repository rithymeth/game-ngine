#pragma once

#include "aether/script/luau_host.h"

// Luau's `Loot` table: loot tables (`.aloot`) rolled and given to an entity's inventory, on the active LootSystem. Present only when the loot kit is built
// (it needs the inventory kit).
//
//   Loot.Give (owner, table [, seed])  -> "ok", or why not: "unknown_table", "dead_entity". Rolls the table and puts the drops in the inventory; what does not
//                                         fit is lost (see Loot.LastGiven). With a seed the same seed always drops the same; without one the system's own seed
//                                         stream is used (the same every run unless Loot.SetSeed changes it)
//   Loot.LastGiven ()                  -> number of items the last Give actually put in an inventory
//   Loot.Roll (table)                  -> string such as "2 Scrap, 1 Component" ("nothing" when nothing drops, "" for an unknown table): what the next roll
//                                         would drop, without giving it (it does use up that step of the seed stream)
//   Loot.SetSeed (seed)                -> nothing: restarts the seed stream (any number)
//   Loot.Exists (table)                -> boolean
//   Loot.GetTitle (table)              -> string (the title, or the name; "" for an unknown table)
//
// Loot goes through the inventory, so the item events (and the quests that count items) hear it. A call with a missing or wrong-typed argument raises a
// script error; without an active system the functions return false / 0 / "" / "unknown_table".

namespace aether::script {

void InstallLootApi(LuauHost& host); // does nothing when the kit isn't built

} // namespace aether::script
