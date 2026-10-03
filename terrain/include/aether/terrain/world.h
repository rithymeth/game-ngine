#pragma once

#include "aether/core/base.h"
#include "aether/math/vec.h"

#include <unordered_set>
#include <vector>

namespace aether {
namespace terrain {

// Phase 21 step 8: large worlds. Grid-cell streaming (world partition) and a
// floating origin with double-precision world positions. Both are headless;
// the caller loads and unloads the actual scenes for the cells reported here.

struct CellCoord {
    i32 x = 0;
    i32 z = 0;
    bool operator==(const CellCoord& o) const { return x == o.x && z == o.z; }
};

struct CellCoordHash {
    usize operator()(const CellCoord& c) const {
        return static_cast<usize>(static_cast<u32>(c.x)) * 73856093u ^
               static_cast<usize>(static_cast<u32>(c.z)) * 19349663u;
    }
};

struct WorldPartitionSettings {
    f32 cell_size = 256.0f;    // world units per cell side
    f32 load_radius = 512.0f;  // cells within this distance of the focus load
    f32 unload_radius = 640.0f; // loaded cells beyond this unload; > load_radius avoids thrash
    u32 max_loads_per_update = 4; // spreads load work across frames; 0 = unlimited
};

struct StreamingDelta {
    std::vector<CellCoord> to_load;   // nearest first
    std::vector<CellCoord> to_unload; // farthest first
};

class WorldPartition {
public:
    explicit WorldPartition(const WorldPartitionSettings& settings = {}) : settings_(settings) {}

    const WorldPartitionSettings& Settings() const { return settings_; }

    CellCoord CellOf(f64 x, f64 z) const;
    // Axis-aligned footprint of a cell in world units.
    void CellBounds(CellCoord cell, f64& min_x, f64& min_z, f64& max_x, f64& max_z) const;

    // Compares the loaded set against the focus position and returns what to
    // load and unload. Reported loads are treated as loaded immediately.
    StreamingDelta Update(f64 focus_x, f64 focus_z);

    bool IsLoaded(CellCoord cell) const { return loaded_.count(cell) != 0; }
    usize LoadedCount() const { return loaded_.size(); }
    void Reset() { loaded_.clear(); }
    // Drops a cell from the loaded set without reporting an unload, so the next
    // Update loads it again if it is in range (a retry after a failed load).
    void Forget(CellCoord cell) { loaded_.erase(cell); }

private:
    f64 DistanceToCell(CellCoord cell, f64 x, f64 z) const;

    WorldPartitionSettings settings_;
    std::unordered_set<CellCoord, CellCoordHash> loaded_;
};

struct WorldPosition {
    f64 x = 0.0;
    f64 y = 0.0;
    f64 z = 0.0;
};

// Keeps render-space positions small by periodically shifting the origin to
// the focus, so float precision stays high far from the world's zero.
class FloatingOrigin {
public:
    explicit FloatingOrigin(f64 rebase_threshold = 4096.0) : threshold_(rebase_threshold) {}

    const WorldPosition& Origin() const { return origin_; }

    Vec3 ToLocal(const WorldPosition& world) const;
    WorldPosition ToWorld(const Vec3& local) const;

    // Rebases onto `focus` when it drifts beyond the threshold from the origin.
    // Returns true and sets `shift` to the amount local-space objects must move
    // by (the negated origin delta) so they stay put in world space.
    bool Update(const WorldPosition& focus, Vec3& shift);

private:
    f64 threshold_;
    WorldPosition origin_;
};

} // namespace terrain
} // namespace aether
