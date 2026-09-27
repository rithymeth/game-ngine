#pragma once

#include "aether/core/base.h"
#include "aether/math/vec.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

class dtNavMesh;
class dtNavMeshQuery;

namespace aether::nav {

// Navigation meshes (Phase 20 step 1, docs/design/PHASE_SPECS.md §20.1):
// walkable surfaces baked from the scene's triangles with Recast, in
// square tiles (so step 2 can rebuild tiles at runtime), and queried with
// Detour: paths, nearest points, raycasts, random points.

// Area ids: 0 is not walkable, 63 is plain ground; 1..62 are yours (water,
// grass, road) with costs in the query filter.
constexpr u8 kAreaNull = 0;
constexpr u8 kAreaWalkable = 63;

// The triangles to bake from, with an area per triangle.
struct NavGeometry {
    std::vector<Vec3> vertices;
    std::vector<u32> indices; // three per triangle
    std::vector<u8> areas;    // one per triangle

    void AddTriangles(const std::vector<Vec3>& vertices, const std::vector<u32>& indices, u8 area = kAreaWalkable);
    // A flat rectangle facing up at `center` (half sizes on X and Z).
    void AddPlane(const Vec3& center, f32 half_x, f32 half_z, u8 area = kAreaWalkable);
    // A solid box (walls and a walkable top).
    void AddBox(const Vec3& center, const Vec3& half_extents, u8 area = kAreaWalkable);
    usize TriangleCount() const { return indices.size() / 3; }
    bool Bounds(Vec3& min, Vec3& max) const; // false if empty
};

// The agent the mesh is baked for, and how finely.
struct NavMeshSettings {
    f32 cell_size = 0.3f;   // horizontal voxel size (world units)
    f32 cell_height = 0.2f; // vertical voxel size
    f32 agent_height = 2.0f;
    f32 agent_radius = 0.6f;    // the mesh keeps this far from walls
    f32 agent_max_climb = 0.9f; // steps up to this are walkable
    f32 agent_max_slope = 45.0f; // degrees
    i32 region_min_size = 8;     // cells: smaller islands are dropped
    i32 region_merge_size = 20;
    f32 edge_max_length = 12.0f;
    f32 edge_max_error = 1.3f;
    i32 verts_per_poly = 6;
    f32 detail_sample_distance = 6.0f; // cells (below 0.9: no detail mesh)
    f32 detail_sample_max_error = 1.0f;
    i32 tile_size = 48; // cells per tile side (0: one tile over everything)
};

// A baked mesh: Detour's tile data plus what's needed to load it.
struct NavMeshData {
    NavMeshSettings settings;
    Vec3 origin;             // the tile grid's corner
    f32 tile_world_size = 0.0f;
    i32 tiles_x = 0, tiles_z = 0;
    struct Tile {
        i32 x = 0, z = 0;
        std::vector<u8> data; // dtCreateNavMeshData's output
    };
    std::vector<Tile> tiles; // non-empty ones only
};

struct NavBuildStats {
    usize tiles = 0, empty_tiles = 0;
    usize polygons = 0, vertices = 0;
    f64 milliseconds = 0.0;
};

// Bakes `geometry`. False (with the reason) for no geometry or bad settings.
bool BuildNavMesh(const NavGeometry& geometry, const NavMeshSettings& settings, NavMeshData& out, std::string* error = nullptr,
                  NavBuildStats* stats = nullptr);
// Rebakes one tile (x, z) of an existing mesh's grid from `geometry` (step 2 uses it for obstacles).
bool BuildNavTile(const NavGeometry& geometry, const NavMeshData& grid, i32 x, i32 z, NavMeshData::Tile& out, std::string* error = nullptr);

// `.anav` files.
std::vector<u8> SaveNavMesh(const NavMeshData& data);
bool LoadNavMesh(const std::vector<u8>& bytes, NavMeshData& out, std::string* error = nullptr);

// Per-area costs and exclusions for a query (the defaults: every area costs 1).
struct NavQueryFilter {
    std::array<f32, 64> area_costs;
    std::array<bool, 64> excluded;
    NavQueryFilter() {
        area_costs.fill(1.0f);
        excluded.fill(false);
    }
};

enum class PathStatus : u8 {
    Complete, // reaches the goal
    Partial,  // gets as near as it can (the goal is cut off, or too far to search)
    None,     // no start or goal on the mesh
};

struct NavPath {
    PathStatus status = PathStatus::None;
    std::vector<Vec3> points; // corners from the start to the end, on the mesh
    f32 Length() const;
};

// One walkable polygon, for drawing (the editor's overlay).
struct NavPolygon {
    std::vector<Vec3> vertices;
    u8 area = kAreaWalkable;
    i32 tile_x = 0, tile_z = 0;
};

class NavMesh {
public:
    NavMesh();
    ~NavMesh();
    NavMesh(const NavMesh&) = delete;
    NavMesh& operator=(const NavMesh&) = delete;

    bool Load(const NavMeshData& data, std::string* error = nullptr);
    bool Loaded() const { return mesh_ != nullptr; }
    // Swaps one tile's data (a rebuilt tile); empty data removes it.
    bool ReplaceTile(const NavMeshData::Tile& tile, std::string* error = nullptr);

    // The shortest path over the mesh (corners), from the points nearest `start` and `end`.
    bool FindPath(const Vec3& start, const Vec3& end, NavPath& out, const NavQueryFilter& filter = {}) const;
    // The nearest point on the mesh within `extents` (half sizes) of `p`; its area too.
    bool NearestPoint(const Vec3& p, Vec3& out, const Vec3& extents = Vec3(2, 4, 2), u8* area = nullptr) const;
    // Walks a straight line over the mesh: true when a wall (or the mesh's edge) stops it
    // before `end`, with where and the wall's normal.
    bool Raycast(const Vec3& start, const Vec3& end, Vec3& hit, Vec3* normal = nullptr, const NavQueryFilter& filter = {}) const;
    // A random point on the mesh (by area), or within `radius` of `center` over connected ground.
    bool RandomPoint(u64 seed, Vec3& out, const NavQueryFilter& filter = {}) const;
    bool RandomPointNear(const Vec3& center, f32 radius, u64 seed, Vec3& out, const NavQueryFilter& filter = {}) const;
    // Whether a full path joins the two.
    bool Reachable(const Vec3& a, const Vec3& b, const NavQueryFilter& filter = {}) const;

    usize TileCount() const;
    usize PolygonCount() const;
    std::vector<NavPolygon> Polygons() const;
    const NavMeshData& Data() const { return data_; }
    dtNavMesh* Detour() const { return mesh_; }         // for crowds (step 3)
    dtNavMeshQuery* Query() const { return query_; }

private:
    void Free();
    dtNavMesh* mesh_ = nullptr;
    dtNavMeshQuery* query_ = nullptr;
    NavMeshData data_;
};

} // namespace aether::nav
