# Inventory and items

The inventory kit gives characters bags, stacks, weight limits, equipment and usable
items, built on the [gameplay](09-gameplay.md) effects. It is an optional part of the
build (`AETHER_KIT_INVENTORY`, on by default). The build spec is
[PHASE_SPECS §30.7](../design/PHASE_SPECS.md).

## Items

An item is a `.aitem` file under your content folder; it is cooked automatically.

```json
{ "name": "Potion", "display_key": "item.potion", "max_stack": 5, "weight": 0.5, "use_effect": "Heal" }
```

```json
{
  "name": "Iron Helm",
  "max_stack": 1,
  "weight": 3,
  "tags": ["Item.Armor"],
  "equip_slot": "Head",
  "equip_effects": ["ArmorBonus"]
}
```

`use_effect` and `equip_effects` name effects (`.aeffect`): `Heal` could be an instant
Health +25, and `ArmorBonus` an infinite effect adding Armor and granting a tag.
`consume_on_use` (default true) says whether using the item uses one up. A broken item is
a warning in the player, naming the file and the problem (`item.unknown_effect` if it names
an effect that doesn't exist); the rest still load.

## Inventories

An entity's `Inventory` component has a number of slots (20 by default), an optional
weight limit and what it is wearing. It is saved with the entity.

- `Add` fills partly full stacks, then empty slots, within the weight limit, and says how many fit.
- `Remove` is all or nothing, so crafting never half-spends.
- `Move` puts a stack in an empty slot, merges it into the same item, or swaps two items. `Split` takes some out of a stack.
- `Equip` wears one item in the slot its definition names and applies its effects to the wearer; what was worn goes back into the bag (there must be room). `Unequip` takes it off and removes the effects.
- `Use` applies the item's use effect and consumes it.

## From Luau

```lua
local Hero = {}

function Hero:OnStart()
    Inventory.Configure(self.entity, 12, 30)       -- 12 slots, 30 kg
    Inventory.Add(self.entity, "Potion", 3)
end

function Hero:OnUpdate(dt)
    if Input.GetBool("UsePotion") and Inventory.Has(self.entity, "Potion") then
        Inventory.Use(self.entity, 0)               -- slot 0
    end
end

function Hero:OnItemAdded(item, count) end          -- also OnItemRemoved
function Hero:OnItemEquipped(item, slot) end        -- also OnItemUnequipped
function Hero:OnItemUsed(item) end

return Hero
```

`Inventory` has `Configure`, `Add`, `Remove`, `Count`, `Has`, `Move`, `Split`,
`GetSlotItem`, `GetSlotCount`, `GetWeight`, `Equip`, `Unequip`, `Use` and `GetEquipped`.
Slots count from 0.

## From Blueprints

The `Items` library has the same functions (`AddItem`, `RemoveItem`, `GetItemCount`,
`EquipItem`, `UseItem` and so on), and the events are `Event.OnItemAdded`,
`Event.OnItemRemoved`, `Event.OnItemEquipped`, `Event.OnItemUnequipped` and
`Event.OnItemUsed`.

## Not built yet

Containers shared between entities (chests), items with their own state (durability),
crafting, and an inventory panel in the editor. Interaction, quests and dialogue are the
other genre kits, coming next.
