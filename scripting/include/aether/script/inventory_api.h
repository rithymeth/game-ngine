#pragma once

#include "aether/script/luau_host.h"

// Luau's `Inventory` table (Phase 30 step 7a, docs/design/PHASE_SPECS.md §30.7): the same
// functions as the `Items` Blueprint library, on the active InventorySystem. Present only
// when the inventory kit is built (otherwise the table doesn't exist). Slots count from 0.
//
//   Inventory.Configure (owner, capacity, maxWeight)            -> boolean
//   Inventory.Add (owner, item, count)                          -> number actually added
//   Inventory.Remove (owner, item, count)                       -> boolean (all or nothing)
//   Inventory.Count (owner, item)                               -> number
//   Inventory.Has (owner, item, count)                          -> boolean
//   Inventory.Move (owner, fromSlot, toSlot) / Split (owner, slot, count, toSlot) -> boolean
//   Inventory.GetSlotItem (owner, slot)                         -> string ("" if empty)
//   Inventory.GetSlotCount (owner, slot)                        -> number
//   Inventory.GetWeight (owner)                                 -> number
//   Inventory.Equip (owner, slot) / Unequip (owner, equipSlot) / Use (owner, slot) -> boolean
//   Inventory.GetEquipped (owner, equipSlot)                    -> string ("" if none)
//
// An entity's script hears OnItemAdded / OnItemRemoved (item, count), OnItemEquipped /
// OnItemUnequipped (item, slot) and OnItemUsed (item). A call with a missing or wrong-typed
// argument raises a script error; without an active system the functions return 0 / false / "".

namespace aether::script {

void InstallInventoryApi(LuauHost& host); // does nothing when the kit isn't built

} // namespace aether::script
