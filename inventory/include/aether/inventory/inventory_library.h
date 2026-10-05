#pragma once

#include "aether/core/base.h"
#include "aether/ecs/entity.h"
#include "aether/reflection/reflection.h"

#include <string>

// Blueprint function library for inventories (Phase 30 step 7a, §30.8), acting on
// InventorySystem::Active(); without one everything returns 0 / false / "".
// `Event.OnItemAdded` / `OnItemRemoved` (item, count), `Event.OnItemEquipped` /
// `OnItemUnequipped` (item, slot) and `Event.OnItemUsed` (item) are what an entity's
// Blueprint hears.

namespace aether::inv {

struct Items {
    static bool ConfigureInventory(const Entity& owner, i32 capacity, f32 max_weight);
    // How many were added (fewer than asked if it was full or too heavy).
    static i32 AddItem(const Entity& owner, const std::string& item, i32 count);
    static bool RemoveItem(const Entity& owner, const std::string& item, i32 count);
    static i32 GetItemCount(const Entity& owner, const std::string& item);
    static bool HasItem(const Entity& owner, const std::string& item, i32 count);
    static bool MoveItem(const Entity& owner, i32 from_slot, i32 to_slot);
    static bool SplitStack(const Entity& owner, i32 slot, i32 count, i32 to_slot);
    static std::string GetSlotItem(const Entity& owner, i32 slot);
    static i32 GetSlotCount(const Entity& owner, i32 slot);
    static f32 GetTotalWeight(const Entity& owner);
    static bool EquipItem(const Entity& owner, i32 slot);
    static bool UnequipItem(const Entity& owner, const std::string& equip_slot);
    static std::string GetEquippedItem(const Entity& owner, const std::string& equip_slot);
    static bool UseItem(const Entity& owner, i32 slot);
};

} // namespace aether::inv

AETHER_REFLECT(aether::inv::Items, 1,
    AETHER_METHOD(ConfigureInventory, Fn_BlueprintCallable, {"owner", "capacity", "max_weight"}),
    AETHER_METHOD(AddItem, Fn_BlueprintCallable, {"owner", "item", "count"}),
    AETHER_METHOD(RemoveItem, Fn_BlueprintCallable, {"owner", "item", "count"}),
    AETHER_METHOD(GetItemCount, Fn_BlueprintCallable | Fn_Pure, {"owner", "item"}),
    AETHER_METHOD(HasItem, Fn_BlueprintCallable | Fn_Pure, {"owner", "item", "count"}),
    AETHER_METHOD(MoveItem, Fn_BlueprintCallable, {"owner", "from_slot", "to_slot"}),
    AETHER_METHOD(SplitStack, Fn_BlueprintCallable, {"owner", "slot", "count", "to_slot"}),
    AETHER_METHOD(GetSlotItem, Fn_BlueprintCallable | Fn_Pure, {"owner", "slot"}),
    AETHER_METHOD(GetSlotCount, Fn_BlueprintCallable | Fn_Pure, {"owner", "slot"}),
    AETHER_METHOD(GetTotalWeight, Fn_BlueprintCallable | Fn_Pure, {"owner"}),
    AETHER_METHOD(EquipItem, Fn_BlueprintCallable, {"owner", "slot"}),
    AETHER_METHOD(UnequipItem, Fn_BlueprintCallable, {"owner", "equip_slot"}),
    AETHER_METHOD(GetEquippedItem, Fn_BlueprintCallable | Fn_Pure, {"owner", "equip_slot"}),
    AETHER_METHOD(UseItem, Fn_BlueprintCallable, {"owner", "slot"}))
