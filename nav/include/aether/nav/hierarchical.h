#pragma once

// @stability: experimental
//
// Hierarchical pathfinding (Phase 40 step 2, docs/design/UPGRADE_PLAN.md). A coarse graph is built once over a
// NavMesh's tile borders: a node per walkable polygon edge that crosses into the next tile, edges between the nodes
// of one tile (straight-line cost) and across each border. A query searches that graph for the tiles to cross, then
// asks Detour for each short leg (start -> border -> border -> goal), so a path across a mesh of hundreds of tiles
// never makes Detour search more than two tiles at a time. The result is walkable and near the shortest path, not
// exactly it. Rebuild after tiles change (DynamicNavMesh edits): Build() again, or BuildTile for one tile's nodes.

#include "aether/nav/navmesh.h"

namespace aether::nav {

struct HierarchicalStats {
    usize tiles = 0, portals = 0, edges = 0;
    usize queries = 0, direct = 0;       // queries answered by a plain FindPath (start and goal in or next to one tile)
    usize graph_nodes_expanded = 0;      // by the last query
    usize legs = 0;                      // Detour searches made by the last query
};

class HierarchicalPathfinder {
public:
    // Reads the tile borders of `mesh`, which must stay loaded and outlive this.
    bool Build(const NavMesh& mesh);
    bool Built() const { return mesh_ != nullptr; }

    // Like NavMesh::FindPath. Falls back to it when the graph can't help (no route found, start/goal off the mesh).
    bool FindPath(const Vec3& start, const Vec3& end, NavPath& out, const NavQueryFilter& filter = {}) const;

    const HierarchicalStats& Stats() const { return stats_; }

private:
    struct Portal {
        i32 tile_x = 0, tile_z = 0;   // the tile the portal is in
        i32 to_x = 0, to_z = 0;       // the tile it leads to
        Vec3 point;                   // the middle of the polygon edge on the border
        std::vector<std::pair<u32, f32>> edges; // other portals (index, cost)
    };
    i32 TileOf(const Vec3& p, i32& z) const;

    const NavMesh* mesh_ = nullptr;
    std::vector<Portal> portals_;
    mutable HierarchicalStats stats_;
};

} // namespace aether::nav
