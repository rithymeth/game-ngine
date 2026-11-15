#include "aether/loot/loot_system.h"

namespace aether::loot {

namespace {
LootSystem* g_active = nullptr;

u64 Mix(u64 z) {
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
} // namespace

const char* ResultName(Result result) {
    switch (result) {
        case Result::Ok: return "ok";
        case Result::DeadEntity: return "dead_entity";
        case Result::UnknownTable: return "unknown_table";
    }
    return "unknown";
}

LootSystem::LootSystem(inv::InventorySystem& inventory, const LootLibrary& tables) : inventory_(inventory), tables_(tables) { MakeActive(); }
LootSystem::~LootSystem() {
    if (g_active == this) g_active = nullptr;
}
LootSystem* LootSystem::Active() { return g_active; }
void LootSystem::MakeActive() { g_active = this; }

void LootSystem::SetSeed(u64 seed) {
    base_seed_ = seed;
    counter_ = 0;
}

u64 LootSystem::NextSeed() { return Mix(base_seed_ + 0x9e3779b97f4a7c15ull * ++counter_); }

std::vector<Drop> LootSystem::Roll(const std::string& table) {
    const LootTableDef* def = tables_.Find(table);
    return def ? RollLoot(*def, NextSeed()) : std::vector<Drop>{};
}

Result LootSystem::GiveDrops(Entity owner, const std::vector<Drop>& drops, std::vector<Drop>* given) {
    last_given_ = 0;
    if (given) given->clear();
    for (const Drop& drop : drops) {
        const i32 added = inventory_.Add(owner, drop.item, drop.count);
        if (added > 0) {
            last_given_ += added;
            if (given) given->push_back({drop.item, added});
        }
    }
    return Result::Ok;
}

Result LootSystem::Give(Entity owner, const std::string& table, std::vector<Drop>* given) {
    if (given) given->clear();
    last_given_ = 0;
    if (owner.IsNull()) return Result::DeadEntity;
    const LootTableDef* def = tables_.Find(table);
    if (def == nullptr) return Result::UnknownTable;
    return GiveDrops(owner, RollLoot(*def, NextSeed()), given);
}

Result LootSystem::GiveSeeded(Entity owner, const std::string& table, u64 seed, std::vector<Drop>* given) {
    if (given) given->clear();
    last_given_ = 0;
    if (owner.IsNull()) return Result::DeadEntity;
    const LootTableDef* def = tables_.Find(table);
    if (def == nullptr) return Result::UnknownTable;
    return GiveDrops(owner, RollLoot(*def, seed), given);
}

} // namespace aether::loot
