#include "aether/terrain/scatter.h"
#include "test_framework.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <set>

using namespace aether;
using namespace aether::terrain;

namespace {

TerrainGenSettings World() {
    TerrainGenSettings s;
    s.width = s.height = 129;
    s.hydraulic_droplets = 20000;
    s.river_min_catchment = 120;
    return s;
}

const GeneratedTerrain& SharedTerrain() {
    static const GeneratedTerrain t = GenerateTerrain(World());
    return t;
}

f32 SlopeAt(const Heightmap& hm, f32 x, f32 z) {
    const Vec3 n = TerrainNormalAt(hm, x, z);
    return std::sqrt(std::max(1.0f - n.y * n.y, 0.0f)) / std::max(n.y, 1e-4f);
}

f32 MeanNearest(const std::vector<ScatterInstance>& v) {
    double sum = 0.0;
    for (usize i = 0; i < v.size(); ++i) {
        f32 best = std::numeric_limits<f32>::max();
        for (usize j = 0; j < v.size(); ++j) {
            if (i == j) continue;
            const f32 dx = v[i].position.x - v[j].position.x, dz = v[i].position.z - v[j].position.z;
            best = std::min(best, dx * dx + dz * dz);
        }
        sum += std::sqrt(best);
    }
    return static_cast<f32>(sum / static_cast<double>(std::max<usize>(v.size(), 1)));
}

ScatterRule Open(const char* name) {
    ScatterRule r;
    r.name = name;
    r.slope_max = 100.0f;
    r.footprint = 0.0f;
    return r;
}

} // namespace

AETHER_TEST(ScatterIsDeterministicAndEachRuleHasItsOwnStream) {
    const GeneratedTerrain& t = SharedTerrain();
    std::vector<ScatterRule> rules = DefaultScatterRules();
    ScatterSettings settings;
    settings.seed = 7;
    const std::vector<ScatterInstance> a = Scatter(t, World(), rules, settings), b = Scatter(t, World(), rules, settings);
    AETHER_CHECK(!a.empty() && a.size() == b.size());
    bool same = a.size() == b.size();
    for (usize i = 0; same && i < a.size(); ++i) same = a[i].position.x == b[i].position.x && a[i].position.z == b[i].position.z && a[i].scale == b[i].scale && a[i].rule == b[i].rule;
    AETHER_CHECK(same);
    settings.seed = 8;
    const std::vector<ScatterInstance> c = Scatter(t, World(), rules, settings);
    AETHER_CHECK(c.size() != a.size() || c[0].position.x != a[0].position.x);

    // A rule added at the end leaves every earlier rule's instances where they were.
    rules.push_back(Open("Extra"));
    rules.back().density = 5.0f;
    settings.seed = 7;
    const std::vector<ScatterInstance> d = Scatter(t, World(), rules, settings);
    AETHER_CHECK(d.size() > a.size());
    bool prefix = true;
    for (usize i = 0; i < a.size(); ++i) prefix = prefix && a[i].position.x == d[i].position.x && a[i].position.z == d[i].position.z && a[i].rule == d[i].rule;
    AETHER_CHECK(prefix);
}

AETHER_TEST(ScatterObeysBiomeHeightSlopeAndWaterRules) {
    const GeneratedTerrain& t = SharedTerrain();
    const TerrainGenSettings world = World();
    const std::vector<ScatterRule> rules = DefaultScatterRules();
    const std::vector<ScatterInstance> out = Scatter(t, world, rules, {});
    AETHER_CHECK(out.size() > 100);
    const std::vector<f32> water = WaterDistanceField(t);
    const f32 range = t.height_max - t.height_min;
    const f32 cell = t.heightmap.cell_size;
    const f32 size = static_cast<f32>(t.Width() - 1) * cell;
    std::set<u32> used;
    for (const ScatterInstance& s : out) {
        AETHER_CHECK(s.rule < rules.size());
        used.insert(s.rule);
        const ScatterRule& r = rules[s.rule];
        AETHER_CHECK(s.position.x >= 0.0f && s.position.x <= size && s.position.z >= 0.0f && s.position.z <= size);
        AETHER_CHECK((r.biomes & BiomeBit(s.biome)) != 0 && s.biome != Biome::Water);
        const f32 ground = t.heightmap.SampleLinear(s.position.x, s.position.z);
        AETHER_CHECK(std::fabs(s.position.y - (ground + r.y_offset)) < 1e-3f);
        const f32 normalized = (ground - t.height_min) / range;
        AETHER_CHECK(normalized >= r.height_min - 1e-4f && normalized <= r.height_max + 1e-4f);
        const f32 slope = SlopeAt(t.heightmap, s.position.x, s.position.z);
        AETHER_CHECK(slope >= r.slope_min - 1e-3f && slope <= r.slope_max + 1e-3f);
        const u32 cx = std::min(static_cast<u32>(s.position.x / cell + 0.5f), t.Width() - 1), cz = std::min(static_cast<u32>(s.position.z / cell + 0.5f), t.Height() - 1);
        const f32 d = water[t.Index(cx, cz)];
        AETHER_CHECK(d >= r.water_distance_min && d <= r.water_distance_max + 1e-3f);
        AETHER_CHECK(s.scale >= std::min(r.scale_min, r.scale_max) * 0.5f && s.scale <= std::max(r.scale_min, r.scale_max) + 1e-4f);
        AETHER_CHECK(std::fabs(s.rotation.x * s.rotation.x + s.rotation.y * s.rotation.y + s.rotation.z * s.rotation.z + s.rotation.w * s.rotation.w - 1.0f) < 1e-3f);
    }
    AETHER_CHECK(used.size() >= 5); // a varied scatter, not one kind of object
}

AETHER_TEST(ScatterKeepsFootprintsApartAndLaterRulesClearOfEarlierOnes) {
    const GeneratedTerrain& t = SharedTerrain();
    ScatterRule trees = Open("Trees");
    trees.density = 3.0f;
    trees.footprint = 2.0f;
    trees.overlap = 0.0f;
    trees.scale_min = 1.0f;
    trees.scale_max = 1.5f;
    ScatterRule shrubs = Open("Shrubs");
    shrubs.density = 6.0f;
    shrubs.footprint = 0.8f;
    shrubs.overlap = 0.0f;
    const std::vector<ScatterRule> rules = {trees, shrubs};
    const std::vector<ScatterInstance> out = Scatter(t, World(), rules, {});
    usize tree_count = 0, shrub_count = 0;
    for (const ScatterInstance& s : out) (s.rule == 0 ? tree_count : shrub_count)++;
    AETHER_CHECK(tree_count > 20 && shrub_count > 20);
    bool clear = true;
    for (usize i = 0; i < out.size(); ++i) {
        for (usize j = i + 1; j < out.size(); ++j) {
            const f32 dx = out[i].position.x - out[j].position.x, dz = out[i].position.z - out[j].position.z;
            const f32 need = (rules[out[i].rule].footprint * out[i].scale + rules[out[j].rule].footprint * out[j].scale);
            if (dx * dx + dz * dz < need * need - 1e-3f) clear = false;
        }
    }
    AETHER_CHECK(clear);

    // With overlap allowed the same rule packs tighter.
    trees.overlap = 0.9f;
    const std::vector<ScatterRule> packed = {trees};
    const usize with_overlap = Scatter(t, World(), packed, {}).size();
    trees.overlap = 0.0f;
    const std::vector<ScatterRule> apart = {trees};
    AETHER_CHECK(with_overlap > Scatter(t, World(), apart, {}).size());
}

AETHER_TEST(ScatterDensityClumpsPatchesAndCaps) {
    const GeneratedTerrain& t = SharedTerrain();
    ScatterRule one = Open("One");
    one.density = 1.0f;
    const usize base = Scatter(t, World(), std::vector<ScatterRule>{one}, {}).size();
    one.density = 2.0f;
    const usize doubled = Scatter(t, World(), std::vector<ScatterRule>{one}, {}).size();
    AETHER_CHECK(base > 50);
    AETHER_CHECK(doubled > base * 17 / 10 && doubled < base * 23 / 10); // twice the density, about twice the objects

    // Clumps: the same number of objects sit nearer each other.
    ScatterRule single = Open("Single");
    single.density = 0.6f;
    ScatterRule clumped = single;
    clumped.clump_size = 6;
    clumped.clump_radius = 2.5f;
    const std::vector<ScatterInstance> s = Scatter(t, World(), std::vector<ScatterRule>{single}, {});
    std::vector<ScatterInstance> c = Scatter(t, World(), std::vector<ScatterRule>{clumped}, {});
    AETHER_CHECK(s.size() > 30 && c.size() > 30);
    AETHER_CHECK(MeanNearest(c) < MeanNearest(s) * 0.6f);

    // Patchiness leaves ground bare.
    ScatterRule patchy = single;
    patchy.patch_scale = 4.0f;
    patchy.patch_threshold = 0.6f;
    AETHER_CHECK(Scatter(t, World(), std::vector<ScatterRule>{patchy}, {}).size() < s.size() * 3 / 4);

    // Caps: per rule and in all.
    ScatterRule capped = Open("Capped");
    capped.density = 50.0f;
    capped.max_instances = 123;
    const std::vector<ScatterInstance> limited = Scatter(t, World(), std::vector<ScatterRule>{capped}, {});
    AETHER_CHECK(limited.size() == 123);
    ScatterSettings total;
    total.max_total = 77;
    capped.max_instances = 100000;
    AETHER_CHECK(Scatter(t, World(), std::vector<ScatterRule>{capped}, total).size() == 77);
}

AETHER_TEST(ScatterWaterDistanceAndNearWaterRules) {
    const GeneratedTerrain& t = SharedTerrain();
    const std::vector<f32> d = WaterDistanceField(t);
    AETHER_CHECK(d.size() == t.heightmap.heights.size());
    usize wet = 0;
    for (usize i = 0; i < d.size(); ++i) {
        const bool is_water = t.biome[i] == Biome::Water || t.river[i] || t.lake[i];
        if (is_water) { ++wet; AETHER_CHECK(d[i] == 0.0f); }
        else AETHER_CHECK(d[i] > 0.0f);
    }
    AETHER_CHECK(wet > 0);
    // Neighbouring cells differ by at most one step (a cell and a half on a diagonal).
    f32 worst = 0.0f;
    const u32 w = t.Width();
    for (usize i = 0; i + 1 < d.size(); ++i) if (d[i] < 1e8f && d[i + 1] < 1e8f && (i + 1) % w != 0) worst = std::max(worst, std::fabs(d[i] - d[i + 1]));
    AETHER_CHECK(worst <= t.heightmap.cell_size * 1.42f + 1e-3f);

    ScatterRule shore = Open("Shore");
    shore.density = 20.0f;
    shore.water_distance_max = 5.0f;
    shore.slope_max = 100.0f;
    const std::vector<ScatterInstance> out = Scatter(t, World(), std::vector<ScatterRule>{shore}, {});
    AETHER_CHECK(!out.empty());
    for (const ScatterInstance& s : out) {
        const u32 cx = std::min(static_cast<u32>(s.position.x / t.heightmap.cell_size + 0.5f), w - 1), cz = std::min(static_cast<u32>(s.position.z / t.heightmap.cell_size + 0.5f), t.Height() - 1);
        AETHER_CHECK(d[t.Index(cx, cz)] <= 5.0f + 1e-3f && d[t.Index(cx, cz)] > 0.0f);
    }
}

AETHER_TEST(ScatterOrientationAndFoliageConversion) {
    const GeneratedTerrain& t = SharedTerrain();
    ScatterRule flat = Open("Upright");
    flat.density = 4.0f;
    flat.align_to_normal = 0.0f;
    ScatterRule tilted = flat;
    tilted.align_to_normal = 1.0f;
    const std::vector<ScatterInstance> up = Scatter(t, World(), std::vector<ScatterRule>{flat}, {});
    const std::vector<ScatterInstance> lean = Scatter(t, World(), std::vector<ScatterRule>{tilted}, {});
    AETHER_CHECK(!up.empty() && !lean.empty());
    // The object's up (its rotation applied to +Y) is straight up, or the ground's normal.
    auto up_of = [](const Quaternion& q) {
        const Vec3 u(q.x, q.y, q.z);
        const Vec3 v(0, 1, 0);
        return v + u.Cross(u.Cross(v) + v * q.w) * 2.0f;
    };
    for (const ScatterInstance& s : up) AETHER_CHECK(up_of(s.rotation).y > 0.9999f);
    for (const ScatterInstance& s : lean) AETHER_CHECK((up_of(s.rotation) - s.normal).Length() < 1e-3f);

    const std::vector<FoliageInstance> foliage = ToFoliageInstances(lean, 32.0f);
    AETHER_CHECK(foliage.size() == lean.size());
    for (usize i = 0; i < lean.size(); ++i) {
        AETHER_CHECK(foliage[i].type_index == lean[i].rule);
        AETHER_CHECK(foliage[i].chunk_x == static_cast<u16>(lean[i].position.x / 32.0f) && foliage[i].chunk_z == static_cast<u16>(lean[i].position.z / 32.0f));
        AETHER_CHECK(foliage[i].scale.x == lean[i].scale);
    }
}

AETHER_TEST(ScatterHandlesEmptyInputs) {
    const GeneratedTerrain& t = SharedTerrain();
    AETHER_CHECK(Scatter(t, World(), std::span<const ScatterRule>{}, {}).empty());
    GeneratedTerrain none;
    AETHER_CHECK(Scatter(none, World(), DefaultScatterRules(), {}).empty());
    ScatterRule no_density = Open("None");
    no_density.density = 0.0f;
    AETHER_CHECK(Scatter(t, World(), std::vector<ScatterRule>{no_density}, {}).empty());
    AETHER_CHECK(ToFoliageInstances({}, 32.0f).empty());
}

AETHER_TEST(ScatterPreviewAndTiming) {
    if (const char* dir = std::getenv("AETHER_TERRAIN_PREVIEW_DIR")) {
        TerrainGenSettings big;
        big.width = big.height = 513;
        big.hydraulic_droplets = 200000;
        big.river_min_catchment = 800;
        const GeneratedTerrain terrain = GenerateTerrain(big);
        const auto begin = std::chrono::steady_clock::now();
        const std::vector<ScatterInstance> out = Scatter(terrain, big, DefaultScatterRules(), {});
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        std::vector<usize> counts(DefaultScatterRules().size(), 0);
        for (const ScatterInstance& s : out) ++counts[s.rule];
        std::printf("[scatter] 513x513 default rules: %zu instances in %.0f ms;", out.size(), ms);
        const std::vector<ScatterRule> rules = DefaultScatterRules();
        for (usize i = 0; i < counts.size(); ++i) std::printf(" %s %zu", rules[i].name.c_str(), counts[i]);
        std::printf("\n");
        std::vector<u8> rgb = RenderTerrainPreview(terrain);
        OverlayScatterPreview(rgb, terrain, out);
        const std::string path = std::string(dir) + "/scatter.ppm";
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fprintf(f, "P6\n%u %u\n255\n", terrain.Width(), terrain.Height());
            std::fwrite(rgb.data(), 1, rgb.size(), f);
            std::fclose(f);
        }
    }
}
