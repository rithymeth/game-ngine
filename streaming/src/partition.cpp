#include "aether/streaming/partition.h"

#include "aether/scene/components.h"
#include "aether/scene/entity_guid.h"
#include "aether/scene/hierarchy.h"
#include "aether/scene/serialization.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace aether {

void RegisterStreamingComponents() {
    (void)GetComponentId<AlwaysLoaded>();
    (void)GetComponentId<StreamingSource>();
    (void)GetComponentId<Transform>();
    (void)GetComponentId<IdComponent>();
    (void)GetComponentId<Parent>();
}

} // namespace aether

namespace aether::streaming {

using nlohmann::json;

namespace {
bool Fail(std::string* error, const std::string& m) {
    if (error != nullptr) *error = m;
    return false;
}

std::vector<Entity> AllEntities(const World& world) {
    std::vector<Entity> out;
    world.ForEachArchetype([&](Archetype& archetype) {
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            Entity* e = archetype.EntityArray(c);
            out.insert(out.end(), e, e + archetype.ChunkEntityCount(c));
        }
    });
    std::sort(out.begin(), out.end(), [](Entity a, Entity b) { return a.index < b.index; });
    return out;
}

bool WriteBytes(const std::filesystem::path& path, const std::vector<u8>& bytes) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}
} // namespace

std::string CellName(const CellCoord& cell) { return std::to_string(cell.x) + "_" + std::to_string(cell.z); }

CellCoord CellOf(const Vec3& p, const PartitionSettings& s) {
    const f32 size = s.cell_size > 0.0f ? s.cell_size : 1.0f;
    return {static_cast<i32>(std::floor((p.x - s.origin.x) / size)), static_cast<i32>(std::floor((p.z - s.origin.z) / size))};
}

void CellBounds(const CellCoord& c, const PartitionSettings& s, Vec3& lo, Vec3& hi) {
    lo = Vec3(s.origin.x + static_cast<f32>(c.x) * s.cell_size, -1e30f, s.origin.z + static_cast<f32>(c.z) * s.cell_size);
    hi = Vec3(lo.x + s.cell_size, 1e30f, lo.z + s.cell_size);
}

const CellInfo* WorldIndex::Find(const CellCoord& cell) const {
    const auto it = std::lower_bound(cells.begin(), cells.end(), cell, [](const CellInfo& c, const CellCoord& k) { return c.coord < k; });
    return it != cells.end() && it->coord == cell ? &*it : nullptr;
}

json SaveWorldIndex(const WorldIndex& index) {
    json cells = json::array();
    for (const CellInfo& c : index.cells) {
        cells.push_back({{"x", c.coord.x}, {"z", c.coord.z}, {"entities", c.entity_count},
                         {"min", {c.min.x, c.min.y, c.min.z}}, {"max", {c.max.x, c.max.y, c.max.z}}});
    }
    const Vec3& o = index.settings.origin;
    return {{"version", 1}, {"cell_size", index.settings.cell_size}, {"origin", {o.x, o.y, o.z}}, {"persistent", index.persistent_count}, {"cells", cells}};
}

bool LoadWorldIndex(const json& j, WorldIndex& out, std::string* error) {
    try {
        if (!j.is_object() || !j.contains("cells")) return Fail(error, "not a world index (no cells)");
        if (j.value("version", 1) > 1) return Fail(error, "made by a newer version");
        WorldIndex index;
        index.settings.cell_size = j.at("cell_size").get<f32>();
        if (index.settings.cell_size <= 0.0f) return Fail(error, "cell_size must be more than 0");
        const auto vec = [](const json& a) { return Vec3(a.at(0).get<f32>(), a.at(1).get<f32>(), a.at(2).get<f32>()); };
        if (j.contains("origin")) index.settings.origin = vec(j.at("origin"));
        index.persistent_count = j.value("persistent", 0u);
        for (const json& c : j.at("cells")) {
            CellInfo info;
            info.coord = {c.at("x").get<i32>(), c.at("z").get<i32>()};
            info.entity_count = c.value("entities", 0u);
            if (c.contains("min")) info.min = vec(c.at("min"));
            if (c.contains("max")) info.max = vec(c.at("max"));
            index.cells.push_back(info);
        }
        std::sort(index.cells.begin(), index.cells.end(), [](const CellInfo& a, const CellInfo& b) { return a.coord < b.coord; });
        out = std::move(index);
        return true;
    } catch (const json::exception& e) {
        return Fail(error, std::string("world index: ") + e.what());
    }
}

Entity CopyEntity(const World& from, Entity entity, World& to) {
    ComponentMask mask;
    const ComponentId count = RegisteredComponentCount();
    for (ComponentId id = 0; id < count; ++id)
        if (from.HasComponentRaw(entity, id)) mask.set(id);
    const Entity copy = to.CreateEntityRaw(mask);
    std::vector<u8> bytes;
    for (ComponentId id = 0; id < count; ++id) {
        if (!mask.test(id)) continue;
        const ComponentInfo& info = GetComponentInfo(id);
        const void* src = from.GetComponentRaw(entity, id);
        void* dst = to.GetComponentRaw(copy, id);
        if (info.serialize != nullptr && info.deserialize != nullptr) {
            bytes.clear();
            info.serialize(src, bytes);
            info.deserialize(dst, bytes.data(), bytes.size());
        } else if (info.trivially_copyable && info.size > 0) {
            std::memcpy(dst, src, info.size);
        }
    }
    return copy;
}

PartitionResult PartitionWorld(const World& world, const PartitionSettings& settings) {
    RegisterStreamingComponents();
    PartitionResult result;
    result.index.settings = settings;
    const std::vector<Entity> entities = AllEntities(world);
    // GUIDs, to follow Parent chains to their roots.
    std::unordered_map<u64, std::vector<std::pair<EntityGuid, Entity>>> by_guid;
    const auto key = [](const EntityGuid& g) { return g.hi ^ (g.lo * 0x9e3779b97f4a7c15ull); };
    for (Entity e : entities)
        if (const IdComponent* id = world.GetComponent<IdComponent>(e)) by_guid[key(id->guid)].push_back({id->guid, e});
    const auto find = [&](const EntityGuid& g) {
        const auto it = by_guid.find(key(g));
        if (it != by_guid.end())
            for (const auto& [guid, e] : it->second)
                if (guid == g) return e;
        return kNullEntity;
    };
    const auto root_of = [&](Entity e) {
        for (int depth = 0; depth < kMaxHierarchyDepth; ++depth) {
            const Parent* p = world.GetComponent<Parent>(e);
            if (p == nullptr) break;
            const Entity up = find(p->parent);
            if (up.IsNull() || up == e) break;
            e = up;
        }
        return e;
    };

    std::map<CellCoord, World> cell_worlds;
    std::map<CellCoord, CellInfo> infos;
    World persistent;
    for (Entity e : entities) {
        const Entity root = root_of(e);
        const Transform* t = world.GetComponent<Transform>(root);
        const AlwaysLoaded* always = world.GetComponent<AlwaysLoaded>(root);
        const bool stays = t == nullptr || (always != nullptr && always->enabled) || world.GetComponent<StreamingSource>(root) != nullptr;
        if (stays) {
            CopyEntity(world, e, persistent);
            ++result.index.persistent_count;
            continue;
        }
        const CellCoord cell = CellOf(t->position, settings);
        CopyEntity(world, e, cell_worlds[cell]);
        CellInfo& info = infos[cell];
        if (info.entity_count == 0) {
            info.coord = cell;
            info.min = info.max = t->position;
        }
        ++info.entity_count;
        info.min = Vec3(std::min(info.min.x, t->position.x), std::min(info.min.y, t->position.y), std::min(info.min.z, t->position.z));
        info.max = Vec3(std::max(info.max.x, t->position.x), std::max(info.max.y, t->position.y), std::max(info.max.z, t->position.z));
    }
    for (auto& [cell, w] : cell_worlds) result.cells[cell] = SaveSceneToMemory(w);
    for (auto& [cell, info] : infos) result.index.cells.push_back(info);
    result.persistent = SaveSceneToMemory(persistent);
    return result;
}

std::filesystem::path CellFile(const std::filesystem::path& dir, const CellCoord& cell) { return dir / "cells" / (CellName(cell) + ".aesc"); }

bool SavePartition(const PartitionResult& p, const std::filesystem::path& dir, std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(dir / "cells", ec);
    if (ec) return Fail(error, "can't make " + (dir / "cells").string() + ": " + ec.message());
    // Old cell files would be loaded as if they were still part of the world.
    for (const auto& f : std::filesystem::directory_iterator(dir / "cells", ec))
        if (f.path().extension() == ".aesc") std::filesystem::remove(f.path(), ec);
    std::ofstream index(dir / "world.aworld");
    if (!index) return Fail(error, "can't write " + (dir / "world.aworld").string());
    index << SaveWorldIndex(p.index).dump(2) << "\n";
    if (!WriteBytes(dir / "persistent.aesc", p.persistent)) return Fail(error, "can't write persistent.aesc");
    for (const auto& [cell, bytes] : p.cells)
        if (!WriteBytes(CellFile(dir, cell), bytes)) return Fail(error, "can't write " + CellFile(dir, cell).string());
    return true;
}

bool LoadWorldIndexFile(const std::filesystem::path& dir, WorldIndex& out, std::string* error) {
    std::ifstream in(dir / "world.aworld");
    if (!in) return Fail(error, "can't open " + (dir / "world.aworld").string());
    std::stringstream ss;
    ss << in.rdbuf();
    const json j = json::parse(ss.str(), nullptr, false);
    if (j.is_discarded()) return Fail(error, "world.aworld isn't JSON");
    return LoadWorldIndex(j, out, error);
}

} // namespace aether::streaming
