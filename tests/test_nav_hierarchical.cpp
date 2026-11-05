#include "aether/nav/hierarchical.h"
#include "test_framework.h"

#include <cmath>

// Phase 40 step 2: hierarchical pathfinding over a tiled mesh - paths cross many tiles with short Detour legs,
// stay walkable and near the shortest, and go around walls.

using namespace aether;
using namespace aether::nav;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

NavMeshSettings Tiles() {
    NavMeshSettings s;
    s.tile_size = 40; // 12 units: a 120 x 120 floor is 10 x 10 tiles
    return s;
}

bool Bake(const NavGeometry& g, NavMesh& mesh) {
    NavMeshData data;
    return BuildNavMesh(g, Tiles(), data) && mesh.Load(data);
}

} // namespace

AETHER_TEST(NavHierarchical_CrossesManyTilesNearTheShortestPath) {
    NavGeometry g;
    g.AddPlane(Vec3(0, 0, 0), 60, 60);
    NavMesh mesh;
    CHECK(Bake(g, mesh));
    HierarchicalPathfinder h;
    CHECK(h.Build(mesh));
    CHECK(h.Stats().tiles >= 50 && h.Stats().portals > 0 && h.Stats().edges > 0);
    NavPath hp, fp;
    CHECK(h.FindPath(Vec3(-50, 0, -50), Vec3(50, 0, 50), hp));
    CHECK(mesh.FindPath(Vec3(-50, 0, -50), Vec3(50, 0, 50), fp));
    CHECK(hp.status == PathStatus::Complete && hp.points.size() >= 2);
    CHECK(h.Stats().legs > 2);
    CHECK(hp.Length() <= fp.Length() * 1.2f);
    CHECK((hp.points.back() - Vec3(50, 0, 50)).Length() < 1.5f);
    CHECK((hp.points.front() - Vec3(-50, 0, -50)).Length() < 1.5f);
}

AETHER_TEST(NavHierarchical_GoesAroundAWall) {
    NavGeometry g;
    g.AddPlane(Vec3(0, 0, 0), 60, 60);
    g.volumes.push_back(NavVolume::Box(Vec3(0, 0, 0), Vec3(2, 2, 45))); // a wall across the middle, gaps at both ends
    NavMesh mesh;
    CHECK(Bake(g, mesh));
    HierarchicalPathfinder h;
    CHECK(h.Build(mesh));
    NavPath hp;
    CHECK(h.FindPath(Vec3(-50, 0, 0), Vec3(50, 0, 0), hp));
    CHECK(hp.status == PathStatus::Complete);
    f32 max_z = 0;
    for (const Vec3& p : hp.points) max_z = std::max(max_z, std::fabs(p.z));
    CHECK(max_z > 40.0f); // it went round the end
    CHECK(hp.Length() > 100.0f);
}

AETHER_TEST(NavHierarchical_NearbyAndUnbuiltCasesFallBack) {
    NavGeometry g;
    g.AddPlane(Vec3(0, 0, 0), 60, 60);
    NavMesh mesh;
    CHECK(Bake(g, mesh));
    HierarchicalPathfinder h;
    NavPath p;
    CHECK(!h.FindPath(Vec3(0, 0, 0), Vec3(1, 0, 1), p)); // not built
    CHECK(h.Build(mesh));
    CHECK(h.FindPath(Vec3(0, 0, 0), Vec3(3, 0, 3), p));
    CHECK(h.Stats().direct == 1 && h.Stats().legs == 1);
    CHECK(!h.Build(NavMesh())); // nothing loaded
}

AETHER_TEST(NavHierarchical_FollowsASerpentineOfWalls) {
    // A serpentine of walls forces a path far longer than the straight line, over many tiles.
    NavGeometry g;
    g.AddPlane(Vec3(0, 0, 0), 150, 150);
    for (int i = 0; i < 9; ++i) {
        const f32 x = -135.0f + static_cast<f32>(i) * 30.0f;
        const f32 z_center = (i % 2 == 0) ? 15.0f : -15.0f; // a gap at the top, then at the bottom
        g.volumes.push_back(NavVolume::Box(Vec3(x, 0, z_center), Vec3(2, 2, 135)));
    }
    NavMesh mesh;
    CHECK(Bake(g, mesh));
    HierarchicalPathfinder h;
    CHECK(h.Build(mesh));
    NavPath plain, hier;
    CHECK(mesh.FindPath(Vec3(-145, 0, 0), Vec3(145, 0, 0), plain));
    CHECK(h.FindPath(Vec3(-145, 0, 0), Vec3(145, 0, 0), hier));
    CHECK(plain.status == PathStatus::Complete && hier.status == PathStatus::Complete);
    CHECK(hier.Length() > 290.0f * 5.0f);          // it really snakes: many times the straight line
    CHECK(hier.Length() <= plain.Length() * 1.15f); // and stays near the shortest
    // It went through the portal graph rather than falling back to a plain search.
    // The leg count is no longer asserted: it used to be one Detour search per portal
    // along the route, and collinear waypoints are now collapsed (this serpentine went
    // from more than ten legs to two, with the same path). Path quality above is what
    // matters -- the shape of the route is unchanged.
    CHECK(h.Stats().direct == 0);
    CHECK(h.Stats().legs >= 1);
}

// Phase 40.2: a pathfinder can be rebuilt when the mesh changes (a dynamic edit),
// and rebuilding doesn't accumulate portals.
AETHER_TEST(NavHierarchical_RebuildsAfterTheMeshChanges) {
    NavGeometry g;
    g.AddPlane(Vec3(0, 0, 0), 60, 60);
    NavMesh mesh;
    CHECK(Bake(g, mesh));
    HierarchicalPathfinder h;
    CHECK(h.Build(mesh));
    const HierarchicalStats first = h.Stats();
    CHECK(first.portals > 0);

    // Build clears its state first, so repeating it is idempotent.
    CHECK(h.Build(mesh));
    CHECK(h.Stats().portals == first.portals && h.Stats().edges == first.edges);

    // A wall appears; the rebuilt graph reflects the new mesh.
    g.AddBox(Vec3(0, 1.5f, 0), Vec3(1, 1.5f, 20));
    CHECK(Bake(g, mesh));
    CHECK(h.Build(mesh));
    NavPath hp;
    CHECK(h.FindPath(Vec3(-40, 0, 0), Vec3(40, 0, 0), hp));
    CHECK(hp.status == PathStatus::Complete);
    bool off_line = false;
    for (const Vec3& p : hp.points) off_line = off_line || std::fabs(p.z) > 2.0f;
    CHECK(off_line); // it had to go around the wall
}
