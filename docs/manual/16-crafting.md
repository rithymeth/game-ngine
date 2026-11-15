# Crafting

The crafting kit turns items into other items: a recipe lists what it consumes and what it makes, and a
`CraftingSystem` carries it out on an entity's inventory. It is an optional part of the build
(`AETHER_KIT_CRAFTING`, on by default) and needs the inventory kit.

## A recipe

A recipe is a `.arecipe` file under your content folder; it is cooked automatically.

```json
{
  "name": "GeneratorCore",
  "title": "Generator Core",
  "inputs":  [ { "item": "Scrap", "count": 4 }, { "item": "Component", "count": 2 } ],
  "outputs": [ { "item": "Generator Core", "count": 1 } ]
}
```

- `inputs` and `outputs` each need at least one line; a line names an item and a `count` (1 to 100000, default 1).
- Every item must exist in an `.aitem` file. A recipe that names an unknown item, or has no inputs or outputs, is a warning in the player that names the file.

## Crafting

`CraftingSystem::Craft(owner, recipe, times)` is **all or nothing**: it checks the inputs, takes them, and adds the outputs. If the
outputs do not fit (weight or slots), the inputs are given back and the result is `NoRoom`. `times` is 1 to 1000.
`CanCraft` and `Missing` answer without changing anything. The results are `Ok`, `DeadEntity`, `UnknownRecipe`, `BadCount`,
`MissingInputs` and `NoRoom`.

Crafting goes through the inventory, so the inventory's item events (and the quests that count items) hear it.

## From a script

```lua
if Crafting.CanCraft(player, "GeneratorCore") then
    local result = Crafting.Craft(player, "GeneratorCore")   -- "ok", "missing_inputs", "no_room", ...
end
print(Crafting.Describe("GeneratorCore"))  -- "4 Scrap + 2 Component -> 1 Generator Core"
```

`Crafting.Craft`, `CanCraft`, `MissingCount`, `Exists`, `GetTitle` and `Describe` are in the Luau inventory.
