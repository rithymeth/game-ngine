# Loot

The loot kit decides what a thing drops: a crate, a defeated enemy, a chest. A **loot table** lists what can drop and how likely it is, and the kit
rolls it and puts the drops in an entity's inventory. It is an optional part of the build (`AETHER_KIT_LOOT`, on by default) and needs the inventory kit.

## A loot table

A loot table is a `.aloot` file under your content folder; it is cooked automatically.

```json
{
  "name": "Salvage",
  "title": "Salvage",
  "rolls": 2,
  "entries": [
    { "item": "Scrap", "weight": 6, "min": 1, "max": 4 },
    { "item": "Component", "weight": 2, "min": 1, "max": 2 },
    { "item": "", "weight": 4 }
  ]
}
```

- `rolls` is how many times the table is rolled (1 to 100; default 1). Each roll lands on **one** entry.
- An entry's `weight` (1 to 1,000,000; default 1) is its share of the total: weights 6, 2 and 4 make the entries 50%, 17% and 33% likely.
- `min` and `max` are how many drop (1 to 100,000; default 1). With only `min`, exactly that many.
- An entry with an **empty `item`** is a roll that drops nothing, so a table can say "a third of the time, nothing". At least one entry has to name an item.
- Drops of the same item from different rolls are added together.

A table with no name, no entries, a weight or count out of range, a minimum above its maximum, or an item that is not an item is a warning in the player
that names the file.

## Rolling and giving

`Loot.Give(owner, table)` rolls the table and puts the drops in the inventory. Loot is **not all or nothing**, unlike crafting: what does not fit (slots or
weight) is lost, and `Loot.LastGiven()` says how many items actually went in. It goes through the inventory, so the item events, and the quests that count
items, hear it. An owner that no longer exists receives nothing.

Rolls are **deterministic**: the same table and seed always give the same drop. `Loot.Give(owner, table, seed)` uses your seed. Without one the kit uses its own seed stream,
which starts the same every run, so a game that wants different loot each time seeds it with something that changes (`Loot.SetSeed(os.time())`, a saved number, the
position of the crate).

## From a script

```lua
function Crate:OnInteract(who)
    Loot.Give(who, "Salvage")
    print("You found: " .. tostring(Loot.LastGiven()) .. " items")
end
```

| Call | Result |
| --- | --- |
| `Loot.Give(owner, table [, seed])` | `"ok"`, `"unknown_table"` or `"dead_entity"` |
| `Loot.LastGiven()` | how many items the last `Give` put in an inventory |
| `Loot.Roll(table)` | a string such as `"2 Scrap, 1 Component"` (`"nothing"`, or `""` for an unknown table): what the next roll would drop, without giving it |
| `Loot.SetSeed(seed)` | restarts the seed stream |
| `Loot.Exists(table)` / `GetTitle(table)` | a boolean / the title (or the name) |
