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
#include <string>
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

// A serpentine of walls: the case a hierarchical pathfinder is supposed to win,
// because one long plain search has to explore every dead end while the portal graph
// jumps between the gaps. Same shape as the unit test, scaled up.
struct SerpentineState {
    nav::NavMesh mesh;
    nav::HierarchicalPathfinder hierarchical;
    nav::NavPath path;
};
SerpentineState& Serpentine() {
    static SerpentineState s;
    if (!s.mesh.Loaded()) {
        nav::NavGeometry g;
        g.AddPlane(Vec3(0, 0, 0), 600, 600);
        for (int i = 0; i < 19; ++i) {
            const f32 x = -540.0f + static_cast<f32>(i) * 60.0f;
            const f32 z_center = (i % 2 == 0) ? 60.0f : -60.0f; // a gap at one end, then the other
            g.volumes.push_back(nav::NavVolume::Box(Vec3(x, 0, z_center), Vec3(2, 2, 540)));
        }
        nav::NavMeshSettings settings;
        settings.tile_size = 40;
        nav::NavMeshData data;
        std::string error;
        if (!nav::BuildNavMesh(g, settings, data, &error)) {
            std::fprintf(stderr, "serpentine bake failed: %s\n", error.c_str());
            std::abort();
        }
        if (!s.mesh.Load(data) || !s.hierarchical.Build(s.mesh)) {
            std::fprintf(stderr, "serpentine mesh setup failed\n");
            std::abort();
        }
    }
    return s;
}

PathState& Paths() {
    static PathState s;
    if (!s.mesh.Loaded()) { // built on first use, so either benchmark can run alone
        nav::NavGeometry g;
        // The bake sizes tiles as tile_size cells * cell_size (40 * 0.3 = 12 world
        // units here) and refuses more than 1 << 14 of them, so a 1400 x 1400 plane
        // is ~13 700 tiles -- about as large as this can go, and the path below
        // crosses ~115 of them. It used to be a 120 x 120 plane, where both
        // endpoints were a tile apart and the "hierarchical" case measured
        // essentially the same short path as the plain one.
        g.AddPlane(Vec3(0, 0, 0), 700, 700);
        // A wall across the middle with its only gap at the far end (z in 560..700).
        // Without it the plain path was a straight line -- Detour's easiest case --
        // and the hierarchy paid for its portal A* and leg searches with nothing to
        // save. The wall forces the plain search to work its way along it.
        g.AddBox(Vec3(0, 1.5f, -70), Vec3(5, 1.5f, 630));
        nav::NavMeshSettings settings;
        settings.tile_size = 40;
        nav::NavMeshData data;
        // Say which step failed: a bare abort gave no hint when the bake started
        // refusing larger grids (see the size note above).
        std::string error;
        if (!nav::BuildNavMesh(g, settings, data, &error)) {
            std::fprintf(stderr, "nav bake failed: %s\n", error.c_str());
            std::abort();
        }
        if (!s.mesh.Load(data)) {
            std::fprintf(stderr, "nav bake produced a mesh the runtime would not load\n");
            std::abort();
        }
        if (!s.hierarchical.Build(s.mesh)) {
            std::fprintf(stderr, "hierarchical pathfinder build failed\n");
            std::abort();
        }
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
    "nav/path_around_wall_plain", 5, [] { (void)Paths(); },
    [] {
        PathState& s = Paths();
        for (int i = 0; i < 20; ++i)
            if (!s.mesh.FindPath(Vec3(-650, 0, -650), Vec3(650, 0, 650), s.path)) std::abort();
    });

AETHER_BENCH_SETUP(
    "nav/path_around_wall_hierarchical", 5, [] { (void)Paths(); },
    [] {
        PathState& s = Paths();
        for (int i = 0; i < 20; ++i)
            if (!s.hierarchical.FindPath(Vec3(-650, 0, -650), Vec3(650, 0, 650), s.path)) std::abort();
        // The wall makes this a detour, so answering through the plain
        // path would mean the portal graph is being bypassed and the number below
        // would not be the hierarchical path at all.
        const nav::HierarchicalStats& stats = s.hierarchical.Stats();
        if (stats.direct != 0) std::abort();
        // Where the time goes: a graph with one node per portal means the A* cost
        // tracks world size rather than path length, which is what made this
        // benchmark tens of times slower than the plain path.
        static bool reported = false;
        if (!reported) {
            reported = true;
            std::fprintf(stderr, "  hierarchical graph: %zu portals, %zu edges, %zu nodes expanded, %zu legs\n",
                         stats.portals, stats.edges, stats.graph_nodes_expanded, stats.legs);
        }
    });

// The case the hierarchy exists for: one long route through 19 dead ends.
AETHER_BENCH_SETUP(
    "nav/path_through_serpentine_plain", 5, [] { (void)Serpentine(); },
    [] {
        SerpentineState& s = Serpentine();
        for (int i = 0; i < 20; ++i)
            if (!s.mesh.FindPath(Vec3(-580, 0, 0), Vec3(580, 0, 0), s.path)) std::abort();
    });

AETHER_BENCH_SETUP(
    "nav/path_through_serpentine_hierarchical", 5, [] { (void)Serpentine(); },
    [] {
        SerpentineState& s = Serpentine();
        for (int i = 0; i < 20; ++i)
            if (!s.hierarchical.FindPath(Vec3(-580, 0, 0), Vec3(580, 0, 0), s.path)) std::abort();
        static bool reported = false;
        if (!reported) {
            reported = true;
            const nav::HierarchicalStats& st = s.hierarchical.Stats();
            std::fprintf(stderr, "  serpentine hierarchical: %zu portals, %zu expanded, %zu legs, direct=%zu\n",
                         st.portals, st.graph_nodes_expanded, st.legs, st.direct);
        }
    });
