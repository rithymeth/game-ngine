#include "aether/script/inventory_api.h"

#if AETHER_KIT_INVENTORY
#include "aether/inventory/inventory_library.h"

#include <cmath>
#include <limits>
#endif

namespace aether::script {

#if AETHER_KIT_INVENTORY

namespace {

using namespace aether::inv;

bool NeedEntity(NativeCall& c) {
    if (c.IsEntity(0)) return true;
    c.Fail("Inventory: the owner must be an entity");
    return false;
}
bool NeedString(NativeCall& c, usize i, const char* what) {
    if (c.IsString(i)) return true;
    c.Fail(std::string("Inventory: ") + what + " must be a string");
    return false;
}
bool ReadInt(NativeCall& c, usize i, const char* what, i32& out) {
    if (!c.IsNumber(i)) {
        c.Fail(std::string("Inventory: ") + what + " must be a number");
        return false;
    }
    // Keep truncation for representable numbers, but do not cast NaN,
    // infinity, or any value whose truncated result would not fit.
    const f64 value = std::trunc(c.Number(i));
    if (!(value >= std::numeric_limits<i32>::min() && value <= std::numeric_limits<i32>::max())) {
        c.Fail(std::string("Inventory: ") + what + " is outside the signed 32-bit range");
        return false;
    }
    out = static_cast<i32>(value);
    return true;
}

bool ReadCount(NativeCall& c, i32& out) {
    if (c.IsNumber(2)) return ReadInt(c, 2, "the count", out);
    out = 1; // preserve the optional count's default
    return true;
}

void Install(LuauHost& host, inv::InventorySystem* system, bool bound) {
    const auto def = [&](const char* name, NativeFunction fn) {
        if (!bound) {
            host.RegisterNative("Inventory", name, std::move(fn));
            return;
        }
        host.RegisterNative("Inventory", name, [system, fn = std::move(fn)](NativeCall& c) {
            InventoryLibraryScope scope(system);
            fn(c);
        });
    };
    def("Configure", [](NativeCall& c) {
        i32 capacity;
        if (NeedEntity(c) && ReadInt(c, 1, "the capacity", capacity))
            c.Return(Items::ConfigureInventory(c.EntityArg(0), capacity, static_cast<f32>(c.Number(2, 0.0))));
    });
    def("Add", [](NativeCall& c) {
        i32 count;
        if (NeedEntity(c) && NeedString(c, 1, "the item") && ReadCount(c, count))
            c.Return(static_cast<f64>(Items::AddItem(c.EntityArg(0), c.String(1), count)));
    });
    def("Remove", [](NativeCall& c) {
        i32 count;
        if (NeedEntity(c) && NeedString(c, 1, "the item") && ReadCount(c, count))
            c.Return(Items::RemoveItem(c.EntityArg(0), c.String(1), count));
    });
    def("Count", [](NativeCall& c) {
        if (NeedEntity(c) && NeedString(c, 1, "the item")) c.Return(static_cast<f64>(Items::GetItemCount(c.EntityArg(0), c.String(1))));
    });
    def("Has", [](NativeCall& c) {
        i32 count;
        if (NeedEntity(c) && NeedString(c, 1, "the item") && ReadCount(c, count))
            c.Return(Items::HasItem(c.EntityArg(0), c.String(1), count));
    });
    def("Move", [](NativeCall& c) {
        i32 from, to;
        if (NeedEntity(c) && ReadInt(c, 1, "the from slot", from) && ReadInt(c, 2, "the to slot", to))
            c.Return(Items::MoveItem(c.EntityArg(0), from, to));
    });
    def("Split", [](NativeCall& c) {
        i32 slot, count, to;
        if (NeedEntity(c) && ReadInt(c, 1, "the slot", slot) && ReadInt(c, 2, "the count", count) && ReadInt(c, 3, "the to slot", to))
            c.Return(Items::SplitStack(c.EntityArg(0), slot, count, to));
    });
    def("GetSlotItem", [](NativeCall& c) {
        i32 slot;
        if (NeedEntity(c) && ReadInt(c, 1, "the slot", slot)) c.Return(Items::GetSlotItem(c.EntityArg(0), slot));
    });
    def("GetSlotCount", [](NativeCall& c) {
        i32 slot;
        if (NeedEntity(c) && ReadInt(c, 1, "the slot", slot)) c.Return(static_cast<f64>(Items::GetSlotCount(c.EntityArg(0), slot)));
    });
    def("GetWeight", [](NativeCall& c) {
        if (NeedEntity(c)) c.Return(static_cast<f64>(Items::GetTotalWeight(c.EntityArg(0))));
    });
    def("Equip", [](NativeCall& c) {
        i32 slot;
        if (NeedEntity(c) && ReadInt(c, 1, "the slot", slot)) c.Return(Items::EquipItem(c.EntityArg(0), slot));
    });
    def("Unequip", [](NativeCall& c) {
        if (NeedEntity(c) && NeedString(c, 1, "the equipment slot")) c.Return(Items::UnequipItem(c.EntityArg(0), c.String(1)));
    });
    def("Use", [](NativeCall& c) {
        i32 slot;
        if (NeedEntity(c) && ReadInt(c, 1, "the slot", slot)) c.Return(Items::UseItem(c.EntityArg(0), slot));
    });
    def("GetEquipped", [](NativeCall& c) {
        if (NeedEntity(c) && NeedString(c, 1, "the equipment slot")) c.Return(Items::GetEquippedItem(c.EntityArg(0), c.String(1)));
    });
}

} // namespace

void InstallInventoryApi(LuauHost& host) { Install(host, nullptr, false); }
void InstallInventoryApi(LuauHost& host, inv::InventorySystem* system) { Install(host, system, true); }

#else

void InstallInventoryApi(LuauHost&) {}
void InstallInventoryApi(LuauHost&, inv::InventorySystem*) {}

#endif

} // namespace aether::script
