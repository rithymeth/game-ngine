#include "aether/terrain/streaming.h"

#include "aether/core/log.h"
#include "aether/scene/serialization.h"

#include <algorithm>
#include <unordered_set>

namespace aether {
namespace terrain {

namespace {

u64 Pack(CellCoord c) { return (static_cast<u64>(static_cast<u32>(c.x)) << 32) | static_cast<u32>(c.z); }

std::unordered_set<Entity> AliveEntities(const World& world) {
    std::unordered_set<Entity> out;
    world.ForEachArchetype([&](Archetype& archetype) {
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            const Entity* e = archetype.EntityArray(c);
            out.insert(e, e + archetype.ChunkEntityCount(c));
        }
    });
    return out;
}

} // namespace

std::string CellScenePath(const std::string& directory, CellCoord cell) {
    const std::string name = "cell_" + std::to_string(cell.x) + "_" + std::to_string(cell.z) + ".aesc";
    if (directory.empty()) return name;
    const char last = directory.back();
    return (last == '/' || last == '\\') ? directory + name : directory + "/" + name;
}

CellSceneStreamer::CellSceneStreamer(World& world, const WorldPartitionSettings& settings, PathFn path_of, LoadFn load)
    : world_(world), partition_(settings), path_of_(std::move(path_of)), load_(std::move(load)) {
    if (!load_) load_ = [](World& w, const std::string& path) { return LoadScene(w, path); };
}

const std::vector<Entity>& CellSceneStreamer::EntitiesOf(CellCoord cell) const {
    static const std::vector<Entity> kNone;
    auto it = entities_.find(Pack(cell));
    return it == entities_.end() ? kNone : it->second;
}

usize CellSceneStreamer::DestroyCell(CellCoord cell) {
    auto it = entities_.find(Pack(cell));
    if (it == entities_.end()) return 0;
    usize destroyed = 0;
    for (Entity e : it->second) {
        if (!world_.IsAlive(e)) continue; // gameplay may have destroyed it already
        world_.DestroyEntity(e);
        ++destroyed;
    }
    entities_.erase(it);
    return destroyed;
}

CellStreamStats CellSceneStreamer::Update(f64 focus_x, f64 focus_z) {
    CellStreamStats stats;
    const StreamingDelta delta = partition_.Update(focus_x, focus_z);

    for (CellCoord cell : delta.to_unload) {
        if (on_unloaded) on_unloaded(cell);
        stats.entities_destroyed += DestroyCell(cell);
        failed_.erase(std::remove(failed_.begin(), failed_.end(), cell), failed_.end());
        ++stats.cells_unloaded;
    }

    std::unordered_set<Entity> known;
    bool have_known = false;
    for (CellCoord cell : delta.to_load) {
        const std::string path = path_of_ ? path_of_(cell) : std::string();
        if (path.empty()) {
            ++stats.cells_loaded; // nothing to load: an empty cell
            continue;
        }
        if (!have_known) {
            known = AliveEntities(world_);
            have_known = true;
        }
        const bool ok = load_(world_, path);
        // Whatever appeared during the load belongs to this cell, even if it failed part-way.
        std::vector<Entity> created;
        for (Entity e : AliveEntities(world_)) {
            if (known.insert(e).second) created.push_back(e);
        }
        std::sort(created.begin(), created.end(), [](Entity a, Entity b) { return a.index < b.index; });
        if (!ok) {
            AETHER_LOG_WARN("World", "Streaming cell (%d, %d): could not load %s", cell.x, cell.z, path.c_str());
            failed_.push_back(cell);
            ++stats.cells_failed;
        } else {
            ++stats.cells_loaded;
        }
        stats.entities_created += created.size();
        if (!created.empty()) {
            std::vector<Entity>& owned = entities_[Pack(cell)];
            owned = std::move(created);
            if (on_loaded && ok) on_loaded(cell, owned);
        }
    }
    return stats;
}

CellStreamStats CellSceneStreamer::UnloadAll() {
    CellStreamStats stats;
    std::vector<u64> keys;
    for (const auto& [key, list] : entities_) {
        (void)list;
        keys.push_back(key);
    }
    for (u64 key : keys) {
        const CellCoord cell{static_cast<i32>(key >> 32), static_cast<i32>(key & 0xFFFFFFFFu)};
        if (on_unloaded) on_unloaded(cell);
        stats.entities_destroyed += DestroyCell(cell);
    }
    stats.cells_unloaded = partition_.LoadedCount(); // includes cells with no content
    partition_.Reset();
    failed_.clear();
    return stats;
}

void CellSceneStreamer::Retry() {
    for (CellCoord cell : failed_) partition_.Forget(cell);
    failed_.clear();
}

} // namespace terrain
} // namespace aether
