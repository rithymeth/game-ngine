#include "aether/inventory/inventory_system.h"

#include "aether/ecs/component.h"

#include <algorithm>
#include <cmath>

namespace aether::inv {

namespace {
InventorySystem* g_active = nullptr;
}

void RegisterInventoryComponents() {
    gas::RegisterGameplayComponents();
    (void)GetComponentId<Inventory>();
}

InventorySystem::InventorySystem(World& world, gas::EffectSystem* effects, const ItemLibrary& items) : world_(world), effects_(effects), items_(items) {
    RegisterInventoryComponents();
    MakeActive();
}

InventorySystem::~InventorySystem() {
    if (g_active == this) g_active = nullptr;
}

InventorySystem* InventorySystem::Active() { return g_active; }
void InventorySystem::MakeActive() { g_active = this; }

Inventory* InventorySystem::InventoryOf(Entity owner) const {
    if (owner.IsNull() || !world_.IsAlive(owner) || !world_.HasComponent<Inventory>(owner)) return nullptr;
    return world_.GetComponent<Inventory>(owner);
}

void InventorySystem::Queue(Entity owner, const std::string& item, i32 count, ItemEvent::Kind kind, const std::string& slot) {
    ItemEvent e;
    e.entity = owner;
    e.item = item;
    e.count = count;
    e.kind = kind;
    e.slot = slot;
    events_.push_back(std::move(e));
}

bool InventorySystem::Configure(Entity owner, i32 capacity, f32 max_weight) {
    if (owner.IsNull() || !world_.IsAlive(owner) || capacity < 0 || !std::isfinite(max_weight) || max_weight < 0.0f) return false;
    if (!world_.HasComponent<Inventory>(owner)) world_.AddComponent<Inventory>(owner, Inventory{});
    Inventory* inv = InventoryOf(owner);
    inv->Normalize();
    for (usize i = static_cast<usize>(capacity); i < inv->slots.size(); ++i) {
        if (inv->slots[i].count > 0) return false;
    }
    inv->capacity = capacity;
    inv->max_weight = max_weight;
    inv->Normalize();
    return true;
}

f32 InventorySystem::TotalWeight(Entity owner) const {
    const Inventory* inv = InventoryOf(owner);
    if (inv == nullptr) return 0.0f;
    f32 total = 0.0f;
    for (const ItemStack& s : inv->slots) {
        if (s.count <= 0) continue;
        const ItemDef* def = items_.Find(s.item);
        if (def != nullptr) total += def->weight * static_cast<f32>(s.count);
    }
    return total;
}

i32 InventorySystem::Count(Entity owner, const std::string& item) const {
    const Inventory* inv = InventoryOf(owner);
    if (inv == nullptr) return 0;
    i32 n = 0;
    for (const ItemStack& s : inv->slots) {
        if (s.count > 0 && s.item == item) n += s.count;
    }
    return n;
}

const ItemStack* InventorySystem::Slot(Entity owner, i32 index) const {
    const Inventory* inv = InventoryOf(owner);
    if (inv == nullptr || index < 0 || static_cast<usize>(index) >= inv->slots.size()) return nullptr;
    return &inv->slots[static_cast<usize>(index)];
}

std::string InventorySystem::Equipped(Entity owner, const std::string& slot) const {
    const Inventory* inv = InventoryOf(owner);
    if (inv == nullptr) return {};
    for (const EquipSlot& e : inv->equipment) {
        if (e.name == slot) return e.item;
    }
    return {};
}

// Puts up to `count` into existing stacks then empty slots; `placed` is how many went in.
bool InventorySystem::Place(Inventory& inv, const std::string& item, i32 count, i32 max_stack, i32& placed, bool respect_weight, f32 weight_each) const {
    placed = 0;
    if (count <= 0) return true;
    if (respect_weight && inv.max_weight > 0.0f && weight_each > 0.0f) {
        f32 used = 0.0f;
        for (const ItemStack& s : inv.slots) {
            if (s.count <= 0) continue;
            const ItemDef* d = items_.Find(s.item);
            if (d != nullptr) used += d->weight * static_cast<f32>(s.count);
        }
        const f32 room = inv.max_weight - used;
        const i32 fit = room <= 0.0f ? 0 : static_cast<i32>(std::floor(room / weight_each + 1e-4f));
        count = std::min(count, fit);
    }
    for (ItemStack& s : inv.slots) {
        if (count <= 0) break;
        if (s.count > 0 && s.item == item && s.count < max_stack) {
            const i32 put = std::min(count, max_stack - s.count);
            s.count += put;
            count -= put;
            placed += put;
        }
    }
    for (ItemStack& s : inv.slots) {
        if (count <= 0) break;
        if (s.count <= 0) {
            const i32 put = std::min(count, max_stack);
            s.item = item;
            s.count = put;
            count -= put;
            placed += put;
        }
    }
    return true;
}

i32 InventorySystem::Add(Entity owner, const std::string& item, i32 count) {
    const ItemDef* def = items_.Find(item);
    if (def == nullptr || count <= 0 || owner.IsNull() || !world_.IsAlive(owner)) return 0;
    if (!world_.HasComponent<Inventory>(owner)) world_.AddComponent<Inventory>(owner, Inventory{});
    Inventory* inv = InventoryOf(owner);
    inv->Normalize();
    i32 placed = 0;
    Place(*inv, item, count, def->max_stack, placed, true, def->weight);
    if (placed > 0) Queue(owner, item, placed, ItemEvent::Kind::Added);
    return placed;
}

bool InventorySystem::Remove(Entity owner, const std::string& item, i32 count) {
    Inventory* inv = InventoryOf(owner);
    if (inv == nullptr || count <= 0 || Count(owner, item) < count) return false;
    i32 left = count;
    for (ItemStack& s : inv->slots) {
        if (left <= 0) break;
        if (s.count > 0 && s.item == item) {
            const i32 take = std::min(left, s.count);
            s.count -= take;
            left -= take;
            if (s.count == 0) s = ItemStack{};
        }
    }
    Queue(owner, item, count, ItemEvent::Kind::Removed);
    return true;
}

bool InventorySystem::Move(Entity owner, i32 from, i32 to) {
    Inventory* inv = InventoryOf(owner);
    if (inv == nullptr || from == to || from < 0 || to < 0 || static_cast<usize>(from) >= inv->slots.size() || static_cast<usize>(to) >= inv->slots.size()) return false;
    ItemStack& a = inv->slots[static_cast<usize>(from)];
    ItemStack& b = inv->slots[static_cast<usize>(to)];
    if (a.count <= 0) return false;
    if (b.count <= 0) {
        b = a;
        a = ItemStack{};
    } else if (b.item == a.item) {
        const ItemDef* def = items_.Find(a.item);
        const i32 room = (def ? def->max_stack : 1) - b.count;
        const i32 put = std::min(room, a.count);
        b.count += put;
        a.count -= put;
        if (a.count == 0) a = ItemStack{};
    } else {
        std::swap(a, b);
    }
    return true;
}

bool InventorySystem::Split(Entity owner, i32 slot, i32 count, i32 to_slot) {
    Inventory* inv = InventoryOf(owner);
    if (inv == nullptr || slot == to_slot || slot < 0 || to_slot < 0 || static_cast<usize>(slot) >= inv->slots.size() || static_cast<usize>(to_slot) >= inv->slots.size()) return false;
    ItemStack& a = inv->slots[static_cast<usize>(slot)];
    ItemStack& b = inv->slots[static_cast<usize>(to_slot)];
    if (a.count <= 1 || count < 1 || count >= a.count || b.count > 0) return false;
    b.item = a.item;
    b.count = count;
    a.count -= count;
    return true;
}

bool InventorySystem::Equip(Entity owner, i32 slot) {
    Inventory* inv = InventoryOf(owner);
    if (inv == nullptr || slot < 0 || static_cast<usize>(slot) >= inv->slots.size()) return false;
    const ItemStack stack = inv->slots[static_cast<usize>(slot)];
    const ItemDef* def = stack.count > 0 ? items_.Find(stack.item) : nullptr;
    if (def == nullptr || def->equip_slot.empty()) return false;
    const std::string where = def->equip_slot;
    const bool occupied = !Equipped(owner, where).empty();
    // What is replaced goes back into the inventory: it needs this stack to empty, or a free slot.
    if (occupied && stack.count > 1 && std::none_of(inv->slots.begin(), inv->slots.end(), [](const ItemStack& s) { return s.count <= 0; })) return false;
    if (occupied) {
        // Unequip first (it needs room too: this slot frees up when the stack is one).
        if (stack.count == 1) {
            inv->slots[static_cast<usize>(slot)] = ItemStack{};
        } else {
            inv->slots[static_cast<usize>(slot)].count -= 1;
        }
        if (!Unequip(owner, where)) { // no room after all: put the item back
            inv = InventoryOf(owner);
            ItemStack& s = inv->slots[static_cast<usize>(slot)];
            s.item = stack.item;
            s.count = stack.count;
            return false;
        }
        inv = InventoryOf(owner);
    } else {
        if (stack.count == 1) inv->slots[static_cast<usize>(slot)] = ItemStack{};
        else inv->slots[static_cast<usize>(slot)].count -= 1;
    }
    // Applying effects can add components to the owner, which moves the inventory: take handles first.
    std::vector<u32> handles;
    if (effects_ != nullptr) {
        for (const std::string& name : def->equip_effects) {
            const auto r = effects_->Apply(owner, name, owner);
            if (r.handle != 0) handles.push_back(r.handle);
        }
    }
    inv = InventoryOf(owner);
    EquipSlot e;
    e.name = where;
    e.item = stack.item;
    e.handles = std::move(handles);
    inv->equipment.push_back(std::move(e));
    std::sort(inv->equipment.begin(), inv->equipment.end(), [](const EquipSlot& x, const EquipSlot& y) { return x.name < y.name; });
    Queue(owner, stack.item, 1, ItemEvent::Kind::Equipped, where);
    return true;
}

bool InventorySystem::Unequip(Entity owner, const std::string& slot) {
    Inventory* inv = InventoryOf(owner);
    if (inv == nullptr) return false;
    const auto it = std::find_if(inv->equipment.begin(), inv->equipment.end(), [&](const EquipSlot& e) { return e.name == slot; });
    if (it == inv->equipment.end()) return false;
    const std::string item = it->item;
    const ItemDef* def = items_.Find(item);
    i32 placed = 0;
    // Back into the inventory (the weight limit doesn't stop taking off what you wear).
    Place(*inv, item, 1, def ? def->max_stack : 1, placed, false, 0.0f);
    if (placed == 0) return false;
    const std::vector<u32> handles = it->handles;
    inv->equipment.erase(it);
    if (effects_ != nullptr) {
        for (const u32 h : handles) effects_->Remove(h);
    }
    Queue(owner, item, 1, ItemEvent::Kind::Unequipped, slot);
    return true;
}

bool InventorySystem::Use(Entity owner, i32 slot) {
    Inventory* inv = InventoryOf(owner);
    if (inv == nullptr || slot < 0 || static_cast<usize>(slot) >= inv->slots.size()) return false;
    const ItemStack stack = inv->slots[static_cast<usize>(slot)];
    const ItemDef* def = stack.count > 0 ? items_.Find(stack.item) : nullptr;
    if (def == nullptr || def->use_effect.empty() || effects_ == nullptr) return false;
    if (effects_->Apply(owner, def->use_effect, owner).status == gas::EffectSystem::Status::Rejected) return false;
    if (def->consume_on_use) {
        inv = InventoryOf(owner);
        ItemStack& s = inv->slots[static_cast<usize>(slot)];
        if (--s.count <= 0) s = ItemStack{};
    }
    Queue(owner, stack.item, 1, ItemEvent::Kind::Used);
    return true;
}

} // namespace aether::inv
