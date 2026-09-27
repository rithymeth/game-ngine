#include "aether/nav/components.h"
#include "aether/nav/dynamic.h"
#include "test_framework.h"

#include <cmath>
#include <cstring>

// Phase 20 step 2: runtime navigation changes - carving obstacles (boxes,
// turned boxes, cylinders, prisms), area volumes, off-mesh links, tile
// rebakes within a budget, and the scene components that drive them.

using namespace aether;
using namespace aether::nav;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

constexpr u8 kWater = 1;

bool Near(f32 a, f32 b, f32 tol) { return std::fabs(a - b) <= tol; }
f32 Flat(const Vec3& a, const Vec3& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z)); }

NavGeometry Floor() {
    NavGeometry g;
    g.AddPlane(Vec3(0, 0, 0), 10, 10);
    return g;
}

// Two pads with a 6-unit gap between them (x in -3..3).
NavGeometry Pads() {
    NavGeometry g;
    g.AddPlane(Vec3(-6, 0, 0), 3, 3);
    g.AddPlane(Vec3(6, 0, 0), 3, 3);
    return g;
}

NavMeshSettings Tiled() {
    NavMeshSettings s;
    s.tile_size = 24; // 7.2 units: 3 x 3 tiles over the floor
    return s;
}

f32 PathLength(const NavMesh& mesh, const Vec3& a, const Vec3& b, PathStatus* status = nullptr, const NavQueryFilter& f = {}) {
    NavPath p;
    if (!mesh.FindPath(a, b, p, f)) return -1.0f;
    if (status != nullptr) *status = p.status;
    return p.Length();
}

// Whether `p` itself is on the mesh (the nearest point can be farther than the search box).
bool OnMesh(const NavMesh& mesh, const Vec3& p) {
    Vec3 out;
    return mesh.NearestPoint(p, out, Vec3(0.2f, 1, 0.2f)) && Flat(out, p) < 0.05f;
}

Transform At(const Vec3& p, f32 yaw_degrees = 0.0f) {
    return Transform{p, Quaternion::FromAxisAngle(Vec3(0, 1, 0), yaw_degrees * 3.14159265f / 180.0f)};
}

ModelRenderer Model(const char* name) {
    ModelRenderer m;
    SetModelPath(m, name);
    return m;
}

// "floor" is 20 x 20, "pad" 6 x 6, "crate" a 2-unit box on the ground.
bool Meshes(const ModelRenderer& m, std::vector<Vec3>& v, std::vector<u32>& i) {
    NavGeometry g;
    if (std::strcmp(m.asset_path, "floor") == 0) g.AddPlane(Vec3(0, 0, 0), 10, 10);
    else if (std::strcmp(m.asset_path, "pad") == 0) g.AddPlane(Vec3(0, 0, 0), 3, 3);
    else if (std::strcmp(m.asset_path, "crate") == 0) g.AddBox(Vec3(0, 0, 0), Vec3(1, 1, 1));
    else return false;
    v = g.vertices;
    i = g.indices;
    return true;
}

} // namespace

AETHER_TEST(NavDyn_ObstacleCarvesAndMoves) {
    DynamicNavMesh dyn;
    std::string error;
    CHECK(dyn.Build(Floor(), Tiled(), &error));
    CHECK(dyn.PendingTiles() == 0);
    const Vec3 a(-3, 0, 0), b(3, 0, 0);
    CHECK(Near(PathLength(dyn.Mesh(), a, b), 6.0f, 0.1f));
    const u64 rev = dyn.Revision();

    const auto id = dyn.AddVolume(NavVolume::Box(Vec3(0, 1, 0), Vec3(1, 1, 1)));
    CHECK(id != 0);
    CHECK(dyn.PendingTiles() > 0);
    CHECK(dyn.TilePending(1, 1));
    CHECK(Near(PathLength(dyn.Mesh(), a, b), 6.0f, 0.1f)); // nothing changes until Update
    CHECK(dyn.Update(0) > 0);
    CHECK(dyn.PendingTiles() == 0);
    CHECK(dyn.Revision() > rev);
    PathStatus st = PathStatus::None;
    CHECK(PathLength(dyn.Mesh(), a, b, &st) > 6.5f);
    CHECK(st == PathStatus::Complete);
    CHECK(!OnMesh(dyn.Mesh(), Vec3(0, 0, 0)));
    // The agent's radius is kept off it too.
    CHECK(!OnMesh(dyn.Mesh(), Vec3(1.3f, 0, 0)));
    CHECK(OnMesh(dyn.Mesh(), Vec3(2.0f, 0, 0)));

    // Moving it: the old place opens, the new one closes.
    CHECK(dyn.UpdateVolume(id, NavVolume::Box(Vec3(0, 1, 5), Vec3(1, 1, 1))));
    dyn.Update(0);
    CHECK(Near(PathLength(dyn.Mesh(), a, b), 6.0f, 0.1f));
    CHECK(!OnMesh(dyn.Mesh(), Vec3(0, 0, 5)));
    // An unchanged update costs nothing.
    CHECK(dyn.UpdateVolume(id, NavVolume::Box(Vec3(0, 1, 5), Vec3(1, 1, 1))));
    CHECK(dyn.PendingTiles() == 0);
    CHECK(dyn.RemoveVolume(id));
    CHECK(!dyn.RemoveVolume(id));
    CHECK(!dyn.UpdateVolume(id, NavVolume{}));
    dyn.Update(0);
    CHECK(OnMesh(dyn.Mesh(), Vec3(0, 0, 5)));
    CHECK(dyn.VolumeCount() == 0);
}

AETHER_TEST(NavDyn_RebuildBudget) {
    DynamicNavMesh dyn;
    CHECK(dyn.Build(Floor(), Tiled()));
    dyn.AddVolume(NavVolume::Box(Vec3(0, 1, 0), Vec3(8, 1, 0.5f))); // a wall across all three columns
    const usize pending = dyn.PendingTiles();
    CHECK(pending >= 3);
    const usize before = dyn.TilesRebuilt();
    usize frames = 0;
    while (dyn.PendingTiles() > 0 && frames < 100) {
        const usize left = dyn.PendingTiles();
        CHECK(dyn.Update(1) == 1);
        CHECK(dyn.PendingTiles() == left - 1);
        ++frames;
    }
    CHECK(frames == pending);
    CHECK(dyn.TilesRebuilt() == before + pending);
    CHECK(dyn.Update(1) == 0);
    PathStatus st = PathStatus::None;
    CHECK(PathLength(dyn.Mesh(), Vec3(0, 0, -5), Vec3(0, 0, 5), &st) > 10.0f); // round the wall's end
    CHECK(st == PathStatus::Complete);
    dyn.MarkAllDirty();
    CHECK(dyn.PendingTiles() == 9);
    CHECK(dyn.Update(0) == 9);
}

AETHER_TEST(NavDyn_Shapes) {
    DynamicNavMesh dyn;
    CHECK(dyn.Build(Floor(), Tiled()));
    // A cylinder, and the agent's radius round it.
    const auto cyl = dyn.AddVolume(NavVolume::Cylinder(Vec3(0, 1, 0), 1.0f, 2.0f));
    dyn.Update(0);
    Vec3 p;
    CHECK(dyn.Mesh().NearestPoint(Vec3(0, 0, 0), p, Vec3(4, 1, 4)));
    CHECK(Flat(p, Vec3(0, 0, 0)) > 1.3f);
    CHECK(!OnMesh(dyn.Mesh(), Vec3(1.0f, 0, 1.0f))); // 1.41 out: within the radius plus the agent's
    CHECK(OnMesh(dyn.Mesh(), Vec3(2.2f, 0, 0)));
    dyn.RemoveVolume(cyl);

    // A thin wall turned 45 degrees: its long side runs along (1, 0, -1).
    NavVolume wall = NavVolume::Box(Vec3(0, 1, 0), Vec3(4, 1, 0.2f), kAreaNull, 45.0f);
    Vec3 lo, hi;
    wall.Bounds(lo, hi);
    CHECK(Near(hi.x, 4 * 0.7071f + 0.2f * 0.7071f, 0.01f));
    const auto w = dyn.AddVolume(wall);
    dyn.Update(0);
    CHECK(!OnMesh(dyn.Mesh(), Vec3(2, 0, -2)));
    CHECK(OnMesh(dyn.Mesh(), Vec3(2, 0, 2)));
    PathStatus st = PathStatus::None;
    CHECK(PathLength(dyn.Mesh(), Vec3(-2, 0, -2), Vec3(2, 0, 2), &st) > 7.0f);
    dyn.RemoveVolume(w);

    // A triangular prism.
    dyn.AddVolume(NavVolume::Prism({Vec3(4, 0, 4), Vec3(8, 0, 4), Vec3(6, 0, 8)}, -1, 2));
    dyn.Update(0);
    CHECK(!OnMesh(dyn.Mesh(), Vec3(6, 0, 5.5f)));
    CHECK(OnMesh(dyn.Mesh(), Vec3(0, 0, 0)));
    CHECK(OnMesh(dyn.Mesh(), Vec3(2, 0, -2)));
}

AETHER_TEST(NavDyn_AreaVolumes) {
    // Baked in: a water strip across the floor as a prism.
    NavGeometry g = Floor();
    g.volumes.push_back(NavVolume::Prism({Vec3(-1, 0, -10), Vec3(1, 0, -10), Vec3(1, 0, 10), Vec3(-1, 0, 10)}, -1, 1, kWater));
    DynamicNavMesh dyn;
    CHECK(dyn.Build(g, Tiled()));
    u8 area = 0;
    Vec3 p;
    CHECK(dyn.Mesh().NearestPoint(Vec3(0, 0, 0), p, Vec3(0.2f, 1, 0.2f), &area));
    CHECK(area == kWater);
    NavQueryFilter dry;
    dry.excluded[kWater] = true;
    PathStatus st = PathStatus::None;
    CHECK(Near(PathLength(dyn.Mesh(), Vec3(-5, 0, 0), Vec3(5, 0, 0), &st), 10.0f, 0.1f));
    CHECK(st == PathStatus::Complete);
    NavPath cut;
    CHECK(!dyn.Mesh().FindPath(Vec3(-5, 0, 0), Vec3(5, 0, 0), cut, dry) || cut.status == PathStatus::Partial);

    // A dynamic road across the water (area 2 wins inside it).
    const auto road = dyn.AddVolume(NavVolume::Box(Vec3(0, 0, 6), Vec3(3, 1, 1), 2));
    dyn.Update(0);
    CHECK(dyn.Mesh().NearestPoint(Vec3(0, 0, 6), p, Vec3(0.2f, 1, 0.2f), &area));
    CHECK(area == 2);
    CHECK(PathLength(dyn.Mesh(), Vec3(-5, 0, 0), Vec3(5, 0, 0), &st, dry) > 12.0f);
    CHECK(st == PathStatus::Complete);
    // A modifier with area 0 blocks, like an obstacle.
    CHECK(dyn.UpdateVolume(road, NavVolume::Box(Vec3(0, 0, 6), Vec3(3, 1, 1), kAreaNull)));
    dyn.Update(0);
    CHECK(!OnMesh(dyn.Mesh(), Vec3(0, 0, 6)));
}

AETHER_TEST(NavDyn_OffMeshLinks) {
    const Vec3 a(-6, 0, 0), b(6, 0, 0);
    {
        NavMesh mesh;
        NavMeshData data;
        CHECK(BuildNavMesh(Pads(), {}, data));
        CHECK(mesh.Load(data));
        PathStatus st = PathStatus::None;
        PathLength(mesh, a, b, &st);
        CHECK(st == PathStatus::Partial); // the gap
        CHECK(mesh.Links().empty());
    }
    // Baked in, both ways.
    NavGeometry g = Pads();
    NavLink jump;
    jump.start = Vec3(-4, 0, 0);
    jump.end = Vec3(4, 0, 0);
    jump.radius = 0.6f;
    jump.user_id = 7;
    g.links.push_back(jump);
    NavMesh mesh;
    NavMeshData data;
    CHECK(BuildNavMesh(g, {}, data));
    CHECK(mesh.Load(data));
    NavPath path;
    CHECK(mesh.FindPath(a, b, path));
    CHECK(path.status == PathStatus::Complete);
    CHECK(path.flags.size() == path.points.size());
    usize link_at = path.points.size();
    for (usize i = 0; i < path.flags.size(); ++i)
        if (path.flags[i] & kNavPointLinkStart) link_at = i;
    CHECK(link_at + 1 < path.points.size());
    if (link_at + 1 < path.points.size()) {
        CHECK(Near(path.points[link_at].x, -4, 0.7f));
        CHECK(Near(path.points[link_at + 1].x, 4, 0.7f));
    }
    CHECK(mesh.Reachable(b, a));
    const auto links = mesh.Links();
    CHECK(links.size() == 1);
    if (!links.empty()) {
        CHECK(links[0].user_id == 7);
        CHECK(links[0].bidirectional);
        CHECK(Flat(links[0].start, jump.start) < 0.01f);
    }
    // Excluding another area leaves it be (its own area's exclusion is checked below).
    NavQueryFilter other_area;
    other_area.excluded[kWater] = true;
    CHECK(mesh.Reachable(a, b, other_area));

    // A one-way drop, added at runtime.
    DynamicNavMesh dyn;
    CHECK(dyn.Build(Pads(), {}));
    CHECK(!dyn.Mesh().Reachable(a, b));
    NavLink drop = jump;
    drop.bidirectional = false;
    drop.area = kWater;
    const auto id = dyn.AddLink(drop);
    CHECK(dyn.PendingTiles() > 0);
    dyn.Update(0);
    CHECK(dyn.Mesh().Reachable(a, b));
    CHECK(!dyn.Mesh().Reachable(b, a));
    NavQueryFilter dry;
    dry.excluded[kWater] = true;
    CHECK(!dyn.Mesh().Reachable(a, b, dry));
    CHECK(dyn.LinkCount() == 1);
    CHECK(dyn.RemoveLink(id));
    dyn.Update(0);
    CHECK(!dyn.Mesh().Reachable(a, b));
    CHECK(dyn.Mesh().Links().empty());
}

AETHER_TEST(NavDyn_LoadAndReset) {
    NavMeshData data;
    CHECK(BuildNavMesh(Floor(), Tiled(), data));
    NavMeshData loaded;
    CHECK(LoadNavMesh(SaveNavMesh(data), loaded));
    DynamicNavMesh dyn;
    // Volumes added before loading are baked in by the first Update.
    dyn.AddVolume(NavVolume::Box(Vec3(0, 1, 0), Vec3(1, 1, 1)));
    CHECK(dyn.Load(loaded, Floor()));
    CHECK(dyn.PendingTiles() > 0);
    CHECK(OnMesh(dyn.Mesh(), Vec3(0, 0, 0)));
    dyn.Update(0);
    CHECK(!OnMesh(dyn.Mesh(), Vec3(0, 0, 0)));
    // Saving keeps the rebake.
    NavMeshData resaved;
    CHECK(LoadNavMesh(SaveNavMesh(dyn.Mesh().Data()), resaved));
    NavMesh again;
    CHECK(again.Load(resaved));
    CHECK(!OnMesh(again, Vec3(0, 0, 0)));
    dyn.Reset();
    CHECK(!dyn.Mesh().Loaded());
    CHECK(dyn.VolumeCount() == 0);
    CHECK(dyn.Update(0) == 0);
}

AETHER_TEST(NavDyn_WorldComponents) {
    World world;
    NavWorld nav(world, Meshes);
    CHECK(NavWorld::Active() == &nav);
    world.CreateEntity(At({0, 0, 0}), Model("floor"));
    NavObstacle crate_obstacle;
    crate_obstacle.half_extents = Vec3(1, 1, 1);
    const Entity crate = world.CreateEntity(At({0, 1, 0}), Model("crate"), crate_obstacle);
    NavModifierVolume water;
    water.half_extents = Vec3(2, 1, 2);
    water.area = kWater;
    const Entity pond = world.CreateEntity(At({6, 0, -6}), water);

    // The crate isn't static geometry: only the floor is gathered.
    const NavGeometry g = nav.Gather();
    CHECK(g.TriangleCount() == 2);
    std::string error;
    CHECK(nav.Bake(Tiled(), &error));
    CHECK(nav.Ready());
    CHECK(nav.Dynamic().PendingTiles() == 0);
    CHECK(!OnMesh(nav.Mesh(), Vec3(0, 0, 0)));
    u8 area = 0;
    Vec3 p;
    CHECK(nav.Mesh().NearestPoint(Vec3(6, 0, -6), p, Vec3(0.2f, 1, 0.2f), &area));
    CHECK(area == kWater);

    // Small moves wait for the threshold; bigger ones rebake.
    const u64 rev = nav.Dynamic().Revision();
    world.GetComponent<Transform>(crate)->position = Vec3(0.05f, 1, 0);
    nav.Update(0);
    CHECK(nav.Dynamic().Revision() == rev);
    world.GetComponent<Transform>(crate)->position = Vec3(0, 1, 6);
    nav.Update(0);
    CHECK(nav.Dynamic().Revision() > rev);
    CHECK(OnMesh(nav.Mesh(), Vec3(0, 0, 0)));
    CHECK(!OnMesh(nav.Mesh(), Vec3(0, 0, 6)));
    // Turning a long obstacle rebakes too.
    world.GetComponent<NavObstacle>(crate)->half_extents = Vec3(4, 1, 0.3f);
    nav.Update(0);
    CHECK(!OnMesh(nav.Mesh(), Vec3(3, 0, 6)));
    *world.GetComponent<Transform>(crate) = At({0, 1, 6}, 90.0f);
    nav.Update(0);
    CHECK(OnMesh(nav.Mesh(), Vec3(3, 0, 6)));
    CHECK(!OnMesh(nav.Mesh(), Vec3(0, 0, 9)));
    // Not carving, or gone: the hole closes.
    world.GetComponent<NavObstacle>(crate)->carve = false;
    nav.Update(0);
    CHECK(OnMesh(nav.Mesh(), Vec3(0, 0, 6)));
    world.GetComponent<NavObstacle>(crate)->carve = true;
    nav.Update(0);
    CHECK(!OnMesh(nav.Mesh(), Vec3(0, 0, 6)));
    world.DestroyEntity(crate);
    nav.Update(0);
    CHECK(OnMesh(nav.Mesh(), Vec3(0, 0, 6)));
    CHECK(nav.Dynamic().VolumeCount() == 1); // the pond
    world.DestroyEntity(pond);
    nav.Update(0);
    CHECK(nav.Mesh().NearestPoint(Vec3(6, 0, -6), p, Vec3(0.2f, 1, 0.2f), &area));
    CHECK(area == kAreaWalkable);
}

AETHER_TEST(NavDyn_WorldLinksAndPlacedModels) {
    World world;
    NavWorld nav(world, Meshes);
    // The pads are placed by their transforms.
    world.CreateEntity(At({-6, 0, 0}), Model("pad"));
    world.CreateEntity(At({6, 0, 0}, 30.0f), Model("pad"));
    world.CreateEntity(At({0, 0, 0}), Model("unknown")); // skipped
    NavLinkProxy proxy;
    proxy.start = Vec3(0, 0, 4); // turned by the entity: (-4, 0, 0)
    proxy.end = Vec3(0, 0, -4);  // (4, 0, 0)
    proxy.radius = 0.6f;
    proxy.user_id = 3;
    const Entity link = world.CreateEntity(At({0, 0, 0}, -90.0f), proxy);
    CHECK(nav.Bake({}));
    const Vec3 a(-6, 0, 0), b(6, 0, 0);
    CHECK(nav.Mesh().Reachable(a, b));
    const auto links = nav.Mesh().Links();
    CHECK(links.size() == 1);
    if (!links.empty()) {
        CHECK(links[0].user_id == 3);
        CHECK(Flat(links[0].start, Vec3(-4, 0, 0)) < 0.05f);
    }
    world.GetComponent<NavLinkProxy>(link)->enabled = false;
    nav.Update(0);
    CHECK(!nav.Mesh().Reachable(a, b));
    world.GetComponent<NavLinkProxy>(link)->enabled = true;
    world.GetComponent<NavLinkProxy>(link)->bidirectional = false;
    nav.Update(0);
    CHECK(nav.Mesh().Reachable(a, b));
    CHECK(!nav.Mesh().Reachable(b, a));

    // Loading a saved mesh keeps following the components.
    NavMeshData saved = nav.Mesh().Data();
    NavWorld other(world, Meshes);
    CHECK(other.Load(saved));
    CHECK(other.Mesh().Reachable(a, b));
    world.DestroyEntity(link);
    other.Update(0);
    CHECK(!other.Mesh().Reachable(a, b));
}
