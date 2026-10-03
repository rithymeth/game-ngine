#pragma once

#include "aether/ecs/world.h"
#include "aether/terrain/world.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aether {
namespace terrain {

// Phase 21: scene streaming for large worlds. Each WorldPartition cell has an
// optional scene file; the streamer loads it additively when the cell comes in
// range and destroys exactly the entities it created when the cell goes out.
// Scenes keep their authored (world-space) positions. Entities are destroyed
// directly, so they get no OnDestroy (see scene/lifecycle.h).

struct CellStreamStats {
    usize cells_loaded = 0;
    usize cells_unloaded = 0;
    usize cells_failed = 0;
    usize entities_created = 0;
    usize entities_destroyed = 0;
};

// "<directory>/cell_<x>_<z>.aesc"
std::string CellScenePath(const std::string& directory, CellCoord cell);

class CellSceneStreamer {
public:
    // Scene file for a cell, or empty when the cell has no content.
    using PathFn = std::function<std::string(CellCoord)>;
    // Loads a scene additively into the world; LoadScene by default.
    using LoadFn = std::function<bool(World&, const std::string&)>;

    CellSceneStreamer(World& world, const WorldPartitionSettings& settings, PathFn path_of, LoadFn load = {});

    // Streams for the given focus. A cell whose scene fails to load stays
    // empty and is not retried until Retry().
    CellStreamStats Update(f64 focus_x, f64 focus_z);

    // Destroys everything the streamer loaded and forgets all cells.
    CellStreamStats UnloadAll();

    // Makes failed cells eligible to load again on the next Update.
    void Retry();

    const WorldPartition& Partition() const { return partition_; }
    const std::vector<Entity>& EntitiesOf(CellCoord cell) const;
    const std::vector<CellCoord>& Failed() const { return failed_; }

    std::function<void(CellCoord, const std::vector<Entity>&)> on_loaded;
    std::function<void(CellCoord)> on_unloaded; // called before the cell's entities are destroyed

private:
    usize DestroyCell(CellCoord cell);

    World& world_;
    WorldPartition partition_;
    PathFn path_of_;
    LoadFn load_;
    std::unordered_map<u64, std::vector<Entity>> entities_; // by packed cell
    std::vector<CellCoord> failed_;
};

} // namespace terrain
} // namespace aether
