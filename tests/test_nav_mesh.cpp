#include "aether/nav/navmesh.h"
#include "test_framework.h"

#include <cmath>

// Phase 20 step 1: baking navigation meshes with Recast (walls, steps,
// the agent's radius, islands, areas, tiles) and querying them with Detour
// (paths, nearest points, raycasts, random points, costs, files, tiles).

using namespace aether;
using namespace aether::nav;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

constexpr u8 kWater = 1;

bool Near(f32 a, f32 b, f32 tol) { return std::fabs(a - b) <= tol; }

f32 Flat(const Vec3& a, const Vec3& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z)); }

bool Bake(const NavGeometry& g, NavMesh& mesh, NavMeshSettings s = {}, NavMeshData* out = nullptr) {
    NavMeshData data;
    std::string error;
    if (!BuildNavMesh(g, s, data, &error)) return false;
    if (!mesh.Load(data, &error)) return false;
    if (out != nullptr) *out = data;
    return true;
}

// A 20 x 20 floor at y = 0.
NavGeometry Floor() {
    NavGeometry g;
    g.AddPlane(Vec3(0, 0, 0), 10, 10);
    return g;
}

// A floor split by a water channel (x in -2..2) with a dry bridge at z > 6.
NavGeometry Channel() {
    NavGeometry g;
    g.AddPlane(Vec3(-6, 0, 0), 4, 10);
    g.AddPlane(Vec3(6, 0, 0), 4, 10);
    g.AddPlane(Vec3(0, 0, -2), 2, 8, kWater);
    g.AddPlane(Vec3(0, 0, 8), 2, 2);
    return g;
}

} // namespace

AETHER_TEST(Nav_FlatFloorStraightPath) {
    NavMesh mesh;
    NavBuildStats stats;
    NavMeshData data;
    CHECK(BuildNavMesh(Floor(), {}, data, nullptr, &stats));
    CHECK(stats.tiles > 0 && stats.polygons > 0);
    CHECK(mesh.Load(data));
    CHECK(mesh.Loaded());
    CHECK(mesh.PolygonCount() == stats.polygons);
    NavPath path;
    CHECK(mesh.FindPath(Vec3(-5, 0, 0), Vec3(5, 0, 0), path));
    CHECK(path.status == PathStatus::Complete);
    CHECK(path.points.size() == 2);
    CHECK(Near(path.Length(), 10.0f, 0.1f));
    CHECK(Near(path.points.front().y, 0.0f, 0.25f));
    CHECK(mesh.Reachable(Vec3(-8, 0, -8), Vec3(8, 0, 8)));
}

AETHER_TEST(Nav_BuildRejectsBadInput) {
    NavMeshData data;
    std::string error;
    CHECK(!BuildNavMesh(NavGeometry{}, {}, data, &error));
    CHECK(!error.empty());
    NavMeshSettings bad;
    bad.cell_size = 0.0f;
    CHECK(!BuildNavMesh(Floor(), bad, data, &error));
    // A wall only: nothing to stand on.
    NavGeometry wall;
    wall.AddTriangles({{0, 0, 0}, {0, 3, 0}, {0, 3, 3}}, {0, 1, 2});
    CHECK(!BuildNavMesh(wall, {}, data, &error));
    NavMesh mesh;
    NavPath path;
    CHECK(!mesh.FindPath(Vec3(0, 0, 0), Vec3(1, 0, 0), path));
    CHECK(path.status == PathStatus::None);
}

AETHER_TEST(Nav_PathGoesAroundAWall) {
    NavGeometry g = Floor();
    g.AddBox(Vec3(0, 1.5f, 0), Vec3(0.5f, 1.5f, 6.0f)); // a wall across the middle
    NavMesh mesh;
    CHECK(Bake(g, mesh));
    NavPath path;
    CHECK(mesh.FindPath(Vec3(-5, 0, 0), Vec3(5, 0, 0), path));
    CHECK(path.status == PathStatus::Complete);
    CHECK(path.points.size() >= 3);
    CHECK(path.Length() > 14.0f);
    // The corners round the wall's end, the agent's radius clear of it.
    bool rounds = false;
    for (const Vec3& p : path.points) rounds = rounds || std::fabs(p.z) > 6.4f;
    CHECK(rounds);
    // The wall's top is too small and high to count; no polygon is on it.
    for (const NavPolygon& poly : mesh.Polygons())
        for (const Vec3& v : poly.vertices) CHECK(v.y < 1.0f);
}

AETHER_TEST(Nav_RaycastStopsAtWalls) {
    NavGeometry g = Floor();
    g.AddBox(Vec3(0, 1.5f, 0), Vec3(0.5f, 1.5f, 6.0f));
    NavMesh mesh;
    CHECK(Bake(g, mesh));
    Vec3 hit, normal;
    CHECK(mesh.Raycast(Vec3(-5, 0, 0), Vec3(5, 0, 0), hit, &normal));
    CHECK(Near(hit.x, -1.1f, 0.35f)); // the wall's face less the radius
    CHECK(normal.x < -0.9f);
    // Parallel to the wall: clear.
    CHECK(!mesh.Raycast(Vec3(-5, 0, -2), Vec3(-5, 0, 3), hit));
    // Off the floor's edge.
    CHECK(mesh.Raycast(Vec3(-5, 0, 7), Vec3(-5, 0, 30), hit));
    CHECK(hit.z < 10.0f && hit.z > 8.5f);
}

AETHER_TEST(Nav_StepsAndLedges) {
    NavGeometry g = Floor();
    g.AddBox(Vec3(-5, 0.2f, 0), Vec3(2, 0.2f, 2));  // a 0.4 step: climbable
    g.AddBox(Vec3(5, 1.5f, 0), Vec3(3, 1.5f, 3));    // a 3.0 block: not
    NavMesh mesh;
    CHECK(Bake(g, mesh));
    Vec3 p;
    CHECK(mesh.NearestPoint(Vec3(-5, 0.5f, 0), p));
    CHECK(p.y > 0.25f && p.y < 0.8f); // on the step (within Recast's voxel height error)
    CHECK(mesh.Reachable(Vec3(-9, 0, -8), Vec3(-5, 0.4f, 0)));
    // The block's top is walkable ground of its own, but cut off: a partial path.
    CHECK(mesh.NearestPoint(Vec3(5, 3.2f, 0), p));
    CHECK(Near(p.y, 3.0f, 0.4f));
    NavPath path;
    CHECK(mesh.FindPath(Vec3(-9, 0, -8), Vec3(5, 3, 0), path));
    CHECK(path.status == PathStatus::Partial);
    CHECK(Near(path.points.back().y, 0.0f, 0.25f));
    CHECK(!mesh.Reachable(Vec3(-9, 0, -8), Vec3(5, 3, 0)));
}

AETHER_TEST(Nav_AgentRadiusKeepsOffEdges) {
    NavMeshSettings wide;
    wide.agent_radius = 1.5f;
    for (const NavMeshSettings& s : {NavMeshSettings{}, wide}) {
        NavMesh mesh;
        CHECK(Bake(Floor(), mesh, s));
        Vec3 p;
        CHECK(mesh.NearestPoint(Vec3(9.9f, 0, 0), p));
        CHECK(p.x < 10.0f - s.agent_radius + s.cell_size);
        CHECK(p.x > 10.0f - s.agent_radius - 2 * s.cell_size);
    }
    // A corridor narrower than the agent is closed.
    NavGeometry g;
    g.AddPlane(Vec3(-6, 0, 0), 4, 4);
    g.AddPlane(Vec3(6, 0, 0), 4, 4);
    g.AddPlane(Vec3(0, 0, 0), 2, 0.5f); // 1.0 wide
    NavMesh thin, narrow;
    CHECK(Bake(g, thin));
    CHECK(!thin.Reachable(Vec3(-6, 0, 0), Vec3(6, 0, 0)));
    NavMeshSettings small;
    small.agent_radius = 0.2f;
    small.cell_size = 0.1f;
    CHECK(Bake(g, narrow, small));
    CHECK(narrow.Reachable(Vec3(-6, 0, 0), Vec3(6, 0, 0)));
}

AETHER_TEST(Nav_AreaCostsAndExclusion) {
    NavMesh mesh;
    CHECK(Bake(Channel(), mesh));
    u8 area = 0;
    Vec3 p;
    CHECK(mesh.NearestPoint(Vec3(0, 0, 0), p, Vec3(0.5f, 2, 0.5f), &area));
    CHECK(area == kWater);
    CHECK(mesh.NearestPoint(Vec3(-6, 0, 0), p, Vec3(2, 4, 2), &area));
    CHECK(area == kAreaWalkable);
    bool water_polys = false;
    for (const NavPolygon& poly : mesh.Polygons()) water_polys = water_polys || poly.area == kWater;
    CHECK(water_polys);

    // Every area costs the same: straight through the water.
    NavPath straight;
    CHECK(mesh.FindPath(Vec3(-6, 0, 0), Vec3(6, 0, 0), straight));
    CHECK(straight.status == PathStatus::Complete);
    CHECK(Near(straight.Length(), 12.0f, 0.2f));
    // Wading costs ten times as much: round by the bridge.
    NavQueryFilter costly;
    costly.area_costs[kWater] = 10.0f;
    NavPath bridge;
    CHECK(mesh.FindPath(Vec3(-6, 0, 0), Vec3(6, 0, 0), bridge, costly));
    CHECK(bridge.status == PathStatus::Complete);
    CHECK(bridge.Length() > 16.0f);
    for (const Vec3& q : bridge.points) CHECK(std::fabs(q.x) > 1.5f || q.z > 5.5f);
    // A small detour isn't worth it when wading is cheap enough.
    NavQueryFilter mild;
    mild.area_costs[kWater] = 1.1f;
    NavPath wade;
    CHECK(mesh.FindPath(Vec3(-6, 0, 0), Vec3(6, 0, 0), wade, mild));
    CHECK(Near(wade.Length(), 12.0f, 0.2f));

    // Excluded: never entered, even when that's the only way.
    NavQueryFilter dry;
    dry.excluded[kWater] = true;
    NavPath around;
    CHECK(mesh.FindPath(Vec3(-6, 0, 0), Vec3(6, 0, 0), around, dry));
    CHECK(around.status == PathStatus::Complete);
    CHECK(around.Length() > 16.0f);
    Vec3 hit;
    CHECK(mesh.Raycast(Vec3(-6, 0, 0), Vec3(6, 0, 0), hit, nullptr, dry));
    CHECK(hit.x < -1.5f);
    CHECK(!mesh.Raycast(Vec3(-6, 0, 0), Vec3(6, 0, 0), hit)); // wading is fine
    NavQueryFilter no_ground;
    no_ground.excluded[kAreaWalkable] = true;
    NavPath none;
    CHECK(!mesh.FindPath(Vec3(-6, 0, 0), Vec3(6, 0, 0), none, no_ground) || none.status != PathStatus::Complete);
    for (u64 seed = 0; seed < 20; ++seed) {
        Vec3 r;
        if (mesh.RandomPoint(seed, r, dry)) CHECK(std::fabs(r.x) > 1.5f || r.z > 5.5f);
    }
}

AETHER_TEST(Nav_SaveAndLoad) {
    NavGeometry g = Floor();
    g.AddBox(Vec3(0, 1.5f, 0), Vec3(0.5f, 1.5f, 6.0f));
    NavMesh mesh;
    NavMeshData data;
    CHECK(Bake(g, mesh, {}, &data));
    const std::vector<u8> bytes = SaveNavMesh(data);
    CHECK(bytes.size() > 16);
    CHECK(bytes[0] == 'A' && bytes[1] == 'N' && bytes[2] == 'A' && bytes[3] == 'V');
    NavMeshData loaded;
    std::string error;
    CHECK(LoadNavMesh(bytes, loaded, &error));
    CHECK(loaded.tiles.size() == data.tiles.size());
    CHECK(loaded.tiles_x == data.tiles_x && loaded.tiles_z == data.tiles_z);
    CHECK(loaded.settings.agent_radius == data.settings.agent_radius);
    NavMesh again;
    CHECK(again.Load(loaded));
    NavPath a, b;
    CHECK(mesh.FindPath(Vec3(-5, 0, 0), Vec3(5, 0, 0), a));
    CHECK(again.FindPath(Vec3(-5, 0, 0), Vec3(5, 0, 0), b));
    CHECK(a.points.size() == b.points.size());
    for (usize i = 0; i < a.points.size() && i < b.points.size(); ++i) CHECK(Flat(a.points[i], b.points[i]) < 1e-4f);
    // Broken files are refused.
    CHECK(!LoadNavMesh({}, loaded, &error));
    CHECK(!LoadNavMesh(std::vector<u8>(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(bytes.size() / 2)), loaded, &error));
    std::vector<u8> wrong = bytes;
    wrong[0] = 'X';
    CHECK(!LoadNavMesh(wrong, loaded, &error));
}

AETHER_TEST(Nav_TiledMatchesSingleTile) {
    NavGeometry g = Floor();
    g.AddBox(Vec3(0, 1.5f, 0), Vec3(0.5f, 1.5f, 6.0f));
    NavMeshSettings one;
    one.tile_size = 0;
    NavMeshSettings tiled;
    tiled.tile_size = 24; // 7.2 world units: 3 x 3 tiles
    NavMesh a, b;
    NavMeshData da, db;
    CHECK(Bake(g, a, one, &da));
    CHECK(Bake(g, b, tiled, &db));
    CHECK(da.tiles_x == 1 && da.tiles_z == 1);
    CHECK(db.tiles_x == 3 && db.tiles_z == 3);
    CHECK(a.TileCount() == 1);
    CHECK(b.TileCount() == 9);
    NavPath pa, pb;
    CHECK(a.FindPath(Vec3(-8, 0, -8), Vec3(8, 0, 8), pa));
    CHECK(b.FindPath(Vec3(-8, 0, -8), Vec3(8, 0, 8), pb));
    CHECK(pa.status == PathStatus::Complete && pb.status == PathStatus::Complete);
    CHECK(Near(pa.Length(), pb.Length(), 0.3f));
    CHECK(a.FindPath(Vec3(-5, 0, 0), Vec3(5, 0, 0), pa));
    CHECK(b.FindPath(Vec3(-5, 0, 0), Vec3(5, 0, 0), pb));
    CHECK(Near(pa.Length(), pb.Length(), 0.3f));
    // Each polygon belongs to its tile.
    for (const NavPolygon& poly : b.Polygons()) {
        CHECK(poly.tile_x >= 0 && poly.tile_x < 3 && poly.tile_z >= 0 && poly.tile_z < 3);
        CHECK(poly.vertices.size() >= 3);
    }
}

AETHER_TEST(Nav_RandomPoints) {
    NavMesh mesh;
    CHECK(Bake(Floor(), mesh));
    Vec3 a, b, c;
    CHECK(mesh.RandomPoint(7, a));
    CHECK(mesh.RandomPoint(7, b));
    CHECK(Flat(a, b) < 1e-6f); // the same seed, the same point
    CHECK(std::fabs(a.x) < 10.0f && std::fabs(a.z) < 10.0f && Near(a.y, 0.0f, 0.25f));
    bool differs = false;
    for (u64 seed = 8; seed < 16; ++seed) {
        CHECK(mesh.RandomPoint(seed, c));
        differs = differs || Flat(a, c) > 0.5f;
    }
    CHECK(differs);
    for (u64 seed = 0; seed < 20; ++seed) {
        CHECK(mesh.RandomPointNear(Vec3(3, 0, 3), 2.0f, seed, c));
        CHECK(Flat(c, Vec3(3, 0, 3)) < 2.0f + 1.5f); // within the radius (Detour's polygons can overshoot a little)
    }
    CHECK(!mesh.RandomPointNear(Vec3(50, 0, 50), 2.0f, 1, c)); // off the mesh
}

AETHER_TEST(Nav_RebuildOneTile) {
    NavMeshSettings s;
    s.tile_size = 24;
    NavMesh mesh;
    NavMeshData data;
    CHECK(Bake(Floor(), mesh, s, &data));
    NavPath before;
    CHECK(mesh.FindPath(Vec3(-3, 0, 0), Vec3(3, 0, 0), before));
    CHECK(Near(before.Length(), 6.0f, 0.1f));
    // A crate appears in the middle tile: rebake just that tile.
    NavGeometry g = Floor();
    g.AddBox(Vec3(0, 1, 0), Vec3(1, 1, 1));
    NavMeshData::Tile tile;
    std::string error;
    CHECK(BuildNavTile(g, data, 1, 1, tile, &error));
    CHECK(!tile.data.empty());
    CHECK(mesh.ReplaceTile(tile, &error));
    CHECK(mesh.TileCount() == 9);
    NavPath after;
    CHECK(mesh.FindPath(Vec3(-3, 0, 0), Vec3(3, 0, 0), after));
    CHECK(after.status == PathStatus::Complete);
    CHECK(after.Length() > 6.5f);
    // The mesh's data follows, so saving keeps the rebuild.
    NavMesh reloaded;
    NavMeshData saved;
    CHECK(LoadNavMesh(SaveNavMesh(mesh.Data()), saved));
    CHECK(reloaded.Load(saved));
    NavPath again;
    CHECK(reloaded.FindPath(Vec3(-3, 0, 0), Vec3(3, 0, 0), again));
    CHECK(Near(again.Length(), after.Length(), 1e-3f));
    // Removing the middle tile leaves a hole.
    CHECK(mesh.ReplaceTile(NavMeshData::Tile{1, 1, {}}));
    CHECK(mesh.TileCount() == 8);
    CHECK(mesh.Data().tiles.size() == 8);
    Vec3 p;
    CHECK(!mesh.NearestPoint(Vec3(0, 0, 0), p, Vec3(0.5f, 1, 0.5f)));
    CHECK(mesh.Reachable(Vec3(-8, 0, -8), Vec3(8, 0, 8)));
}
