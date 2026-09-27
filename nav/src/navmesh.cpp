#include "aether/nav/navmesh.h"

#include <DetourAlloc.h>
#include <DetourCommon.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Recast.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

namespace aether::nav {

namespace {

constexpr u16 kWalkFlag = 1;
constexpr i32 kMaxPathPolys = 512;

bool Fail(std::string* error, const std::string& m) {
    if (error != nullptr) *error = m;
    return false;
}

// A Recast context that keeps the first error.
class Context final : public rcContext {
public:
    Context() : rcContext(true) {}
    std::string first_error;

protected:
    void doLog(const rcLogCategory category, const char* msg, const int len) override {
        if (category == RC_LOG_ERROR && first_error.empty()) first_error.assign(msg, static_cast<usize>(len));
    }
};

template <typename T, void (*Free)(T*)>
struct Owned {
    T* p = nullptr;
    ~Owned() {
        if (p != nullptr) Free(p);
    }
};

bool Settle(const NavMeshSettings& s, std::string* error) {
    if (s.cell_size <= 0.0f || s.cell_height <= 0.0f) return Fail(error, "cell sizes must be more than 0");
    if (s.agent_height <= 0.0f || s.agent_radius < 0.0f || s.agent_max_climb < 0.0f) return Fail(error, "the agent's height must be more than 0, and its radius and climb 0 or more");
    if (s.verts_per_poly < 3 || s.verts_per_poly > DT_VERTS_PER_POLYGON) return Fail(error, "verts_per_poly must be 3 to 6");
    if (s.tile_size < 0) return Fail(error, "tile_size can't be negative");
    return true;
}

f32 Frand(u64& state) {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<f32>((state >> 40) & 0xFFFFFF) / 16777216.0f;
}
thread_local u64 g_rand_state = 1;
float DetourRand() { return Frand(g_rand_state); }

// A filter with per-area costs that also refuses excluded areas outright.
class AreaFilter final : public dtQueryFilter {
public:
    explicit AreaFilter(const NavQueryFilter& f) : excluded_(f.excluded) {
        setIncludeFlags(0xFFFF);
        setExcludeFlags(0);
        for (int i = 0; i < 64; ++i) setAreaCost(i, std::max(f.area_costs[static_cast<usize>(i)], 0.0f));
    }
    bool passFilter(const dtPolyRef ref, const dtMeshTile* tile, const dtPoly* poly) const override {
        return !excluded_[poly->getArea()] && dtQueryFilter::passFilter(ref, tile, poly);
    }

private:
    std::array<bool, 64> excluded_;
};

} // namespace

// --- Geometry ------------------------------------------------------------------------------------------

void NavGeometry::AddTriangles(const std::vector<Vec3>& v, const std::vector<u32>& idx, u8 area) {
    const u32 base = static_cast<u32>(vertices.size());
    vertices.insert(vertices.end(), v.begin(), v.end());
    for (u32 i : idx) indices.push_back(base + i);
    areas.insert(areas.end(), idx.size() / 3, area);
}

void NavGeometry::AddPlane(const Vec3& c, f32 hx, f32 hz, u8 area) {
    // Counter-clockwise seen from above (Recast's up-facing winding).
    AddTriangles({{c.x - hx, c.y, c.z - hz}, {c.x + hx, c.y, c.z - hz}, {c.x + hx, c.y, c.z + hz}, {c.x - hx, c.y, c.z + hz}}, {0, 2, 1, 0, 3, 2}, area);
}

void NavGeometry::AddBox(const Vec3& c, const Vec3& h, u8 area) {
    std::vector<Vec3> v;
    for (int i = 0; i < 8; ++i) v.push_back({c.x + (i & 1 ? h.x : -h.x), c.y + (i & 2 ? h.y : -h.y), c.z + (i & 4 ? h.z : -h.z)});
    // Outward-facing faces: -Y, +Y, -X, +X, -Z, +Z.
    AddTriangles(v, {0, 1, 5, 0, 5, 4, 2, 7, 3, 2, 6, 7, 0, 6, 2, 0, 4, 6, 1, 3, 7, 1, 7, 5, 0, 2, 3, 0, 3, 1, 4, 5, 7, 4, 7, 6}, area);
}

NavVolume NavVolume::Box(const Vec3& c, const Vec3& h, u8 area, f32 yaw) {
    NavVolume v;
    v.shape = Shape::Box, v.center = c, v.half_extents = h, v.area = area, v.yaw_degrees = yaw;
    return v;
}

NavVolume NavVolume::Cylinder(const Vec3& c, f32 r, f32 h, u8 area) {
    NavVolume v;
    v.shape = Shape::Cylinder, v.center = c, v.radius = r, v.height = h, v.area = area;
    return v;
}

NavVolume NavVolume::Prism(const std::vector<Vec3>& pts, f32 lo, f32 hi, u8 area) {
    NavVolume v;
    v.shape = Shape::Prism, v.points = pts, v.min_y = lo, v.max_y = hi, v.area = area;
    return v;
}

namespace {
// A box's corners on the ground plane, turned by its yaw.
std::array<Vec3, 4> BoxCorners(const NavVolume& v) {
    const f32 a = v.yaw_degrees * 3.14159265f / 180.0f, c = std::cos(a), s = std::sin(a);
    std::array<Vec3, 4> out;
    const f32 sx[4] = {-1, 1, 1, -1}, sz[4] = {-1, -1, 1, 1};
    for (int i = 0; i < 4; ++i) {
        const f32 x = sx[i] * v.half_extents.x, z = sz[i] * v.half_extents.z;
        out[static_cast<usize>(i)] = Vec3(v.center.x + x * c + z * s, v.center.y, v.center.z - x * s + z * c);
    }
    return out;
}
} // namespace

void NavVolume::Bounds(Vec3& lo, Vec3& hi) const {
    switch (shape) {
    case Shape::Box: {
        lo = hi = center;
        for (const Vec3& p : BoxCorners(*this)) {
            lo = Vec3(std::min(lo.x, p.x), 0, std::min(lo.z, p.z));
            hi = Vec3(std::max(hi.x, p.x), 0, std::max(hi.z, p.z));
        }
        lo.y = center.y - half_extents.y;
        hi.y = center.y + half_extents.y;
        break;
    }
    case Shape::Cylinder:
        lo = Vec3(center.x - radius, center.y - height * 0.5f, center.z - radius);
        hi = Vec3(center.x + radius, center.y + height * 0.5f, center.z + radius);
        break;
    case Shape::Prism:
        lo = Vec3(std::numeric_limits<f32>::max(), min_y, std::numeric_limits<f32>::max());
        hi = Vec3(std::numeric_limits<f32>::lowest(), max_y, std::numeric_limits<f32>::lowest());
        for (const Vec3& p : points) {
            lo.x = std::min(lo.x, p.x), lo.z = std::min(lo.z, p.z);
            hi.x = std::max(hi.x, p.x), hi.z = std::max(hi.z, p.z);
        }
        if (points.empty()) lo = hi = Vec3(0, min_y, 0);
        break;
    }
}

bool NavGeometry::Bounds(Vec3& lo, Vec3& hi) const {
    if (vertices.empty()) return false;
    lo = hi = vertices[0];
    for (const Vec3& p : vertices) {
        lo = Vec3(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
        hi = Vec3(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
    }
    return true;
}

// --- Baking --------------------------------------------------------------------------------------------

namespace {
void MarkVolume(rcContext& ctx, const NavVolume& v, rcCompactHeightfield& chf) {
    switch (v.shape) {
    case NavVolume::Shape::Box: {
        float verts[12];
        const auto corners = BoxCorners(v);
        for (usize i = 0; i < 4; ++i) verts[i * 3] = corners[i].x, verts[i * 3 + 1] = corners[i].y, verts[i * 3 + 2] = corners[i].z;
        rcMarkConvexPolyArea(&ctx, verts, 4, v.center.y - v.half_extents.y, v.center.y + v.half_extents.y, v.area, chf);
        break;
    }
    case NavVolume::Shape::Cylinder: {
        const float base[3] = {v.center.x, v.center.y - v.height * 0.5f, v.center.z};
        rcMarkCylinderArea(&ctx, base, v.radius, v.height, v.area, chf);
        break;
    }
    case NavVolume::Shape::Prism: {
        if (v.points.size() < 3) break;
        std::vector<float> verts;
        for (const Vec3& p : v.points) verts.insert(verts.end(), {p.x, v.min_y, p.z});
        rcMarkConvexPolyArea(&ctx, verts.data(), static_cast<int>(v.points.size()), v.min_y, v.max_y, v.area, chf);
        break;
    }
    }
}

i32 BorderCells(const NavMeshSettings& s) { return static_cast<i32>(std::ceil(s.agent_radius / s.cell_size)) + 3; }
} // namespace

std::vector<std::pair<i32, i32>> NavTilesTouching(const NavMeshData& grid, const Vec3& lo, const Vec3& hi) {
    std::vector<std::pair<i32, i32>> out;
    if (grid.tile_world_size <= 0.0f) return out;
    const f32 border = static_cast<f32>(BorderCells(grid.settings)) * grid.settings.cell_size;
    const auto cell = [&](f32 v, f32 origin) { return static_cast<i32>(std::floor((v - origin) / grid.tile_world_size)); };
    const i32 x0 = std::max(0, cell(lo.x - border, grid.origin.x)), x1 = std::min(grid.tiles_x - 1, cell(hi.x + border, grid.origin.x));
    const i32 z0 = std::max(0, cell(lo.z - border, grid.origin.z)), z1 = std::min(grid.tiles_z - 1, cell(hi.z + border, grid.origin.z));
    for (i32 z = z0; z <= z1; ++z)
        for (i32 x = x0; x <= x1; ++x) out.emplace_back(x, z);
    return out;
}

bool BuildNavTile(const NavGeometry& g, const NavMeshData& grid, i32 tx, i32 tz, NavMeshData::Tile& out, std::string* error,
                  const std::vector<NavVolume>* extra_volumes, const std::vector<NavLink>* extra_links) {
    const NavMeshSettings& s = grid.settings;
    out = {tx, tz, {}};
    Vec3 gmin, gmax;
    if (!g.Bounds(gmin, gmax)) return Fail(error, "no geometry to build from");
    rcConfig cfg{};
    cfg.cs = s.cell_size;
    cfg.ch = s.cell_height;
    cfg.walkableSlopeAngle = s.agent_max_slope;
    cfg.walkableHeight = static_cast<int>(std::ceil(s.agent_height / cfg.ch));
    cfg.walkableClimb = static_cast<int>(std::floor(s.agent_max_climb / cfg.ch));
    cfg.walkableRadius = static_cast<int>(std::ceil(s.agent_radius / cfg.cs));
    cfg.maxEdgeLen = static_cast<int>(s.edge_max_length / cfg.cs);
    cfg.maxSimplificationError = s.edge_max_error;
    cfg.minRegionArea = s.region_min_size * s.region_min_size;
    cfg.mergeRegionArea = s.region_merge_size * s.region_merge_size;
    cfg.maxVertsPerPoly = s.verts_per_poly;
    cfg.tileSize = static_cast<int>(std::lround(grid.tile_world_size / cfg.cs));
    cfg.borderSize = BorderCells(s);
    cfg.width = cfg.tileSize + cfg.borderSize * 2;
    cfg.height = cfg.tileSize + cfg.borderSize * 2;
    cfg.detailSampleDist = s.detail_sample_distance < 0.9f ? 0.0f : cfg.cs * s.detail_sample_distance;
    cfg.detailSampleMaxError = cfg.ch * s.detail_sample_max_error;
    cfg.bmin[0] = grid.origin.x + static_cast<f32>(tx) * grid.tile_world_size - static_cast<f32>(cfg.borderSize) * cfg.cs;
    cfg.bmin[1] = gmin.y - 1.0f;
    cfg.bmin[2] = grid.origin.z + static_cast<f32>(tz) * grid.tile_world_size - static_cast<f32>(cfg.borderSize) * cfg.cs;
    cfg.bmax[0] = cfg.bmin[0] + static_cast<f32>(cfg.width) * cfg.cs;
    cfg.bmax[1] = gmax.y + s.agent_height + 1.0f;
    cfg.bmax[2] = cfg.bmin[2] + static_cast<f32>(cfg.height) * cfg.cs;

    Context ctx;
    Owned<rcHeightfield, rcFreeHeightField> hf{rcAllocHeightfield()};
    if (!rcCreateHeightfield(&ctx, *hf.p, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) return Fail(error, "out of memory for the heightfield");
    // The tile's triangles (those overlapping it), marked walkable by slope, keeping their areas.
    std::vector<float> verts(g.vertices.size() * 3);
    for (usize i = 0; i < g.vertices.size(); ++i) verts[i * 3] = g.vertices[i].x, verts[i * 3 + 1] = g.vertices[i].y, verts[i * 3 + 2] = g.vertices[i].z;
    std::vector<int> tris;
    std::vector<u8> areas;
    for (usize t = 0; t < g.TriangleCount(); ++t) {
        const Vec3& a = g.vertices[g.indices[t * 3]];
        const Vec3& b = g.vertices[g.indices[t * 3 + 1]];
        const Vec3& c = g.vertices[g.indices[t * 3 + 2]];
        if (std::max({a.x, b.x, c.x}) < cfg.bmin[0] || std::min({a.x, b.x, c.x}) > cfg.bmax[0] || std::max({a.z, b.z, c.z}) < cfg.bmin[2] ||
            std::min({a.z, b.z, c.z}) > cfg.bmax[2]) {
            continue;
        }
        tris.insert(tris.end(), {static_cast<int>(g.indices[t * 3]), static_cast<int>(g.indices[t * 3 + 1]), static_cast<int>(g.indices[t * 3 + 2])});
        areas.push_back(t < g.areas.size() ? g.areas[t] : kAreaWalkable);
    }
    const int ntris = static_cast<int>(tris.size() / 3);
    if (ntris == 0) return true; // an empty tile
    std::vector<u8> walkable(static_cast<usize>(ntris), 0);
    rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, verts.data(), static_cast<int>(g.vertices.size()), tris.data(), ntris, walkable.data());
    for (int t = 0; t < ntris; ++t) walkable[static_cast<usize>(t)] = walkable[static_cast<usize>(t)] != 0 ? areas[static_cast<usize>(t)] : kAreaNull;
    if (!rcRasterizeTriangles(&ctx, verts.data(), static_cast<int>(g.vertices.size()), tris.data(), walkable.data(), ntris, *hf.p, cfg.walkableClimb)) {
        return Fail(error, "rasterizing failed");
    }
    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *hf.p);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf.p);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *hf.p);
    Owned<rcCompactHeightfield, rcFreeCompactHeightfield> chf{rcAllocCompactHeightfield()};
    if (!rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf.p, *chf.p)) return Fail(error, "building the compact heightfield failed");
    // Obstacles carve before eroding, so the agent's radius is kept off them too; areas are marked after.
    const auto each_volume = [&](auto&& fn) {
        for (const NavVolume& v : g.volumes) fn(v);
        if (extra_volumes != nullptr)
            for (const NavVolume& v : *extra_volumes) fn(v);
    };
    each_volume([&](const NavVolume& v) {
        if (v.area == kAreaNull) MarkVolume(ctx, v, *chf.p);
    });
    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf.p)) return Fail(error, "eroding the walkable area failed");
    each_volume([&](const NavVolume& v) {
        if (v.area != kAreaNull) MarkVolume(ctx, v, *chf.p);
    });
    if (!rcBuildDistanceField(&ctx, *chf.p)) return Fail(error, "building the distance field failed");
    if (!rcBuildRegions(&ctx, *chf.p, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea)) return Fail(error, "building regions failed");
    Owned<rcContourSet, rcFreeContourSet> cset{rcAllocContourSet()};
    if (!rcBuildContours(&ctx, *chf.p, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset.p)) return Fail(error, "building contours failed");
    if (cset.p->nconts == 0) return true;
    Owned<rcPolyMesh, rcFreePolyMesh> pmesh{rcAllocPolyMesh()};
    if (!rcBuildPolyMesh(&ctx, *cset.p, cfg.maxVertsPerPoly, *pmesh.p)) return Fail(error, "building polygons failed: " + ctx.first_error);
    Owned<rcPolyMeshDetail, rcFreePolyMeshDetail> dmesh{rcAllocPolyMeshDetail()};
    if (!rcBuildPolyMeshDetail(&ctx, *pmesh.p, *chf.p, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh.p)) return Fail(error, "building the detail mesh failed");
    if (pmesh.p->npolys == 0) return true;
    for (int i = 0; i < pmesh.p->npolys; ++i) pmesh.p->flags[i] = pmesh.p->areas[i] != kAreaNull ? kWalkFlag : 0;

    // The links that start in this tile (not its border, so each is in one tile only).
    std::vector<float> link_verts, link_radii;
    std::vector<u16> link_flags;
    std::vector<u8> link_areas, link_dirs;
    std::vector<u32> link_ids;
    const f32 x0 = grid.origin.x + static_cast<f32>(tx) * grid.tile_world_size, z0 = grid.origin.z + static_cast<f32>(tz) * grid.tile_world_size;
    const auto add_link = [&](const NavLink& l) {
        if (l.start.x < x0 || l.start.x >= x0 + grid.tile_world_size || l.start.z < z0 || l.start.z >= z0 + grid.tile_world_size) return;
        link_verts.insert(link_verts.end(), {l.start.x, l.start.y, l.start.z, l.end.x, l.end.y, l.end.z});
        link_radii.push_back(l.radius);
        link_flags.push_back(kWalkFlag);
        link_areas.push_back(l.area);
        link_dirs.push_back(l.bidirectional ? DT_OFFMESH_CON_BIDIR : 0);
        link_ids.push_back(l.user_id);
    };
    for (const NavLink& l : g.links) add_link(l);
    if (extra_links != nullptr)
        for (const NavLink& l : *extra_links) add_link(l);

    dtNavMeshCreateParams params{};
    if (!link_radii.empty()) {
        params.offMeshConVerts = link_verts.data();
        params.offMeshConRad = link_radii.data();
        params.offMeshConFlags = link_flags.data();
        params.offMeshConAreas = link_areas.data();
        params.offMeshConDir = link_dirs.data();
        params.offMeshConUserID = link_ids.data();
        params.offMeshConCount = static_cast<int>(link_radii.size());
    }
    params.verts = pmesh.p->verts;
    params.vertCount = pmesh.p->nverts;
    params.polys = pmesh.p->polys;
    params.polyAreas = pmesh.p->areas;
    params.polyFlags = pmesh.p->flags;
    params.polyCount = pmesh.p->npolys;
    params.nvp = pmesh.p->nvp;
    params.detailMeshes = dmesh.p->meshes;
    params.detailVerts = dmesh.p->verts;
    params.detailVertsCount = dmesh.p->nverts;
    params.detailTris = dmesh.p->tris;
    params.detailTriCount = dmesh.p->ntris;
    params.walkableHeight = s.agent_height;
    params.walkableRadius = s.agent_radius;
    params.walkableClimb = s.agent_max_climb;
    params.tileX = tx;
    params.tileY = tz;
    params.tileLayer = 0;
    rcVcopy(params.bmin, pmesh.p->bmin);
    rcVcopy(params.bmax, pmesh.p->bmax);
    params.cs = cfg.cs;
    params.ch = cfg.ch;
    params.buildBvTree = true;
    unsigned char* data = nullptr;
    int size = 0;
    if (!dtCreateNavMeshData(&params, &data, &size)) return Fail(error, "Detour couldn't make the tile's data");
    out.data.assign(data, data + size);
    dtFree(data);
    return true;
}

bool BuildNavMesh(const NavGeometry& g, const NavMeshSettings& s, NavMeshData& out, std::string* error, NavBuildStats* stats) {
    const auto t0 = std::chrono::steady_clock::now();
    if (!Settle(s, error)) return false;
    Vec3 lo, hi;
    if (!g.Bounds(lo, hi) || g.TriangleCount() == 0) return Fail(error, "no geometry to build from");
    NavMeshData data;
    data.settings = s;
    data.origin = lo;
    const f32 extent = std::max(hi.x - lo.x, hi.z - lo.z);
    data.tile_world_size = s.tile_size > 0 ? static_cast<f32>(s.tile_size) * s.cell_size : std::ceil(extent / s.cell_size + 1.0f) * s.cell_size;
    data.tiles_x = std::max(1, static_cast<i32>(std::ceil((hi.x - lo.x) / data.tile_world_size)));
    data.tiles_z = std::max(1, static_cast<i32>(std::ceil((hi.z - lo.z) / data.tile_world_size)));
    if (static_cast<i64>(data.tiles_x) * data.tiles_z > (1 << 16)) return Fail(error, "too many tiles: make tile_size bigger");
    NavBuildStats st;
    for (i32 z = 0; z < data.tiles_z; ++z) {
        for (i32 x = 0; x < data.tiles_x; ++x) {
            NavMeshData::Tile tile;
            if (!BuildNavTile(g, data, x, z, tile, error)) return false;
            if (tile.data.empty()) {
                ++st.empty_tiles;
                continue;
            }
            const auto* header = reinterpret_cast<const dtMeshHeader*>(tile.data.data());
            st.polygons += static_cast<usize>(header->polyCount);
            st.vertices += static_cast<usize>(header->vertCount);
            data.tiles.push_back(std::move(tile));
        }
    }
    st.tiles = data.tiles.size();
    st.milliseconds = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (data.tiles.empty()) return Fail(error, "nothing walkable: check the agent's settings and the geometry's slope");
    out = std::move(data);
    if (stats != nullptr) *stats = st;
    return true;
}

// --- Files ---------------------------------------------------------------------------------------------

namespace {
constexpr u32 kMagic = 0x56414E41; // "ANAV"
constexpr u32 kVersion = 1;
template <typename T>
void Put(std::vector<u8>& b, const T& v) {
    const auto* p = reinterpret_cast<const u8*>(&v);
    b.insert(b.end(), p, p + sizeof(T));
}
template <typename T>
bool Get(const std::vector<u8>& b, usize& at, T& v) {
    if (at + sizeof(T) > b.size()) return false;
    std::memcpy(&v, b.data() + at, sizeof(T));
    at += sizeof(T);
    return true;
}
} // namespace

std::vector<u8> SaveNavMesh(const NavMeshData& d) {
    std::vector<u8> b;
    Put(b, kMagic), Put(b, kVersion), Put(b, d.settings), Put(b, d.origin.x), Put(b, d.origin.y), Put(b, d.origin.z);
    Put(b, d.tile_world_size), Put(b, d.tiles_x), Put(b, d.tiles_z), Put(b, static_cast<u32>(d.tiles.size()));
    for (const auto& t : d.tiles) {
        Put(b, t.x), Put(b, t.z), Put(b, static_cast<u32>(t.data.size()));
        b.insert(b.end(), t.data.begin(), t.data.end());
    }
    return b;
}

bool LoadNavMesh(const std::vector<u8>& b, NavMeshData& out, std::string* error) {
    usize at = 0;
    u32 magic = 0, version = 0, count = 0;
    NavMeshData d;
    if (!Get(b, at, magic) || magic != kMagic) return Fail(error, "not a navigation mesh");
    if (!Get(b, at, version) || version > kVersion) return Fail(error, "made by a newer version");
    if (!Get(b, at, d.settings) || !Get(b, at, d.origin.x) || !Get(b, at, d.origin.y) || !Get(b, at, d.origin.z) || !Get(b, at, d.tile_world_size) ||
        !Get(b, at, d.tiles_x) || !Get(b, at, d.tiles_z) || !Get(b, at, count)) {
        return Fail(error, "the file is cut short");
    }
    for (u32 i = 0; i < count; ++i) {
        NavMeshData::Tile t;
        u32 size = 0;
        if (!Get(b, at, t.x) || !Get(b, at, t.z) || !Get(b, at, size) || at + size > b.size()) return Fail(error, "the file is cut short");
        t.data.assign(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + size));
        at += size;
        d.tiles.push_back(std::move(t));
    }
    out = std::move(d);
    return true;
}

// --- Queries -------------------------------------------------------------------------------------------

f32 NavPath::Length() const {
    f32 len = 0.0f;
    for (usize i = 1; i < points.size(); ++i) len += (points[i] - points[i - 1]).Length();
    return len;
}

NavMesh::NavMesh() = default;
NavMesh::~NavMesh() { Free(); }

void NavMesh::Free() {
    if (query_ != nullptr) dtFreeNavMeshQuery(query_);
    if (mesh_ != nullptr) dtFreeNavMesh(mesh_);
    query_ = nullptr;
    mesh_ = nullptr;
}

void NavMesh::Unload() {
    Free();
    data_ = {};
}

bool NavMesh::Load(const NavMeshData& data, std::string* error) {
    Free();
    dtNavMeshParams params{};
    params.orig[0] = data.origin.x, params.orig[1] = data.origin.y, params.orig[2] = data.origin.z;
    params.tileWidth = params.tileHeight = data.tile_world_size;
    const u32 tiles = static_cast<u32>(std::max(1, data.tiles_x * data.tiles_z));
    int tile_bits = 0;
    while ((1u << tile_bits) < tiles) ++tile_bits;
    tile_bits = std::min(tile_bits, 14);
    params.maxTiles = 1 << tile_bits;
    params.maxPolys = 1 << (22 - tile_bits);
    mesh_ = dtAllocNavMesh();
    if (mesh_ == nullptr || dtStatusFailed(mesh_->init(&params))) {
        Free();
        return Fail(error, "Detour couldn't set up the mesh");
    }
    data_ = data;
    for (const auto& t : data.tiles) {
        if (!ReplaceTile(t, error)) {
            Free();
            return false;
        }
    }
    query_ = dtAllocNavMeshQuery();
    if (query_ == nullptr || dtStatusFailed(query_->init(mesh_, 4096))) {
        Free();
        return Fail(error, "Detour couldn't set up queries");
    }
    return true;
}

bool NavMesh::ReplaceTile(const NavMeshData::Tile& t, std::string* error) {
    if (mesh_ == nullptr) return Fail(error, "no mesh loaded");
    if (const dtTileRef old = mesh_->getTileRefAt(t.x, t.z, 0); old != 0) mesh_->removeTile(old, nullptr, nullptr);
    // Keep our copy of the data current (so saving after a rebuild saves it).
    auto it = std::find_if(data_.tiles.begin(), data_.tiles.end(), [&](const NavMeshData::Tile& x) { return x.x == t.x && x.z == t.z; });
    if (t.data.empty()) {
        if (it != data_.tiles.end()) data_.tiles.erase(it);
        return true;
    }
    if (it != data_.tiles.end()) *it = t;
    else if (&t < data_.tiles.data() || &t >= data_.tiles.data() + data_.tiles.size()) data_.tiles.push_back(t);
    auto* copy = static_cast<unsigned char*>(dtAlloc(t.data.size(), DT_ALLOC_PERM));
    std::memcpy(copy, t.data.data(), t.data.size());
    if (dtStatusFailed(mesh_->addTile(copy, static_cast<int>(t.data.size()), DT_TILE_FREE_DATA, 0, nullptr))) {
        dtFree(copy);
        return Fail(error, "Detour refused a tile");
    }
    return true;
}

bool NavMesh::NearestPoint(const Vec3& p, Vec3& out, const Vec3& extents, u8* area) const {
    if (query_ == nullptr) return false;
    dtQueryFilter filter;
    const float c[3] = {p.x, p.y, p.z}, e[3] = {extents.x, extents.y, extents.z};
    float nearest[3];
    dtPolyRef ref = 0;
    if (dtStatusFailed(query_->findNearestPoly(c, e, &filter, &ref, nearest)) || ref == 0) return false;
    out = Vec3(nearest[0], nearest[1], nearest[2]);
    if (area != nullptr) {
        const dtMeshTile* tile = nullptr;
        const dtPoly* poly = nullptr;
        mesh_->getTileAndPolyByRefUnsafe(ref, &tile, &poly);
        *area = poly->getArea();
    }
    return true;
}

bool NavMesh::FindPath(const Vec3& start, const Vec3& end, NavPath& out, const NavQueryFilter& f) const {
    out = {};
    if (query_ == nullptr) return false;
    const AreaFilter filter(f);
    const float ext[3] = {2.0f, 4.0f, 2.0f};
    const float sp[3] = {start.x, start.y, start.z}, ep[3] = {end.x, end.y, end.z};
    float s[3], e[3];
    dtPolyRef sref = 0, eref = 0;
    query_->findNearestPoly(sp, ext, &filter, &sref, s);
    query_->findNearestPoly(ep, ext, &filter, &eref, e);
    if (sref == 0 || eref == 0) return false;
    dtPolyRef polys[kMaxPathPolys];
    int npolys = 0;
    const dtStatus st = query_->findPath(sref, eref, s, e, &filter, polys, &npolys, kMaxPathPolys);
    if (dtStatusFailed(st) || npolys == 0) return false;
    float target[3] = {e[0], e[1], e[2]};
    bool partial = polys[npolys - 1] != eref || dtStatusDetail(st, DT_PARTIAL_RESULT);
    if (polys[npolys - 1] != eref) {
        // As near as it gets: the closest point on the last polygon.
        query_->closestPointOnPoly(polys[npolys - 1], e, target, nullptr);
    }
    float straight[kMaxPathPolys * 3];
    unsigned char straight_flags[kMaxPathPolys];
    int nstraight = 0;
    if (dtStatusFailed(query_->findStraightPath(s, target, polys, npolys, straight, straight_flags, nullptr, &nstraight, kMaxPathPolys))) return false;
    for (int i = 0; i < nstraight; ++i) {
        out.points.push_back(Vec3(straight[i * 3], straight[i * 3 + 1], straight[i * 3 + 2]));
        out.flags.push_back((straight_flags[i] & DT_STRAIGHTPATH_OFFMESH_CONNECTION) != 0 ? kNavPointLinkStart : 0);
    }
    out.status = partial ? PathStatus::Partial : PathStatus::Complete;
    return true;
}

bool NavMesh::Reachable(const Vec3& a, const Vec3& b, const NavQueryFilter& f) const {
    NavPath p;
    return FindPath(a, b, p, f) && p.status == PathStatus::Complete;
}

bool NavMesh::Raycast(const Vec3& start, const Vec3& end, Vec3& hit, Vec3* normal, const NavQueryFilter& f) const {
    if (query_ == nullptr) return false;
    const AreaFilter filter(f);
    const float ext[3] = {2.0f, 4.0f, 2.0f};
    const float sp[3] = {start.x, start.y, start.z}, ep[3] = {end.x, end.y, end.z};
    float s[3];
    dtPolyRef sref = 0;
    query_->findNearestPoly(sp, ext, &filter, &sref, s);
    if (sref == 0) return false;
    float t = 0.0f, n[3] = {0, 0, 0};
    dtPolyRef polys[kMaxPathPolys];
    int npolys = 0;
    if (dtStatusFailed(query_->raycast(sref, s, ep, &filter, &t, n, polys, &npolys, kMaxPathPolys))) return false;
    if (t > 1.0f) return false; // reached the end
    hit = Vec3(s[0] + (ep[0] - s[0]) * t, s[1] + (ep[1] - s[1]) * t, s[2] + (ep[2] - s[2]) * t);
    if (normal != nullptr) *normal = Vec3(n[0], n[1], n[2]);
    return true;
}

bool NavMesh::RandomPoint(u64 seed, Vec3& out, const NavQueryFilter& f) const {
    if (query_ == nullptr) return false;
    const AreaFilter filter(f);
    g_rand_state = seed * 2654435761ULL + 1;
    dtPolyRef ref = 0;
    float p[3];
    if (dtStatusFailed(query_->findRandomPoint(&filter, DetourRand, &ref, p)) || ref == 0) return false;
    out = Vec3(p[0], p[1], p[2]);
    return true;
}

bool NavMesh::RandomPointNear(const Vec3& center, f32 radius, u64 seed, Vec3& out, const NavQueryFilter& f) const {
    if (query_ == nullptr) return false;
    const AreaFilter filter(f);
    const float ext[3] = {2.0f, 4.0f, 2.0f};
    const float c[3] = {center.x, center.y, center.z};
    float s[3];
    dtPolyRef sref = 0;
    query_->findNearestPoly(c, ext, &filter, &sref, s);
    if (sref == 0) return false;
    g_rand_state = seed * 2654435761ULL + 1;
    // Detour picks a point in a random polygon touching the circle, which a big
    // polygon can put far outside it: keep drawing until one lands inside.
    const f32 r2 = radius * radius;
    for (int attempt = 0; attempt < 32; ++attempt) {
        dtPolyRef ref = 0;
        float p[3];
        if (dtStatusFailed(query_->findRandomPointAroundCircle(sref, s, radius, &filter, DetourRand, &ref, p)) || ref == 0) return false;
        if ((p[0] - s[0]) * (p[0] - s[0]) + (p[2] - s[2]) * (p[2] - s[2]) <= r2) {
            out = Vec3(p[0], p[1], p[2]);
            return true;
        }
    }
    // Then a point in the disc, walked to over the mesh from the center (so it stays connected).
    const f32 angle = DetourRand() * 6.2831853f, dist = radius * std::sqrt(DetourRand());
    const float target[3] = {s[0] + std::cos(angle) * dist, s[1], s[2] + std::sin(angle) * dist};
    float p[3];
    dtPolyRef visited[16];
    int nvisited = 0;
    if (dtStatusFailed(query_->moveAlongSurface(sref, s, target, &filter, p, visited, &nvisited, 16))) return false;
    if (nvisited > 0) query_->getPolyHeight(visited[nvisited - 1], p, &p[1]);
    out = Vec3(p[0], p[1], p[2]);
    return true;
}

usize NavMesh::TileCount() const {
    if (mesh_ == nullptr) return 0;
    usize n = 0;
    const dtNavMesh* m = mesh_;
    for (int i = 0; i < m->getMaxTiles(); ++i) {
        const dtMeshTile* t = m->getTile(i);
        if (t != nullptr && t->header != nullptr) ++n;
    }
    return n;
}

usize NavMesh::PolygonCount() const {
    if (mesh_ == nullptr) return 0;
    usize n = 0;
    const dtNavMesh* m = mesh_;
    for (int i = 0; i < m->getMaxTiles(); ++i) {
        const dtMeshTile* t = m->getTile(i);
        if (t != nullptr && t->header != nullptr) n += static_cast<usize>(t->header->polyCount);
    }
    return n;
}

std::vector<NavPolygon> NavMesh::Polygons() const {
    std::vector<NavPolygon> out;
    if (mesh_ == nullptr) return out;
    const dtNavMesh* m = mesh_;
    for (int i = 0; i < m->getMaxTiles(); ++i) {
        const dtMeshTile* t = m->getTile(i);
        if (t == nullptr || t->header == nullptr) continue;
        for (int p = 0; p < t->header->polyCount; ++p) {
            const dtPoly& poly = t->polys[p];
            if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION) continue;
            NavPolygon np;
            np.area = poly.getArea();
            np.tile_x = t->header->x;
            np.tile_z = t->header->y;
            for (int v = 0; v < poly.vertCount; ++v) {
                const float* q = &t->verts[poly.verts[v] * 3];
                np.vertices.push_back(Vec3(q[0], q[1], q[2]));
            }
            out.push_back(std::move(np));
        }
    }
    return out;
}

std::vector<NavLink> NavMesh::Links() const {
    std::vector<NavLink> out;
    if (mesh_ == nullptr) return out;
    const dtNavMesh* m = mesh_;
    for (int i = 0; i < m->getMaxTiles(); ++i) {
        const dtMeshTile* t = m->getTile(i);
        if (t == nullptr || t->header == nullptr) continue;
        for (int c = 0; c < t->header->offMeshConCount; ++c) {
            const dtOffMeshConnection& con = t->offMeshCons[c];
            NavLink l;
            l.start = Vec3(con.pos[0], con.pos[1], con.pos[2]);
            l.end = Vec3(con.pos[3], con.pos[4], con.pos[5]);
            l.radius = con.rad;
            l.bidirectional = (con.flags & DT_OFFMESH_CON_BIDIR) != 0;
            l.area = t->polys[con.poly].getArea();
            l.user_id = con.userId;
            out.push_back(l);
        }
    }
    return out;
}

} // namespace aether::nav
