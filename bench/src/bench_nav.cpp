// Navigation benchmarks (Phase 40 step 3): a crowd of 1000 agents crossing an open field, and a long path query
// through the hierarchical pathfinder against plain Detour.
#include "aether/bench/bench.h"
#include "aether/nav/crowd.h"
#include "aether/nav/hierarchical.h"
#include "aether/scene/components.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

using namespace aether;

namespace {

bool BigFloor(const ModelRenderer& m, std::vector<Vec3>& v, std::vector<u32>& i) {
    if (std::strcmp(m.asset_path, "big") != 0) return false;
    nav::NavGeometry g;
    g.AddPlane(Vec3(0, 0, 0), 40, 40);
    v = g.vertices;
    i = g.indices;
    return true;
}

struct CrowdState {
    World world;
    std::unique_ptr<nav::NavWorld> nav;
    std::unique_ptr<nav::NavCrowd> crowd;
};
CrowdState& Crowd() {
    static CrowdState s;
    return s;
}

struct PathState {
    nav::NavMesh mesh;
    nav::HierarchicalPathfinder hierarchical;
    nav::NavPath path;
};
PathState& Paths() {
    static PathState s;
    if (!s.mesh.Loaded()) { // built on first use, so either benchmark can run alone
        nav::NavGeometry g;
        g.AddPlane(Vec3(0, 0, 0), 60, 60);
        nav::NavMeshSettings settings;
        settings.tile_size = 40;
        nav::NavMeshData data;
        if (!nav::BuildNavMesh(g, settings, data) || !s.mesh.Load(data) || !s.hierarchical.Build(s.mesh)) std::abort();
    }
    return s;
}

} // namespace

AETHER_BENCH_SETUP(
    "nav/crowd_1000_agents_30_frames", 3,
    [] {
        CrowdState& s = Crowd();
        s.nav = std::make_unique<nav::NavWorld>(s.world, BigFloor);
        ModelRenderer floor;
        SetModelPath(floor, "big");
        s.world.CreateEntity(Transform{Vec3(0, 0, 0), Quaternion::Identity()}, floor);
        nav::NavMeshSettings settings;
        settings.tile_size = 60;
        if (!s.nav->Bake(settings)) std::abort();
        s.crowd = std::make_unique<nav::NavCrowd>(s.world, *s.nav, 1024);
        for (int i = 0; i < 1000; ++i) {
            const f32 x = -38.0f + static_cast<f32>(i % 40) * 1.9f, z = -38.0f + static_cast<f32>(i / 40) * 1.2f;
            NavAgent a;
            a.radius = 0.4f;
            a.avoidance_quality = 1;
            const Entity e = s.world.CreateEntity(Transform{Vec3(x, 0, z), Quaternion::Identity()}, a);
            s.world.GetComponent<NavAgent>(e)->MoveTo(Vec3(x, 0, z + 30.0f));
        }
    },
    [] {
        CrowdState& s = Crowd();
        for (int frame = 0; frame < 30; ++frame) {
            s.nav->Update(0);
            s.crowd->Update(1.0f / 30.0f);
        }
        if (s.crowd->AgentCount() != 1000) std::abort();
    });

AETHER_BENCH_SETUP(
    "nav/path_across_100_tiles_plain", 5, [] { (void)Paths(); },
    [] {
        PathState& s = Paths();
        for (int i = 0; i < 20; ++i)
            if (!s.mesh.FindPath(Vec3(-50, 0, -50), Vec3(50, 0, 50), s.path)) std::abort();
    });

AETHER_BENCH_SETUP(
    "nav/path_across_100_tiles_hierarchical", 5, [] { (void)Paths(); },
    [] {
        PathState& s = Paths();
        for (int i = 0; i < 20; ++i)
            if (!s.hierarchical.FindPath(Vec3(-50, 0, -50), Vec3(50, 0, 50), s.path)) std::abort();
    });
