#include "aether/inventory/inventory.h"

#include <algorithm>

namespace aether::inv {

void Inventory::Normalize() {
    capacity = std::max(capacity, 0);
    slots.resize(static_cast<usize>(capacity));
    for (ItemStack& s : slots) {
        if (s.count <= 0) s = ItemStack{};
    }
    std::sort(equipment.begin(), equipment.end(), [](const EquipSlot& a, const EquipSlot& b) { return a.name < b.name; });
}

} // namespace aether::inv
