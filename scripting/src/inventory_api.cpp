#include "aether/script/inventory_api.h"

#if AETHER_KIT_INVENTORY
#include "aether/inventory/inventory_library.h"
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
bool NeedNumber(NativeCall& c, usize i, const char* what) {
    if (c.IsNumber(i)) return true;
    c.Fail(std::string("Inventory: ") + what + " must be a number");
    return false;
}
i32 Int(NativeCall& c, usize i) { return static_cast<i32>(c.Number(i)); }

} // namespace

void InstallInventoryApi(LuauHost& host) {
    const auto def = [&](const char* name, NativeFunction fn) { host.RegisterNative("Inventory", name, std::move(fn)); };
    def("Configure", [](NativeCall& c) {
        if (NeedEntity(c) && NeedNumber(c, 1, "the capacity")) c.Return(Items::ConfigureInventory(c.EntityArg(0), Int(c, 1), static_cast<f32>(c.Number(2, 0.0))));
    });
    def("Add", [](NativeCall& c) {
        if (NeedEntity(c) && NeedString(c, 1, "the item")) c.Return(static_cast<f64>(Items::AddItem(c.EntityArg(0), c.String(1), c.IsNumber(2) ? Int(c, 2) : 1)));
    });
    def("Remove", [](NativeCall& c) {
        if (NeedEntity(c) && NeedString(c, 1, "the item")) c.Return(Items::RemoveItem(c.EntityArg(0), c.String(1), c.IsNumber(2) ? Int(c, 2) : 1));
    });
    def("Count", [](NativeCall& c) {
        if (NeedEntity(c) && NeedString(c, 1, "the item")) c.Return(static_cast<f64>(Items::GetItemCount(c.EntityArg(0), c.String(1))));
    });
    def("Has", [](NativeCall& c) {
        if (NeedEntity(c) && NeedString(c, 1, "the item")) c.Return(Items::HasItem(c.EntityArg(0), c.String(1), c.IsNumber(2) ? Int(c, 2) : 1));
    });
    def("Move", [](NativeCall& c) {
        if (NeedEntity(c) && NeedNumber(c, 1, "the from slot") && NeedNumber(c, 2, "the to slot")) c.Return(Items::MoveItem(c.EntityArg(0), Int(c, 1), Int(c, 2)));
    });
    def("Split", [](NativeCall& c) {
        if (NeedEntity(c) && NeedNumber(c, 1, "the slot") && NeedNumber(c, 2, "the count") && NeedNumber(c, 3, "the to slot")) c.Return(Items::SplitStack(c.EntityArg(0), Int(c, 1), Int(c, 2), Int(c, 3)));
    });
    def("GetSlotItem", [](NativeCall& c) {
        if (NeedEntity(c) && NeedNumber(c, 1, "the slot")) c.Return(Items::GetSlotItem(c.EntityArg(0), Int(c, 1)));
    });
    def("GetSlotCount", [](NativeCall& c) {
        if (NeedEntity(c) && NeedNumber(c, 1, "the slot")) c.Return(static_cast<f64>(Items::GetSlotCount(c.EntityArg(0), Int(c, 1))));
    });
    def("GetWeight", [](NativeCall& c) {
        if (NeedEntity(c)) c.Return(static_cast<f64>(Items::GetTotalWeight(c.EntityArg(0))));
    });
    def("Equip", [](NativeCall& c) {
        if (NeedEntity(c) && NeedNumber(c, 1, "the slot")) c.Return(Items::EquipItem(c.EntityArg(0), Int(c, 1)));
    });
    def("Unequip", [](NativeCall& c) {
        if (NeedEntity(c) && NeedString(c, 1, "the equipment slot")) c.Return(Items::UnequipItem(c.EntityArg(0), c.String(1)));
    });
    def("Use", [](NativeCall& c) {
        if (NeedEntity(c) && NeedNumber(c, 1, "the slot")) c.Return(Items::UseItem(c.EntityArg(0), Int(c, 1)));
    });
    def("GetEquipped", [](NativeCall& c) {
        if (NeedEntity(c) && NeedString(c, 1, "the equipment slot")) c.Return(Items::GetEquippedItem(c.EntityArg(0), c.String(1)));
    });
}

#else

void InstallInventoryApi(LuauHost&) {}

#endif

} // namespace aether::script
