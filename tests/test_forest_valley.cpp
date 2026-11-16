#include "../player/src/camera_path.h"
#include "../player/src/terrain_view.h"
#include "../player/src/vegetation_batch.h"
#include "forest_valley.h"
#include "valley_terrain.h"
#include "test_framework.h"

#include <cmath>
#include <cstdio>

using namespace aether;
using namespace aether::forest_valley;

namespace {

// Generating the valley takes seconds, so every test shares one.
const World& Shared() {
    static const World world = BuildWorld();
    return world;
}

} // namespace

AETHER_TEST(ForestValley_WorldIsFiveHundredTwelveMetresAndDeterministic) {
    const World& w = Shared();
    AETHER_CHECK(w.terrain.Width() == 513 && w.terrain.Height() == 513 && w.settings.cell_size == 1.0f);
    const World again = BuildWorld();
    AETHER_CHECK(again.terrain.heightmap.heights == w.terrain.heightmap.heights);
    AETHER_CHECK(again.route.size() == w.route.size());
    for (u32 i = 0; i < kAreaCount; ++i) {
        AETHER_CHECK(again.areas[i].center.x == w.areas[i].center.x && again.areas[i].center.z == w.areas[i].center.z);
    }
}

AETHER_TEST(ForestValley_FiveAreasInsideTheWorldAndApart) {
    const World& w = Shared();
    for (u32 i = 0; i < kAreaCount; ++i) {
        const Vec3 c = w.areas[i].center;
        std::printf("  area %u %-20s (%.0f, %.1f, %.0f)\n", i, AreaName(w.areas[i].id), static_cast<double>(c.x), static_cast<double>(c.y), static_cast<double>(c.z));
        AETHER_CHECK(w.areas[i].id == static_cast<AreaId>(i));
        AETHER_CHECK(c.x > 0 && c.z > 0 && c.x < kWorldSize && c.z < kWorldSize);
        for (u32 j = i + 1; j < kAreaCount; ++j) {
            const Vec3 d = c - w.areas[j].center;
            AETHER_CHECK(std::hypot(d.x, d.z) >= 60.0f);
        }
    }
    // The overlook is the high ground and the meadow the low: that is what makes the first a viewpoint and the second a spawn.
    AETHER_CHECK(w.GetArea(AreaId::Overlook).center.y > w.GetArea(AreaId::Meadow).center.y + 10.0f);
}

AETHER_TEST(ForestValley_AllAreasAreJoinedByWalkableGround) {
    const World& w = Shared();
    std::printf("  route %.0f m, %zu points, connected=%d\n", static_cast<double>(w.route_length), w.route.size(), w.connected ? 1 : 0);
    AETHER_CHECK(w.connected);
    if (w.route.size() < 2) return;
    for (const Vec3& p : w.route) AETHER_CHECK(w.Walkable(p.x, p.z));
    AETHER_CHECK(std::hypot(w.route.front().x - w.GetArea(AreaId::Meadow).center.x, w.route.front().z - w.GetArea(AreaId::Meadow).center.z) < 3.0f);
    AETHER_CHECK(std::hypot(w.route.back().x - w.GetArea(AreaId::Overlook).center.x, w.route.back().z - w.GetArea(AreaId::Overlook).center.z) < 3.0f);
}

AETHER_TEST(ForestValley_WalkingTheRouteTakesFiveToTenMinutes) {
    const World& w = Shared();
    // The plan's target, checked at a walking pace so it is a statement about the world and not the runner.
    const f32 minutes = w.route_length / 3.5f / 60.0f;
    std::printf("  walking the route at 3.5 m/s: %.1f minutes\n", static_cast<double>(minutes));
    AETHER_CHECK(minutes >= 5.0f && minutes <= 10.0f);
}

AETHER_TEST(ForestValley_WalkerFollowsTheGroundRunsAndJumps) {
    const World& w = Shared();
    const Vec3 start = w.GetArea(AreaId::Meadow).center;
    Walker walker(w, start);
    WalkerInput walk;
    walk.move_x = 1.0f;
    for (int i = 0; i < 180; ++i) walker.Step(walk);
    AETHER_CHECK(walker.Grounded());
    AETHER_CHECK_NEAR(walker.Position().y, w.HeightAt(walker.Position().x, walker.Position().z), 0.01);
    const f32 walked = walker.Distance();
    AETHER_CHECK(walked > 8.0f && walked < 3.5f * 3.0f + 0.1f); // three seconds at the walking pace, never above it

    Walker runner(w, start);
    walk.run = true;
    for (int i = 0; i < 180; ++i) runner.Step(walk);
    AETHER_CHECK(runner.Distance() > walked + 3.0f);

    Walker jumper(w, start);
    for (int i = 0; i < 30; ++i) jumper.Step(WalkerInput{});
    const f32 ground = jumper.Position().y;
    WalkerInput jump;
    jump.jump = true;
    f32 peak = ground;
    for (int i = 0; i < 90; ++i) {
        jumper.Step(jump);
        peak = std::max(peak, jumper.Position().y);
    }
    AETHER_CHECK(peak > ground + 0.8f && peak < ground + 2.5f);
}

AETHER_TEST(ForestValley_WalkerIsStoppedByWaterAndByTheWorldEdge) {
    const World& w = Shared();
    Walker walker(w, w.GetArea(AreaId::Meadow).center);
    WalkerInput dir;
    dir.move_x = -1.0f;
    dir.run = true;
    for (int i = 0; i < 60 * 120; ++i) walker.Step(dir);
    AETHER_CHECK(walker.Position().x >= 0.0f);
    AETHER_CHECK(w.Walkable(walker.Position().x, walker.Position().z));
}

AETHER_TEST(ForestValley_BenchmarkCameraPathIsRepeatableAndLoadsInThePlayer) {
    const World& w = Shared();
    const std::string a = BenchmarkCameraPathJson(w), b = BenchmarkCameraPathJson(w);
    AETHER_CHECK(a == b);
    player::CameraPath path;
    std::string error;
    AETHER_CHECK(player::ParseCameraPath(a, path, &error));
    if (path.keys.size() < 2) return;
    AETHER_CHECK(path.fps == 30.0f);
    // At 4 m/s the run lasts route_length / 4 seconds.
    AETHER_CHECK_NEAR(path.Duration(), w.route_length / 4.0f, 2.0);
    // The camera stays above the ground along its whole length.
    for (u32 i = 0; i < path.FrameCount(); ++i) {
        const player::CameraKey k = player::SampleCameraPath(path, static_cast<f32>(i) / path.fps);
        AETHER_CHECK(k.position.y > w.HeightAt(k.position.x, k.position.z) + 0.3f);
    }
}

AETHER_TEST(ForestValley_PercentilesAreNearestRank) {
    std::vector<f32> v;
    for (int i = 1; i <= 100; ++i) v.push_back(static_cast<f32>(i));
    AETHER_CHECK(Percentile(v, 0.95) == 95.0f);
    AETHER_CHECK(Percentile(v, 0.99) == 99.0f);
    AETHER_CHECK(Percentile({}, 0.95) == 0.0f);
}

AETHER_TEST(ForestValley_GatesPassOnlyWhenMeasuredAndWithinBudget) {
    const std::vector<f32> good(200, 10.0f);
    AETHER_CHECK(EvaluateGates(good, good).All());
    // One slow frame in ten breaks p95 and p99 alike.
    std::vector<f32> spiky(100, 10.0f);
    for (int i = 0; i < 10; ++i) spiky[static_cast<usize>(i) * 10] = 30.0f;
    const GateResult spike = EvaluateGates(spiky, good);
    AETHER_CHECK(!spike.cpu_p95 && !spike.p99 && spike.gpu_p95);
    // No samples is not a pass.
    AETHER_CHECK(!EvaluateGates({}, {}).cpu_p95 && !EvaluateGates({}, {}).All());
    AETHER_CHECK(!EvaluateGates(good, {}).All());
}

AETHER_TEST(ForestValley_ScatterStaysInsideTheAssetBudget) {
    const World& w = Shared();
    // The rules the player uses for the scene's component: one source for the budget, not a copy here.
    const player::VegetationRules vegetation = player::MakeVegetationRules(ValleyTerrainComponent());
    terrain::ScatterSettings s;
    s.seed = kShowcaseSeed;
    const auto instances = terrain::Scatter(w.terrain, w.settings, vegetation.rules, s);
    u32 trees = 0, plants = 0, rocks = 0;
    for (const auto& inst : instances) {
        switch (vegetation.kinds[inst.rule]) {
        case player::PlantKind::Pine:
        case player::PlantKind::Birch: ++trees; break;
        case player::PlantKind::Boulder: ++rocks; break;
        default: ++plants; break;
        }
    }
    std::printf("  scatter: %u trees, %u plants, %u rocks (budget 800-1500, 12000-30000, 500-1000)\n", trees, plants, rocks);
    AETHER_CHECK(trees <= 1500 && plants <= 30000 && rocks <= 1000);
    AETHER_CHECK(trees >= 800 && plants >= 12000 && rocks >= 500);
}

namespace {
const ValleyTerrain& SharedTerrain() {
    static const ValleyTerrain v = BuildValleyTerrain(Shared());
    return v;
}
// The splatmap texel's weights (grass, rock, sand, snow) at a world position.
std::array<u8, 4> SplatAt(const ValleyTerrain& v, f32 x, f32 z) {
    const u32 px = static_cast<u32>(std::lround(x / kWorldSize * static_cast<f32>(v.splatmap.width - 1)));
    const u32 pz = static_cast<u32>(std::lround(z / kWorldSize * static_cast<f32>(v.splatmap.height - 1)));
    const usize i = (static_cast<usize>(pz) * v.splatmap.width + px) * 4;
    return {v.splatmap.pixels[i], v.splatmap.pixels[i + 1], v.splatmap.pixels[i + 2], v.splatmap.pixels[i + 3]};
}
} // namespace

AETHER_TEST(ForestValley_TerrainIsSixteenBySixteenChunksWithAFullSplatmap) {
    const ValleyTerrain& v = SharedTerrain();
    AETHER_CHECK(v.data.chunk_count_x == 16 && v.data.chunk_count_z == 16);
    AETHER_CHECK(v.layers.size() == 4);
    AETHER_CHECK(v.splatmap.width == kSplatResolution && v.splatmap.height == kSplatResolution);
    AETHER_CHECK(v.splatmap.pixels.size() == static_cast<usize>(kSplatResolution) * kSplatResolution * 4);
}

AETHER_TEST(ForestValley_SplatmapPaintsGrassAtTheMeadowAndSnowAtTheOverlook) {
    const World& w = Shared();
    const ValleyTerrain& v = SharedTerrain();
    const Vec3 m = w.GetArea(AreaId::Meadow).center, o = w.GetArea(AreaId::Overlook).center;
    const auto at_meadow = SplatAt(v, m.x, m.z), at_overlook = SplatAt(v, o.x, o.z);
    std::printf("  meadow weights  grass %u rock %u sand %u snow %u\n  overlook weights grass %u rock %u sand %u snow %u\n", at_meadow[0], at_meadow[1],
                at_meadow[2], at_meadow[3], at_overlook[0], at_overlook[1], at_overlook[2], at_overlook[3]);
    // The meadow is grass, the first layer: not sand (the beach) and not snow.
    AETHER_CHECK(at_meadow[0] > at_meadow[1] && at_meadow[0] > at_meadow[3]);
    AETHER_CHECK(at_meadow[0] >= 2 * at_meadow[2]); // clearly grass, not a coin toss with sand
    AETHER_CHECK(at_overlook[3] > 0);
    AETHER_CHECK(at_overlook[3] > at_meadow[3]);
}

AETHER_TEST(ForestValley_FramePlanUsesLodAndCostsLessThanFullDetail) {
    const World& w = Shared();
    const ValleyTerrain& v = SharedTerrain();
    const Vec3 eye = w.GetArea(AreaId::Meadow).center + Vec3(0, 1.7f, 0);
    const TerrainFramePlan plan = PlanTerrainFrame(v, eye);
    std::printf("  frame plan at the meadow: %u chunks, LOD0 %u LOD1 %u LOD2 %u LOD3 %u, %llu triangles, %.1f MB\n", plan.chunks, plan.lod_chunks[0],
                plan.lod_chunks[1], plan.lod_chunks[2], plan.lod_chunks[3], static_cast<unsigned long long>(plan.triangles),
                static_cast<double>(plan.gpu_bytes) / 1048576.0);
    AETHER_CHECK(plan.chunks == 256);
    AETHER_CHECK(plan.lod_chunks[0] > 0); // the ground under the camera is at full detail
    u32 coarse = 0;
    for (usize i = 1; i < plan.lod_chunks.size(); ++i) coarse += plan.lod_chunks[i];
    AETHER_CHECK(coarse > 0); // and the far valley is not
    const u64 full_detail = 256ull * 32 * 32 * 2; // every chunk at LOD 0
    AETHER_CHECK(plan.triangles < full_detail);
    // Deterministic: the same camera gives the same plan.
    const TerrainFramePlan again = PlanTerrainFrame(v, eye);
    AETHER_CHECK(again.triangles == plan.triangles && again.gpu_bytes == plan.gpu_bytes);
}

AETHER_TEST(ForestValley_TheScenesTerrainComponentRegeneratesTheWorldsGround) {
    // What the player draws must be the ground the areas, route and walker were built on, not a lookalike.
    const World& w = Shared();
    const ProceduralTerrain component = ValleyTerrainComponent();
    const terrain::GeneratedTerrain regenerated = terrain::GenerateTerrain(player::ToGenSettings(component));
    AETHER_CHECK(regenerated.heightmap.width == w.terrain.heightmap.width);
    AETHER_CHECK(regenerated.heightmap.heights == w.terrain.heightmap.heights);
    AETHER_CHECK(component.samples == 513 && component.cell_size == 1.0f);
}
