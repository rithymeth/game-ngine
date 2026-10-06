#include "aether/inventory/inventory_library.h"

#include "aether/inventory/inventory_system.h"

namespace aether::inv {

bool Items::ConfigureInventory(const Entity& owner, i32 capacity, f32 max_weight) {
    InventorySystem* s = InventorySystem::Active();
    return s && s->Configure(owner, capacity, max_weight);
}
i32 Items::AddItem(const Entity& owner, const std::string& item, i32 count) {
    InventorySystem* s = InventorySystem::Active();
    return s ? s->Add(owner, item, count) : 0;
}
bool Items::RemoveItem(const Entity& owner, const std::string& item, i32 count) {
    InventorySystem* s = InventorySystem::Active();
    return s && s->Remove(owner, item, count);
}
i32 Items::GetItemCount(const Entity& owner, const std::string& item) {
    const InventorySystem* s = InventorySystem::Active();
    return s ? s->Count(owner, item) : 0;
}
bool Items::HasItem(const Entity& owner, const std::string& item, i32 count) {
    const InventorySystem* s = InventorySystem::Active();
    return s && s->Has(owner, item, count);
}
bool Items::MoveItem(const Entity& owner, i32 from_slot, i32 to_slot) {
    InventorySystem* s = InventorySystem::Active();
    return s && s->Move(owner, from_slot, to_slot);
}
bool Items::SplitStack(const Entity& owner, i32 slot, i32 count, i32 to_slot) {
    InventorySystem* s = InventorySystem::Active();
    return s && s->Split(owner, slot, count, to_slot);
}
std::string Items::GetSlotItem(const Entity& owner, i32 slot) {
    const InventorySystem* s = InventorySystem::Active();
    const ItemStack* stack = s ? s->Slot(owner, slot) : nullptr;
    return stack && stack->count > 0 ? stack->item : std::string();
}
i32 Items::GetSlotCount(const Entity& owner, i32 slot) {
    const InventorySystem* s = InventorySystem::Active();
    const ItemStack* stack = s ? s->Slot(owner, slot) : nullptr;
    return stack ? stack->count : 0;
}
f32 Items::GetTotalWeight(const Entity& owner) {
    const InventorySystem* s = InventorySystem::Active();
    return s ? s->TotalWeight(owner) : 0.0f;
}
bool Items::EquipItem(const Entity& owner, i32 slot) {
    InventorySystem* s = InventorySystem::Active();
    return s && s->Equip(owner, slot);
}
bool Items::UnequipItem(const Entity& owner, const std::string& equip_slot) {
    InventorySystem* s = InventorySystem::Active();
    return s && s->Unequip(owner, equip_slot);
}
std::string Items::GetEquippedItem(const Entity& owner, const std::string& equip_slot) {
    const InventorySystem* s = InventorySystem::Active();
    return s ? s->Equipped(owner, equip_slot) : std::string();
}
bool Items::UseItem(const Entity& owner, i32 slot) {
    InventorySystem* s = InventorySystem::Active();
    return s && s->Use(owner, slot);
}

} // namespace aether::inv
