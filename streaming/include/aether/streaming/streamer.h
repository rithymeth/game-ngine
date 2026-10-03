#pragma once

#include "aether/scene/entity_guid.h"
#include "aether/streaming/partition.h"

#include <functional>
#include <set>

namespace aether::streaming {

// A position in the whole world, in double precision (float positions lose
// centimetres beyond ~100 km; the floating origin keeps Transforms small).
struct WorldPosition {
    f64 x = 0.0, y = 0.0, z = 0.0;
};

enum class CellState : u8 { Unloaded, Loaded, Failed };

struct StreamingEvent {
    CellCoord cell;
    bool loaded = true; // false: unloaded
    usize entities = 0;
};

// Streams a partitioned world's cells in and out around its StreamingSource
// entities (Phase 21 step 6): a cell loads once it's within a source's
// radius, and unloads once it's beyond every source's radius x
// `unload_margin` (so standing on a border doesn't thrash), at most
// `max_loads_per_update` / `max_unloads_per_update` each update. Loading a
// cell adds its entities to the world (indexing their GUIDs); unloading
// destroys the entities it added that still exist. Changes made to them in
// between aren't kept. Positions are local to the floating origin.
class WorldStreamer {
public:
    // The cell's scene bytes; false when there's none.
    using CellLoader = std::function<bool(const CellCoord& cell, std::vector<u8>& bytes)>;
    WorldStreamer(World& world, WorldIndex index, CellLoader loader, GuidIndex* guids = nullptr);
    // Cells from files saved by SavePartition.
    static CellLoader FileLoader(std::filesystem::path dir);

    void Update();

    usize max_loads_per_update = 2;
    usize max_unloads_per_update = 4;
    f32 unload_margin = 1.25f;

    CellState State(const CellCoord& cell) const;
    std::vector<CellCoord> LoadedCells() const;
    const std::vector<Entity>& EntitiesIn(const CellCoord& cell) const;
    usize PendingLoads() const { return pending_.size(); }
    // Keeps a cell loaded whatever the sources (a level's start area, a cutscene).
    void Pin(const CellCoord& cell);
    void Unpin(const CellCoord& cell);
    const std::vector<StreamingEvent>& Events() const { return events_; }
    const std::vector<std::string>& Problems() const { return problems_; }
    const WorldIndex& Index() const { return index_; }

    // The floating origin's offset: a Transform at p is at origin + p in the whole world.
    void SetOriginOffset(const WorldPosition& offset) { offset_ = offset; }
    const WorldPosition& OriginOffset() const { return offset_; }

private:
    bool LoadCell(const CellCoord& cell);
    void UnloadCell(const CellCoord& cell);
    f32 DistanceToCell(const Vec3& local, const CellCoord& cell) const;

    World& world_;
    WorldIndex index_;
    CellLoader loader_;
    GuidIndex* guids_;
    WorldPosition offset_;
    std::map<CellCoord, std::vector<Entity>> loaded_;
    std::map<CellCoord, CellState> failed_;
    std::set<CellCoord> pinned_;
    std::vector<CellCoord> pending_;
    std::vector<StreamingEvent> events_;
    std::vector<std::string> problems_;
};

// Moves everything back near (0, 0, 0) when `focus` strays more than
// `threshold` from it on the ground plane: root Transforms shift by a whole
// number of `step`s, and the offset (the whole-world position of the local
// origin) grows by as much. True when it shifted.
struct FloatingOrigin {
    f32 threshold = 2048.0f;
    f32 step = 1024.0f;
    WorldPosition offset;
    bool Update(World& world, const Vec3& focus, const GuidIndex* guids = nullptr);
    Vec3 last_shift; // the last shift applied (subtracted from Transforms)
    WorldPosition ToWorld(const Vec3& local) const { return {offset.x + local.x, offset.y + local.y, offset.z + local.z}; }
    Vec3 ToLocal(const WorldPosition& p) const {
        return Vec3(static_cast<f32>(p.x - offset.x), static_cast<f32>(p.y - offset.y), static_cast<f32>(p.z - offset.z));
    }
};

} // namespace aether::streaming
