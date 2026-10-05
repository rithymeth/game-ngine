#pragma once

#include "aether/ecs/world.h"
#include "aether/gameplay/effect_system.h"
#include "aether/inventory/inventory.h"
#include "aether/inventory/item_def.h"

#include <string>
#include <vector>

namespace aether::inv {

// Registers the inventory component with the ECS (idempotent).
void RegisterInventoryComponents();

// Something happened to an entity's items (§30.8).
struct ItemEvent {
    enum class Kind : u8 { Added, Removed, Equipped, Unequipped, Used };
    Entity entity;
    std::string item;
    i32 count = 0;
    Kind kind = Kind::Added;
    std::string slot; // the equipment slot, for Equipped / Unequipped
};

// Adds, removes, moves, equips and uses entities' items. Stacks fill before new slots are
// taken; a weight limit and the slot count are respected; equipping applies the item's effects
// to the wearer through the EffectSystem (and unequipping takes them away); using applies the
// use effect and consumes. Changes queue events for the host to hand to scripts and Blueprints.
// Deterministic. The Blueprint library acts on the active system.
class InventorySystem {
public:
    static constexpr const char* kAddedEvent = "Event.OnItemAdded";
    static constexpr const char* kRemovedEvent = "Event.OnItemRemoved";
    static constexpr const char* kEquippedEvent = "Event.OnItemEquipped";
    static constexpr const char* kUnequippedEvent = "Event.OnItemUnequipped";
    static constexpr const char* kUsedEvent = "Event.OnItemUsed";

    // `effects` may be null: items then have no effects.
    InventorySystem(World& world, gas::EffectSystem* effects, const ItemLibrary& items);
    ~InventorySystem();
    InventorySystem(const InventorySystem&) = delete;
    InventorySystem& operator=(const InventorySystem&) = delete;

    static InventorySystem* Active();
    void MakeActive();

    // Gives the entity an inventory of `capacity` slots (and a weight limit; 0 for none), or
    // changes those of the one it has (slots holding items are never lost: a smaller capacity
    // is refused with false while they would be).
    bool Configure(Entity owner, i32 capacity, f32 max_weight = 0.0f);

    // How many were added (all of `count`, or fewer when slots or weight ran out); 0 for an
    // unknown item or a dead entity. Makes a default inventory (20 slots) if there is none.
    i32 Add(Entity owner, const std::string& item, i32 count = 1);
    // All or nothing: false (and nothing changes) if the entity has fewer than `count`.
    bool Remove(Entity owner, const std::string& item, i32 count = 1);
    // Into an empty slot, merged into the same item's stack (the rest stays), or swapped with another item.
    bool Move(Entity owner, i32 from, i32 to);
    // Takes `count` out of a stack into an empty slot (the stack keeps at least one).
    bool Split(Entity owner, i32 slot, i32 count, i32 to_slot);

    i32 Count(Entity owner, const std::string& item) const;
    bool Has(Entity owner, const std::string& item, i32 count = 1) const { return Count(owner, item) >= count; }
    f32 TotalWeight(Entity owner) const;
    const ItemStack* Slot(Entity owner, i32 index) const;
    std::string Equipped(Entity owner, const std::string& slot) const; // the item name, "" if none

    // Wears one item from a slot, in the slot its definition names; what was there goes back to
    // the inventory. False if it can't be worn, or there is no room for what it replaces.
    bool Equip(Entity owner, i32 slot);
    // Takes the worn item off, back into the inventory; false if there is no room.
    bool Unequip(Entity owner, const std::string& slot);
    // Applies the item's use effect to the owner and consumes one if it says so.
    bool Use(Entity owner, i32 slot);

    const std::vector<ItemEvent>& Events() const { return events_; }
    void ClearEvents() { events_.clear(); }

private:
    Inventory* InventoryOf(Entity owner) const;
    bool Place(Inventory& inv, const std::string& item, i32 count, i32 max_stack, i32& placed, bool respect_weight, f32 weight_each) const;
    void Queue(Entity owner, const std::string& item, i32 count, ItemEvent::Kind kind, const std::string& slot = {});

    World& world_;
    gas::EffectSystem* effects_;
    const ItemLibrary& items_;
    std::vector<ItemEvent> events_;
};

} // namespace aether::inv
