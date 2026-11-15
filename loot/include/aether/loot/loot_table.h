#pragma once

#include "aether/core/base.h"
#include "aether/inventory/item_def.h"
#include "aether/reflection/serialize.h"

#include <map>
#include <string>
#include <vector>

namespace aether::loot {

// One thing a roll can land on: `weight` out of the table's total says how likely, and `min_count` to `max_count` how many. An empty `item` is a
// roll that drops nothing, so a table can say "half the time, nothing".
struct LootEntry {
    std::string item;
    i32 weight = 1;
    i32 min_count = 1;
    i32 max_count = 1;
    bool operator==(const LootEntry&) const = default;
};

// A loot table definition (`.aloot`): `rolls` rolls, each landing on one entry by weight.
struct LootTableDef {
    static constexpr i32 kMaxWeight = 1000000;
    static constexpr i32 kMaxCount = 100000;
    static constexpr i32 kMaxRolls = 100;
    static constexpr usize kMaxEntries = 1000;

    std::string name;
    std::string title; // shown text (empty: the name)
    i32 rolls = 1;
    std::vector<LootEntry> entries;

    // Empty string if valid, else a named error ("loot.name_empty", ...).
    std::string Validate() const;
    bool operator==(const LootTableDef&) const = default;
};

// An item and how many of it dropped.
struct Drop {
    std::string item;
    i32 count = 0;
    bool operator==(const Drop&) const = default;
};

reflect::Json LootTableToJson(const LootTableDef& table);
// False (with `error`, "loot.<code>: message") for bad JSON or an invalid table.
bool LootTableFromJson(const std::string& text, LootTableDef& out, std::string* error = nullptr);

// Rolls a table: the same table and seed always give the same drops. Each of the `rolls` picks an entry by weight and a count between its minimum and
// maximum; drops of the same item are added together, in the order each item first dropped. An invalid table drops nothing.
std::vector<Drop> RollLoot(const LootTableDef& table, u64 seed);
// "2 Scrap, 1 Component", or "nothing".
std::string DescribeDrops(const std::vector<Drop>& drops);

// Loot tables by name: filled from cooked `.aloot` assets by the player, or by code.
class LootLibrary {
public:
    bool Register(LootTableDef table, std::string* error = nullptr);
    const LootTableDef* Find(const std::string& name) const;
    void Clear() { tables_.clear(); }
    usize Size() const { return tables_.size(); }
    std::vector<std::string> Names() const; // sorted

    // Every item a table names must be an item ("loot.unknown_item"): the first problem of one table, or empty.
    static std::string CheckItems(const LootTableDef& table, const inv::ItemLibrary& items);

private:
    std::map<std::string, LootTableDef> tables_;
};

} // namespace aether::loot
