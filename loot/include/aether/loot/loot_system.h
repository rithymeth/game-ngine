#pragma once

#include "aether/ecs/entity.h"
#include "aether/inventory/inventory_system.h"
#include "aether/loot/loot_table.h"

#include <string>
#include <vector>

namespace aether::loot {

enum class Result : u8 { Ok, DeadEntity, UnknownTable };
// "ok", "dead_entity" (no entity was given), "unknown_table"
const char* ResultName(Result result);

// Rolls loot tables and gives the drops to an entity's inventory. Unlike crafting, loot is not all or nothing: what does not fit (slots or weight) is
// lost, and `given` says what actually went in. The item events are the inventory's own, so quests that count items hear loot. An entity that no
// longer exists receives nothing (the inventory refuses it), which shows as an empty `given`.
//
// Deterministic: the rolls come from a seed stream, so the same seed (SetSeed) and the same calls give the same loot. A script that wants it to
// differ from run to run seeds it with something that does (the time, a saved number); a test seeds it with a constant.
class LootSystem {
public:
    LootSystem(inv::InventorySystem& inventory, const LootLibrary& tables);
    ~LootSystem();
    LootSystem(const LootSystem&) = delete;
    LootSystem& operator=(const LootSystem&) = delete;

    static LootSystem* Active();
    void MakeActive();

    // Restarts the seed stream (any number).
    void SetSeed(u64 seed);
    // What the next roll of `table` would drop, advancing the stream; empty for an unknown table.
    std::vector<Drop> Roll(const std::string& table);
    // Rolls `table` and gives the drops to `owner`'s inventory.
    Result Give(Entity owner, const std::string& table, std::vector<Drop>* given = nullptr);
    // The same with a seed of the caller's own (the stream is not used or advanced): the same seed always gives the same drops.
    Result GiveSeeded(Entity owner, const std::string& table, u64 seed, std::vector<Drop>* given = nullptr);
    // How many items the last Give or GiveSeeded actually put in an inventory (0 before any).
    i32 LastGiven() const { return last_given_; }

    const LootLibrary& Tables() const { return tables_; }

private:
    Result GiveDrops(Entity owner, const std::vector<Drop>& drops, std::vector<Drop>* given);
    u64 NextSeed();

    inv::InventorySystem& inventory_;
    const LootLibrary& tables_;
    u64 base_seed_ = 0x243f6a8885a308d3ull;
    u64 counter_ = 0;
    i32 last_given_ = 0;
};

} // namespace aether::loot
