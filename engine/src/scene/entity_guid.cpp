#include "aether/scene/entity_guid.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <random>

namespace aether {

namespace {

u64 RandomU64() {
    // One generator per thread, seeded from the OS entropy source, so
    // concurrent callers never share state.
    thread_local std::mt19937_64 generator = [] {
        std::random_device device;
        std::seed_seq seed{device(), device(), device(), device(), device(), device(), device(), device()};
        return std::mt19937_64(seed);
    }();
    return generator();
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

EntityGuid NewEntityGuid() {
    EntityGuid guid;
    do {
        guid.hi = RandomU64();
        guid.lo = RandomU64();
        // RFC 9562 version 4: version nibble 0100, variant bits 10.
        guid.hi = (guid.hi & ~0x000000000000F000ull) | 0x0000000000004000ull;
        guid.lo = (guid.lo & ~0xC000000000000000ull) | 0x8000000000000000ull;
    } while (guid.IsNull()); // can't happen given the fixed bits, but keeps the invariant explicit
    return guid;
}

std::string ToString(const EntityGuid& guid) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    text.reserve(36);
    auto put = [&](u64 value, int first_nibble, int nibble_count) {
        for (int i = 0; i < nibble_count; ++i) {
            text.push_back(kHex[(value >> ((15 - (first_nibble + i)) * 4)) & 0xF]);
        }
    };
    put(guid.hi, 0, 8);
    text.push_back('-');
    put(guid.hi, 8, 4);
    text.push_back('-');
    put(guid.hi, 12, 4);
    text.push_back('-');
    put(guid.lo, 0, 4);
    text.push_back('-');
    put(guid.lo, 4, 12);
    return text;
}

bool ParseEntityGuid(std::string_view text, EntityGuid& out) {
    if (text.size() != 36 || text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-') {
        return false;
    }
    u64 parts[2] = {0, 0};
    int nibbles = 0;
    for (char c : text) {
        if (c == '-') {
            continue;
        }
        int value = HexValue(c);
        if (value < 0) {
            return false;
        }
        u64& part = parts[nibbles / 16];
        part = (part << 4) | static_cast<u64>(value);
        ++nibbles;
    }
    if (nibbles != 32) {
        return false;
    }
    out.hi = parts[0];
    out.lo = parts[1];
    return true;
}

std::vector<Entity> GuidIndex::Rebuild(World& world) {
    by_guid_.clear();
    std::vector<Entity> duplicates;
    // Visit in entity-index order so "the earlier entity keeps the GUID" is
    // deterministic, independent of archetype iteration order.
    std::vector<std::pair<Entity, EntityGuid>> found;
    world.ForEachArchetype([&](const Archetype& archetype) {
        ComponentId id = GetComponentId<IdComponent>();
        if (!archetype.Has(id)) {
            return;
        }
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            const Entity* entities = archetype.EntityArray(c);
            const auto* ids = static_cast<const IdComponent*>(archetype.ComponentArray(c, id));
            for (u32 row = 0; row < archetype.ChunkEntityCount(c); ++row) {
                found.push_back({entities[row], ids[row].guid});
            }
        }
    });
    std::sort(found.begin(), found.end(),
              [](const auto& a, const auto& b) { return a.first.index < b.first.index; });
    for (const auto& [entity, guid] : found) {
        if (guid.IsNull()) {
            continue;
        }
        if (!by_guid_.emplace(guid, entity).second) {
            duplicates.push_back(entity);
        }
    }
    return duplicates;
}

Entity GuidIndex::Find(const World& world, const EntityGuid& guid) const {
    auto it = by_guid_.find(guid);
    if (it == by_guid_.end() || !world.IsAlive(it->second)) {
        return kNullEntity;
    }
    const IdComponent* id = world.GetComponent<IdComponent>(it->second);
    if (id == nullptr || id->guid != guid) {
        return kNullEntity;
    }
    return it->second;
}

EntityGuid EnsureGuid(World& world, Entity entity, GuidIndex* index) {
    if (IdComponent* id = world.GetComponent<IdComponent>(entity); id != nullptr && !id->guid.IsNull()) {
        return id->guid;
    }
    return RegenerateGuid(world, entity, index);
}

EntityGuid RegenerateGuid(World& world, Entity entity, GuidIndex* index) {
    IdComponent* id = world.GetComponent<IdComponent>(entity);
    if (id == nullptr) {
        world.AddComponent(entity, IdComponent{});
        id = world.GetComponent<IdComponent>(entity);
    } else if (index != nullptr && !id->guid.IsNull() && index->Find(world, id->guid) == entity) {
        index->Remove(id->guid);
    }
    id->guid = NewEntityGuid();
    if (index != nullptr) {
        index->Add(id->guid, entity);
    }
    return id->guid;
}

namespace detail {

nlohmann::json EntityGuidToJson(const void* object) {
    return ToString(*static_cast<const EntityGuid*>(object));
}

bool EntityGuidFromJson(const nlohmann::json& data, void* object) {
    if (!data.is_string()) {
        return false;
    }
    return ParseEntityGuid(data.get_ref<const std::string&>(), *static_cast<EntityGuid*>(object));
}

} // namespace detail

} // namespace aether
