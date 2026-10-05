#include "test_framework.h"

#ifdef AETHER_TEST_HAS_INVENTORY

#include "aether/ecs/world.h"
#include "aether/inventory/inventory_library.h"
#include "aether/inventory/inventory_system.h"
#include "aether/reflection/registry.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/components.h"

#include <cmath>

// Phase 30 step 7a (§30.8): item definitions, inventories, equipment and use.

using namespace aether;
using namespace aether::inv;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

bool Near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }

ItemDef Item(const char* name, i32 stack, f32 weight = 0.0f) {
    ItemDef d;
    d.name = name;
    d.max_stack = stack;
    d.weight = weight;
    return d;
}

struct Rig {
    World world;
    gas::AttributeSystem attrs{world};
    gas::EffectLibrary effects;
    gas::EffectSystem fx{world, attrs, effects};
    ItemLibrary items;
    InventorySystem sys{world, &fx, items};
    Entity e;

    Rig() {
        gas::GameplayEffect heal;
        heal.name = "Heal";
        heal.modifiers = {{"Health", gas::GameplayEffect::Op::Add, 25}};
        gas::GameplayEffect armor;
        armor.name = "ArmorBonus";
        armor.duration_policy = gas::GameplayEffect::Duration::Infinite;
        armor.modifiers = {{"Armor", gas::GameplayEffect::Op::Add, 5}};
        armor.granted_tags = {gas::GameplayTag::Make("State.Armored")};
        CHECK(effects.Register(heal) && effects.Register(armor));
        ItemDef potion = Item("Potion", 5, 0.5f);
        potion.use_effect = "Heal";
        ItemDef helm = Item("Helm", 1, 3.0f);
        helm.equip_slot = "Head";
        helm.equip_effects = {"ArmorBonus"};
        ItemDef cap = Item("Cap", 1, 1.0f);
        cap.equip_slot = "Head";
        for (const ItemDef& d : {Item("Coin", 100), Item("Rock", 10, 2.0f), potion, helm, cap}) CHECK(items.Register(d));
        e = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
        attrs.Define(e, "Health", 50, 0, 100);
        attrs.Define(e, "Armor", 0);
    }
};

} // namespace

AETHER_TEST(Inventory_ItemJsonRoundTripAndErrors) {
    ItemDef d = Item("Sword", 1, 4.5f);
    d.display_key = "item.sword";
    d.icon = "Icons/sword.png";
    d.tags = {gas::GameplayTag::Make("Item.Weapon")};
    d.equip_slot = "MainHand";
    d.equip_effects = {"SwordDamage"};
    d.use_effect = "Sharpen";
    d.consume_on_use = false;
    ItemDef back;
    std::string err;
    CHECK(ItemFromJson(ItemToJson(d).dump(), back, &err) && back == d);
    const auto fails = [](const char* json, const char* code) {
        ItemDef out;
        std::string e;
        return !ItemFromJson(json, out, &e) && e.rfind(code, 0) == 0;
    };
    CHECK(fails("nonsense", "item.bad_json"));
    CHECK(fails("{}", "item.name_empty"));
    CHECK(fails(R"({"name":"x","max_stack":0})", "item.bad_stack"));
    CHECK(fails(R"({"name":"x","weight":-1})", "item.bad_weight"));
    CHECK(fails(R"({"name":"x","tags":["bad tag!"]})", "item.bad_tag"));
    CHECK(fails(R"({"name":"x","equip_effects":["A"]})", "item.equip_effects_without_slot"));
    CHECK(fails(R"({"name":"x","icon":3})", "item.bad_json"));
    Rig r;
    ItemDef ok = Item("Ok", 1);
    CHECK(ItemLibrary::CheckEffects(ok, r.effects).empty());
    ok.use_effect = "Missing";
    CHECK(ItemLibrary::CheckEffects(ok, r.effects).rfind("item.unknown_effect", 0) == 0);
}

AETHER_TEST(Inventory_AddStacksAndRespectsCapacityAndWeight) {
    Rig r;
    CHECK(r.sys.Configure(r.e, 3, 0.0f));
    CHECK(r.sys.Add(r.e, "Coin", 250) == 250 && r.sys.Count(r.e, "Coin") == 250); // 100 + 100 + 50
    CHECK(r.sys.Slot(r.e, 0)->count == 100 && r.sys.Slot(r.e, 2)->count == 50);
    CHECK(r.sys.Add(r.e, "Coin", 100) == 50 && r.sys.Count(r.e, "Coin") == 300); // full: only room for 50 more
    CHECK(r.sys.Add(r.e, "Rock", 1) == 0 && r.sys.Add(r.e, "Nope", 1) == 0 && r.sys.Add(r.e, "Coin", 0) == 0);
    // Weight: 10 limit, rocks weigh 2.
    CHECK(r.sys.Configure(r.e, 3, 10.0f) && r.sys.Remove(r.e, "Coin", 300));
    CHECK(r.sys.Add(r.e, "Rock", 8) == 5 && Near(r.sys.TotalWeight(r.e), 10.0f));
    CHECK(r.sys.Add(r.e, "Rock", 1) == 0);
    // A smaller capacity is refused while items would be lost.
    CHECK(r.sys.Configure(r.e, 3, 0.0f) && r.sys.Add(r.e, "Coin", 150) == 150 && !r.sys.Configure(r.e, 1, 0.0f) && r.sys.Configure(r.e, 5, 0.0f));
    // An entity with no inventory gets a default one.
    const Entity other = r.world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
    CHECK(r.sys.Add(other, "Coin", 5) == 5 && r.world.GetComponent<Inventory>(other)->capacity == 20);
    CHECK(r.sys.Add(Entity{}, "Coin", 1) == 0);
}

AETHER_TEST(Inventory_RemoveMoveAndSplit) {
    Rig r;
    r.sys.Configure(r.e, 5);
    r.sys.Add(r.e, "Coin", 150);
    r.sys.Add(r.e, "Rock", 3);
    CHECK(!r.sys.Remove(r.e, "Coin", 151) && r.sys.Count(r.e, "Coin") == 150); // all or nothing
    CHECK(r.sys.Remove(r.e, "Coin", 120) && r.sys.Count(r.e, "Coin") == 30 && r.sys.Slot(r.e, 0)->count == 0 && r.sys.Slot(r.e, 1)->count == 30);
    CHECK(r.sys.Move(r.e, 1, 4) && r.sys.Slot(r.e, 4)->item == "Coin" && r.sys.Slot(r.e, 1)->count == 0); // into an empty slot
    CHECK(r.sys.Move(r.e, 2, 4) && r.sys.Slot(r.e, 2)->item == "Coin" && r.sys.Slot(r.e, 4)->item == "Rock"); // swapped
    r.sys.Add(r.e, "Coin", 80);
    CHECK(r.sys.Count(r.e, "Coin") == 110);
    const i32 a = r.sys.Slot(r.e, 0)->count;
    CHECK(r.sys.Move(r.e, 2, 0) && r.sys.Count(r.e, "Coin") == 110 && r.sys.Slot(r.e, 0)->count == 100 && a > 0); // merged up to the stack size
    CHECK(r.sys.Split(r.e, 0, 40, 3) && r.sys.Slot(r.e, 0)->count == 60 && r.sys.Slot(r.e, 3)->count == 40);
    CHECK(!r.sys.Split(r.e, 0, 60, 1) && !r.sys.Split(r.e, 0, 10, 4) && !r.sys.Split(r.e, 0, 0, 1)); // keep one, empty target, a real count
    CHECK(!r.sys.Move(r.e, 0, 0) && !r.sys.Move(r.e, 9, 0) && !r.sys.Move(r.e, 1, 2));
}

AETHER_TEST(Inventory_EquipAppliesEffectsAndUnequipRemovesThem) {
    Rig r;
    r.sys.Configure(r.e, 2);
    r.sys.Add(r.e, "Helm", 1);
    r.sys.Add(r.e, "Cap", 1);
    CHECK(r.sys.Equip(r.e, 0) && r.sys.Equipped(r.e, "Head") == "Helm" && r.sys.Count(r.e, "Helm") == 0);
    CHECK(Near(r.attrs.Get(r.e, "Armor"), 5) && r.world.GetComponent<gas::TagContainer>(r.e)->HasTag(gas::GameplayTag::Make("State.Armored")));
    // Swapping in the cap puts the helm back and takes its effect away.
    CHECK(r.sys.Equip(r.e, 1) && r.sys.Equipped(r.e, "Head") == "Cap" && r.sys.Count(r.e, "Helm") == 1 && Near(r.attrs.Get(r.e, "Armor"), 0));
    CHECK(r.sys.Equip(r.e, 0) && r.sys.Equipped(r.e, "Head") == "Helm" && r.sys.Count(r.e, "Cap") == 1 && Near(r.attrs.Get(r.e, "Armor"), 5));
    CHECK(r.sys.Unequip(r.e, "Head") && r.sys.Equipped(r.e, "Head").empty() && Near(r.attrs.Get(r.e, "Armor"), 0) && r.sys.Count(r.e, "Helm") == 1);
    CHECK(!r.sys.Unequip(r.e, "Head") && !r.sys.Equip(r.e, 5));
    // A coin has no equipment slot, and an empty slot can't be worn.
    CHECK(r.sys.Configure(r.e, 3) && r.sys.Add(r.e, "Coin", 1) == 1);
    const i32 coin_slot = [&] { for (i32 i = 0; i < 3; ++i) if (r.sys.Slot(r.e, i)->item == "Coin") return i; return -1; }();
    CHECK(coin_slot >= 0 && !r.sys.Equip(r.e, coin_slot));
}

AETHER_TEST(Inventory_EquipNeedsRoomForWhatItReplaces) {
    Rig r;
    r.sys.Configure(r.e, 2);
    r.sys.Add(r.e, "Helm", 1);
    r.sys.Equip(r.e, 0);
    r.sys.Add(r.e, "Cap", 1);
    r.sys.Add(r.e, "Coin", 1);
    // The cap's stack empties when it is worn, leaving room for the helm.
    CHECK(r.sys.Equip(r.e, 0) && r.sys.Equipped(r.e, "Head") == "Cap" && r.sys.Count(r.e, "Helm") == 1);
    // Fill the inventory; a stack of caps can't swap in with nowhere to put the replaced item.
    ItemDef caps = Item("Caps", 5, 1.0f);
    caps.equip_slot = "Head";
    r.items.Register(caps);
    r.sys.Configure(r.e, 3);
    r.sys.Add(r.e, "Caps", 2);
    CHECK(r.sys.Count(r.e, "Helm") == 1 && r.sys.Count(r.e, "Coin") == 1 && r.sys.Count(r.e, "Caps") == 2);
    CHECK(!r.sys.Equip(r.e, [&] { for (i32 i = 0; i < 3; ++i) if (r.sys.Slot(r.e, i)->item == "Caps") return i; return -1; }()));
    CHECK(r.sys.Count(r.e, "Caps") == 2 && r.sys.Equipped(r.e, "Head") == "Cap");
}

AETHER_TEST(Inventory_UseAppliesTheEffectAndConsumes) {
    Rig r;
    r.sys.Configure(r.e, 2);
    r.sys.Add(r.e, "Potion", 2);
    r.sys.Add(r.e, "Rock", 1);
    CHECK(r.sys.Use(r.e, 0) && Near(r.attrs.GetBase(r.e, "Health"), 75) && r.sys.Count(r.e, "Potion") == 1);
    CHECK(r.sys.Use(r.e, 0) && r.sys.Count(r.e, "Potion") == 0 && r.sys.Slot(r.e, 0)->count == 0 && !r.sys.Use(r.e, 0));
    CHECK(!r.sys.Use(r.e, 1) && !r.sys.Use(r.e, 7)); // a rock does nothing
    ItemDef tonic = Item("Tonic", 1);
    tonic.use_effect = "Heal";
    tonic.consume_on_use = false;
    r.items.Register(tonic);
    r.sys.Add(r.e, "Tonic", 1);
    CHECK(r.sys.Use(r.e, 0) && r.sys.Count(r.e, "Tonic") == 1);
}

AETHER_TEST(Inventory_EventsAndSaveRoundTrip) {
    Rig r;
    r.sys.Configure(r.e, 4);
    r.sys.ClearEvents();
    r.sys.Add(r.e, "Helm", 1);
    r.sys.Equip(r.e, 0);
    r.sys.Add(r.e, "Potion", 1);
    r.sys.Use(r.e, 0);
    r.sys.Remove(r.e, "Coin", 1); // fails: no event
    r.sys.Unequip(r.e, "Head");
    using Kind = ItemEvent::Kind;
    std::vector<Kind> kinds;
    for (const ItemEvent& ev : r.sys.Events()) kinds.push_back(ev.kind);
    CHECK((kinds == std::vector<Kind>{Kind::Added, Kind::Equipped, Kind::Added, Kind::Used, Kind::Unequipped}));
    CHECK(r.sys.Events()[1].slot == "Head" && r.sys.Events()[1].item == "Helm");
    // The inventory (with what is worn and the handles of its effects) round-trips through JSON.
    r.sys.Equip(r.e, 0);
    const Inventory& inv = *r.world.GetComponent<Inventory>(r.e);
    const reflect::Json j = reflect::ToJson(inv);
    Inventory back;
    CHECK(reflect::FromJson(back, j) && back.capacity == 4 && back.slots.size() == inv.slots.size() && back.equipment.size() == 1 && back.equipment[0].item == "Helm" &&
          back.equipment[0].handles == inv.equipment[0].handles && !back.equipment[0].handles.empty());
    CHECK(reflect::TypeRegistry::Find("Inventory") != nullptr && reflect::TypeRegistry::Find("ItemStack") != nullptr);
    Inventory shortened;
    shortened.capacity = 3;
    shortened.slots.resize(1);
    shortened.Normalize();
    CHECK(shortened.slots.size() == 3);
}

AETHER_TEST(Inventory_BlueprintLibraryActsOnTheActiveSystem) {
    const reflect::TypeInfo* type = reflect::TypeRegistry::Find("Items");
    CHECK(type != nullptr && type->functions.size() == 14);
    World world;
    const Entity e = world.CreateEntity(Transform{Vec3(), Quaternion::Identity()});
    CHECK(InventorySystem::Active() == nullptr);
    CHECK(Items::AddItem(e, "Coin", 1) == 0 && !Items::EquipItem(e, 0) && Items::GetEquippedItem(e, "Head").empty() && Items::GetItemCount(e, "Coin") == 0 && !Items::HasItem(e, "Coin", 1));
    Rig r;
    CHECK(InventorySystem::Active() == &r.sys);
    CHECK(Items::ConfigureInventory(r.e, 4, 0.0f) && Items::AddItem(r.e, "Coin", 30) == 30 && Items::HasItem(r.e, "Coin", 30) && Items::GetItemCount(r.e, "Coin") == 30);
    CHECK(Items::GetSlotItem(r.e, 0) == "Coin" && Items::GetSlotCount(r.e, 0) == 30 && Items::GetSlotItem(r.e, 1).empty());
    CHECK(Items::SplitStack(r.e, 0, 10, 1) && Items::MoveItem(r.e, 1, 2) && Items::GetSlotCount(r.e, 2) == 10);
    CHECK(Items::RemoveItem(r.e, "Coin", 5) && Items::GetItemCount(r.e, "Coin") == 25 && Near(Items::GetTotalWeight(r.e), 0.0f));
    Items::AddItem(r.e, "Helm", 1);
    CHECK(Items::EquipItem(r.e, 1) || Items::EquipItem(r.e, 3) || Items::EquipItem(r.e, 0));
    CHECK(Items::GetEquippedItem(r.e, "Head") == "Helm" && Items::UnequipItem(r.e, "Head") && !Items::UseItem(r.e, 0));
}

#endif
