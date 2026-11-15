#pragma once

#include "aether/script/luau_host.h"

// Luau's `Crafting` table: recipes (`.arecipe`) crafted from an entity's inventory, on the active CraftingSystem. Present only when the
// crafting kit is built (it needs the inventory kit).
//
//   Crafting.Craft (owner, recipe [, times])      -> "ok", or why not: "missing_inputs", "no_room" (the outputs do not fit), "unknown_recipe",
//                                                    "bad_count" (times is 1 to 1000), "dead_entity". All or nothing: on anything but "ok"
//                                                    the inventory is unchanged.
//   Crafting.CanCraft (owner, recipe [, times])   -> boolean: the entity holds the inputs (room for the outputs is Craft's to say)
//   Crafting.MissingCount (owner, recipe [, times]) -> number: how many of the recipe's inputs the entity is short of (0: it has them all)
//   Crafting.Exists (recipe)                      -> boolean
//   Crafting.GetTitle (recipe)                    -> string (the recipe's title, or its name)
//   Crafting.Describe (recipe)                    -> string such as "4 Scrap + 2 Component -> 1 Generator Core", for a prompt
//
// Crafting moves items through the inventory, so the item events (and the quests that count items) hear it. A call with a missing or
// wrong-typed argument raises a script error; without an active system the functions return false / 0 / "" / "unknown_recipe".

namespace aether::script {

void InstallCraftingApi(LuauHost& host); // does nothing when the kit isn't built

} // namespace aether::script
