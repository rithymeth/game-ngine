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
