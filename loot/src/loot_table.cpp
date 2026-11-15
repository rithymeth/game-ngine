#include "aether/loot/loot_table.h"

#include <algorithm>

namespace aether::loot {

using reflect::Json;

namespace {

bool Fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

u64 NextRandom(u64& state) { // splitmix64: well mixed, and the same on every device
    state += 0x9e3779b97f4a7c15ull;
    u64 z = state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

bool ReadInt(const Json& j, const char* key, i32& out, std::string* error, const char* code) {
    if (!j.contains(key)) return true;
    if (!j[key].is_number_integer()) return Fail(error, std::string(code) + ": '" + key + "' is a whole number");
    const i64 v = j[key].get<i64>();
    out = static_cast<i32>(std::clamp<i64>(v, -1, static_cast<i64>(LootTableDef::kMaxWeight) + 1)); // out of range: Validate names it
    return true;
}

} // namespace

std::string LootTableDef::Validate() const {
    if (name.empty()) return "loot.name_empty: a loot table needs a name";
    if (rolls < 1 || rolls > kMaxRolls) return "loot.bad_rolls: the rolls of '" + name + "' are between 1 and " + std::to_string(kMaxRolls);
    if (entries.empty()) return "loot.no_entries: '" + name + "' has no entries";
    if (entries.size() > kMaxEntries) return "loot.too_many_entries: '" + name + "' has more than " + std::to_string(kMaxEntries) + " entries";
    bool any_item = false;
    for (const LootEntry& e : entries) {
        if (e.weight < 1 || e.weight > kMaxWeight) return "loot.bad_weight: an entry of '" + name + "' has a weight between 1 and " + std::to_string(kMaxWeight);
        if (e.min_count < 1 || e.max_count > kMaxCount || e.min_count > e.max_count) {
            return "loot.bad_count: the counts of an entry of '" + name + "' are between 1 and " + std::to_string(kMaxCount) + ", the minimum not above the maximum";
        }
        any_item = any_item || !e.item.empty();
    }
    if (!any_item) return "loot.nothing_to_drop: '" + name + "' drops nothing (every entry is empty)";
    return {};
}

Json LootTableToJson(const LootTableDef& table) {
    Json entries = Json::array();
    for (const LootEntry& e : table.entries) {
        Json j = {{"item", e.item}, {"weight", e.weight}, {"min", e.min_count}, {"max", e.max_count}};
        entries.push_back(std::move(j));
    }
    Json j = {{"name", table.name}, {"rolls", table.rolls}, {"entries", std::move(entries)}};
    if (!table.title.empty()) j["title"] = table.title;
    return j;
}

bool LootTableFromJson(const std::string& text, LootTableDef& out, std::string* error) {
    const Json j = Json::parse(text, nullptr, false);
    if (!j.is_object()) return Fail(error, "loot.bad_json: a loot table is a JSON object");
    LootTableDef table;
    for (const auto& [key, field] : std::initializer_list<std::pair<const char*, std::string*>>{{"name", &table.name}, {"title", &table.title}}) {
        if (!j.contains(key)) continue;
        if (!j[key].is_string()) return Fail(error, std::string("loot.bad_json: '") + key + "' is a string");
        *field = j[key].get<std::string>();
    }
    if (!ReadInt(j, "rolls", table.rolls, error, "loot.bad_rolls")) return false;
    if (j.contains("entries")) {
        if (!j["entries"].is_array()) return Fail(error, "loot.bad_json: 'entries' is a list");
        for (const Json& entry : j["entries"]) {
            if (!entry.is_object()) return Fail(error, "loot.bad_json: every entry is an object");
            LootEntry e;
            if (entry.contains("item")) {
                if (!entry["item"].is_string()) return Fail(error, "loot.bad_json: an entry's 'item' is a string");
                e.item = entry["item"].get<std::string>();
            }
            if (!ReadInt(entry, "weight", e.weight, error, "loot.bad_weight")) return false;
            if (!ReadInt(entry, "min", e.min_count, error, "loot.bad_count")) return false;
            e.max_count = entry.contains("max") ? e.max_count : e.min_count; // only a minimum: that many exactly
            if (!ReadInt(entry, "max", e.max_count, error, "loot.bad_count")) return false;
            table.entries.push_back(std::move(e));
        }
    }
    const std::string problem = table.Validate();
    if (!problem.empty()) return Fail(error, problem);
    out = std::move(table);
    return true;
}

std::vector<Drop> RollLoot(const LootTableDef& table, u64 seed) {
    std::vector<Drop> drops;
    if (!table.Validate().empty()) return drops;
    u64 total = 0;
    for (const LootEntry& e : table.entries) total += static_cast<u64>(e.weight);
    u64 state = seed;
    for (i32 roll = 0; roll < table.rolls; ++roll) {
        u64 pick = NextRandom(state) % total;
        const LootEntry* chosen = &table.entries.back();
        for (const LootEntry& e : table.entries) {
            if (pick < static_cast<u64>(e.weight)) {
                chosen = &e;
                break;
            }
            pick -= static_cast<u64>(e.weight);
        }
        const u64 span = static_cast<u64>(chosen->max_count - chosen->min_count) + 1;
        const i32 count = chosen->min_count + static_cast<i32>(NextRandom(state) % span); // drawn for an empty entry too: the stream does not depend on what dropped
        if (chosen->item.empty()) continue;
        const auto found = std::find_if(drops.begin(), drops.end(), [&](const Drop& d) { return d.item == chosen->item; });
        if (found != drops.end()) found->count += count;
        else drops.push_back({chosen->item, count});
    }
    return drops;
}

std::string DescribeDrops(const std::vector<Drop>& drops) {
    if (drops.empty()) return "nothing";
    std::string text;
    for (const Drop& d : drops) {
        if (!text.empty()) text += ", ";
        text += std::to_string(d.count) + " " + d.item;
    }
    return text;
}

bool LootLibrary::Register(LootTableDef table, std::string* error) {
    const std::string problem = table.Validate();
    if (!problem.empty()) return Fail(error, problem);
    const std::string name = table.name;
    tables_[name] = std::move(table);
    return true;
}

const LootTableDef* LootLibrary::Find(const std::string& name) const {
    const auto it = tables_.find(name);
    return it == tables_.end() ? nullptr : &it->second;
}

std::vector<std::string> LootLibrary::Names() const {
    std::vector<std::string> names;
    names.reserve(tables_.size());
    for (const auto& entry : tables_) names.push_back(entry.first);
    return names;
}

std::string LootLibrary::CheckItems(const LootTableDef& table, const inv::ItemLibrary& items) {
    for (const LootEntry& e : table.entries) {
        if (!e.item.empty() && items.Find(e.item) == nullptr) return "loot.unknown_item: '" + table.name + "' names '" + e.item + "', which isn't an item";
    }
    return {};
}

} // namespace aether::loot
