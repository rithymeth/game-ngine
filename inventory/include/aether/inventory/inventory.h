#pragma once

#include "aether/core/base.h"
#include "aether/reflection/reflection.h"

#include <string>
#include <vector>

namespace aether::inv {

// A stack in an inventory slot; an empty slot has count 0.
struct ItemStack {
    std::string item;
    i32 count = 0;
};

// What is worn in a named slot, and the handles of the effects it applies (saved with the
// entity's effects, so a loaded game keeps them).
struct EquipSlot {
    std::string name;
    std::string item;
    std::vector<u32> handles;
};

// An entity's items (Phase 30 step 7a, §30.8): `capacity` slots, a weight limit and
// what it is wearing. A component, saved with the entity.
struct Inventory {
    i32 capacity = 20;
    f32 max_weight = 0.0f; // 0: no limit
    std::vector<ItemStack> slots;
    std::vector<EquipSlot> equipment; // sorted by slot name

    // Makes `slots` as long as `capacity` (a loaded inventory may be short).
    void Normalize();
};

} // namespace aether::inv

AETHER_REFLECT(aether::inv::ItemStack, 1, AETHER_FIELD(item, Field_EditAnywhere), AETHER_FIELD(count, Field_EditAnywhere))
AETHER_REFLECT(aether::inv::EquipSlot, 1, AETHER_FIELD(name, Field_EditAnywhere), AETHER_FIELD(item, Field_EditAnywhere), AETHER_FIELD(handles, Field_EditAnywhere))
AETHER_REFLECT(aether::inv::Inventory, 1,
    AETHER_FIELD(capacity, Field_EditAnywhere, {.tooltip = "How many slots"}),
    AETHER_FIELD(max_weight, Field_EditAnywhere, {.tooltip = "0 for no limit"}),
    AETHER_FIELD(slots, Field_EditAnywhere),
    AETHER_FIELD(equipment, Field_EditAnywhere))
