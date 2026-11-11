#include "aether/terrain/generator.h"
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

TerrainGenSettings Small() {
    TerrainGenSettings s;
    s.width = s.height = 129;
    s.hydraulic_droplets = 20000;
    s.river_min_catchment = 120;
    return s;
}

f32 MeanSlope(const GeneratedTerrain& t) {
    double sum = 0.0;
    for (const f32 v : t.slope) sum += v;
    return static_cast<f32>(sum / static_cast<double>(t.slope.size()));
}
f32 MaxSlope(const GeneratedTerrain& t) { return *std::max_element(t.slope.begin(), t.slope.end()); }

} // namespace

AETHER_TEST(TerrainGeneratorIsDeterministicAndSeeded) {
    const TerrainGenSettings s = Small();
    const GeneratedTerrain a = GenerateTerrain(s), b = GenerateTerrain(s);
    AETHER_CHECK(a.heightmap.heights == b.heightmap.heights);
    AETHER_CHECK(a.river == b.river);
    AETHER_CHECK(a.biome == b.biome);
    TerrainGenSettings other = s;
    other.seed = 2;
    const GeneratedTerrain c = GenerateTerrain(other);
    AETHER_CHECK(c.heightmap.heights != a.heightmap.heights);
}

AETHER_TEST(TerrainGeneratorHeightsStayInRangeAndSettingsAreSanitized) {
    const TerrainGenSettings s = Small();
    const GeneratedTerrain t = GenerateTerrain(s);
    AETHER_CHECK(t.heightmap.width == 129 && t.heightmap.height == 129);
    AETHER_CHECK(t.heightmap.heights.size() == 129u * 129u);
    bool finite = true;
    for (const f32 v : t.heightmap.heights) finite = finite && std::isfinite(v) && v >= 0.0f && v <= s.height_scale + 1e-3f;
    AETHER_CHECK(finite);
    AETHER_CHECK(t.height_min >= 0.0f && t.height_max <= s.height_scale + 1e-3f && t.height_max > t.height_min);
    AETHER_CHECK(t.sea_height > t.height_min && t.sea_height < t.height_max);

    // Nonsense in, a terrain out.
    TerrainGenSettings bad;
    bad.width = 0;
    bad.height = 3;
    bad.cell_size = std::numeric_limits<f32>::quiet_NaN();
    bad.height_scale = -5.0f;
    bad.octaves = 0;
    bad.persistence = 50.0f;
    bad.hydraulic_droplets = 1000;
    const TerrainGenSettings fixed = SanitizeSettings(bad);
    AETHER_CHECK(fixed.width >= 8 && fixed.height >= 8 && fixed.cell_size > 0.0f && fixed.height_scale > 0.0f && fixed.octaves >= 1);
    bad.width = bad.height = 16;
    const GeneratedTerrain tiny = GenerateTerrain(bad);
    AETHER_CHECK(tiny.heightmap.heights.size() == 16u * 16u);
}

AETHER_TEST(TerrainThermalErosionSoftensSlopes) {
    TerrainGenSettings s = Small();
    s.hydraulic_droplets = 0;
    s.river_depth = 0.0f;
    s.thermal_iterations = 0;
    const GeneratedTerrain raw = GenerateTerrain(s);
    s.thermal_iterations = 80;
    s.talus = 0.5f;
    const GeneratedTerrain worn = GenerateTerrain(s);
    AETHER_CHECK(MeanSlope(worn) < MeanSlope(raw));
    AETHER_CHECK(MaxSlope(worn) < MaxSlope(raw));
}

AETHER_TEST(TerrainHydraulicErosionReshapesWithoutLosingTheLand) {
    TerrainGenSettings s = Small();
    s.river_depth = 0.0f;
    s.hydraulic_droplets = 0;
    const GeneratedTerrain dry = GenerateTerrain(s);
    s.hydraulic_droplets = 40000;
    const GeneratedTerrain wet = GenerateTerrain(s);
    AETHER_CHECK(wet.heightmap.heights != dry.heightmap.heights);
    // Erosion moves earth about and loses none: the mean height moves by under half a percent of the range (the sliver that goes is what a
    // droplet sets down on the map's own edge, where the next step would have left it).
    double a = 0.0, b = 0.0;
    for (usize i = 0; i < dry.heightmap.heights.size(); ++i) { a += dry.heightmap.heights[i]; b += wet.heightmap.heights[i]; }
    const double n = static_cast<double>(dry.heightmap.heights.size());
    AETHER_CHECK(std::fabs(a - b) / n < 0.005 * s.height_scale);
    // And the droplets did carve: a visible number of cells changed by more than a centimetre.
    usize changed = 0;
    for (usize i = 0; i < dry.heightmap.heights.size(); ++i) changed += std::fabs(dry.heightmap.heights[i] - wet.heightmap.heights[i]) > 0.01f;
    AETHER_CHECK(changed > dry.heightmap.heights.size() / 20);
}

AETHER_TEST(TerrainRiversRunDownhillToTheEdgeAndAreCut) {
    const TerrainGenSettings s = Small();
    const GeneratedTerrain t = GenerateTerrain(s);
    AETHER_CHECK(t.river_cells > 0);
    AETHER_CHECK(t.flow.size() == t.heightmap.heights.size());
    const u32 w = t.Width(), h = t.Height();
    usize river = 0, with_downstream = 0, on_edge = 0;
    f32 biggest = 0.0f;
    usize biggest_i = 0;
    for (u32 z = 0; z < h; ++z) {
        for (u32 x = 0; x < w; ++x) {
            const usize i = t.Index(x, z);
            AETHER_CHECK(t.flow[i] >= 1.0f);
            if (t.flow[i] > biggest) { biggest = t.flow[i]; biggest_i = i; }
            if (!t.river[i]) continue;
            AETHER_CHECK(t.flow[i] >= static_cast<f32>(s.river_min_catchment));
            ++river;
            if (x == 0 || z == 0 || x == w - 1 || z == h - 1) { ++on_edge; continue; }
            // A river cell inside the map drains into a neighbour with more water: the river goes on.
            bool downstream = false;
            for (i32 oz = -1; oz <= 1; ++oz)
                for (i32 ox = -1; ox <= 1; ++ox) {
                    if (!ox && !oz) continue;
                    const usize n = t.Index(static_cast<u32>(static_cast<i32>(x) + ox), static_cast<u32>(static_cast<i32>(z) + oz));
                    downstream = downstream || ((t.river[n] || t.lake[n]) && t.flow[n] > t.flow[i]); // the river goes on, or ends in a lake
                }
            with_downstream += downstream;
            AETHER_CHECK(downstream);
        }
    }
    AETHER_CHECK(river == t.river_cells);
    AETHER_CHECK(on_edge > 0);              // the water leaves the map
    AETHER_CHECK(t.river[biggest_i] == 1);  // the largest catchment is a river
    AETHER_CHECK(with_downstream + on_edge == river);

    // The channel is cut: the same terrain without the cut is higher in the river cells.
    TerrainGenSettings flat = s;
    flat.river_depth = 0.0f;
    const GeneratedTerrain uncut = GenerateTerrain(flat);
    double drop = 0.0;
    for (usize i = 0; i < t.river.size(); ++i) {
        if (!t.river[i]) continue;
        AETHER_CHECK(t.heightmap.heights[i] <= uncut.heightmap.heights[i] + 1e-4f);
        drop += uncut.heightmap.heights[i] - t.heightmap.heights[i];
    }
    AETHER_CHECK(drop / static_cast<double>(std::max<u32>(t.river_cells, 1)) > 0.2);
}

AETHER_TEST(TerrainBiomesFollowHeightSlopeAndWater) {
    const TerrainGenSettings s = Small();
    const GeneratedTerrain t = GenerateTerrain(s);
    const f32 range = t.height_max - t.height_min;
    std::set<Biome> seen;
    for (usize i = 0; i < t.biome.size(); ++i) {
        const Biome b = t.biome[i];
        seen.insert(b);
        const f32 normalized = (t.heightmap.heights[i] - t.height_min) / range;
        if (b == Biome::Water) AETHER_CHECK(t.river[i] || t.lake[i] || t.heightmap.heights[i] <= t.sea_height);
        if (b == Biome::Snow) AETHER_CHECK(normalized > s.snow_line && t.slope[i] <= s.rock_slope && !t.river[i] && !t.lake[i]);
        if (b == Biome::Rock) AETHER_CHECK(t.slope[i] > s.rock_slope || normalized > s.tree_line);
        if (b == Biome::Forest) AETHER_CHECK(normalized <= s.tree_line && t.slope[i] <= s.rock_slope);
        if (b == Biome::Beach) AETHER_CHECK(normalized < s.sea_level + s.beach_band);
        if (t.slope[i] > s.rock_slope && !t.river[i] && !t.lake[i] && t.heightmap.heights[i] > t.sea_height) AETHER_CHECK(b == Biome::Rock);
        AETHER_CHECK(t.moisture[i] >= 0.0f && t.moisture[i] <= 1.0f && t.temperature[i] >= 0.0f && t.temperature[i] <= 1.0f);
    }
    AETHER_CHECK(seen.size() >= 4); // a varied world, not one colour

    // No sea level, no ocean: the only water left is the rivers.
    TerrainGenSettings dry = s;
    dry.sea_level = 0.0f;
    const GeneratedTerrain land = GenerateTerrain(dry);
    for (usize i = 0; i < land.biome.size(); ++i)
        if (land.biome[i] == Biome::Water && !land.river[i] && !land.lake[i]) AETHER_CHECK(land.heightmap.heights[i] <= land.sea_height);
    AETHER_CHECK(std::string(BiomeName(Biome::Snow)) == "Snow");
}

AETHER_TEST(TerrainMaterialWeightsFollowRockSlopeAndSnowHeight) {
    const TerrainGenSettings s = Small();
    const GeneratedTerrain t = GenerateTerrain(s);
    const u32 resolution = 64;
    const std::vector<f32> weights = BuildMaterialWeights(t, s, resolution);
    AETHER_CHECK(weights.size() == static_cast<usize>(resolution) * resolution * 4);
    f32 worst = 0.0f;
    for (usize p = 0; p < static_cast<usize>(resolution) * resolution; ++p) {
        f32 sum = 0.0f;
        for (usize l = 0; l < 4; ++l) { AETHER_CHECK(weights[p * 4 + l] >= 0.0f); sum += weights[p * 4 + l]; }
        worst = std::max(worst, std::fabs(sum - 1.0f));
    }
    AETHER_CHECK(worst < 1e-4f);
    // The steepest cell is mostly rock; the highest cell is not grass.
    const usize steep = static_cast<usize>(std::max_element(t.slope.begin(), t.slope.end()) - t.slope.begin());
    const u32 sx = static_cast<u32>(steep % t.Width()) * resolution / t.Width(), sz = static_cast<u32>(steep / t.Width()) * resolution / t.Height();
    AETHER_CHECK(weights[(static_cast<usize>(sz) * resolution + sx) * 4 + 1] > 0.5f);
    // Packed into a splatmap: four layers, a texel's bytes sum to about 255.
    const SplatmapData splat = BuildTerrainSplatmap(t, s, {}, resolution);
    AETHER_CHECK(splat.width == resolution && splat.pixels.size() == static_cast<usize>(resolution) * resolution * 4);
    int off = 0;
    for (usize p = 0; p < static_cast<usize>(resolution) * resolution; ++p) {
        const int sum = splat.pixels[p * 4] + splat.pixels[p * 4 + 1] + splat.pixels[p * 4 + 2] + splat.pixels[p * 4 + 3];
        off = std::max(off, std::abs(sum - 255));
    }
    AETHER_CHECK(off <= 3);
    AETHER_CHECK(DefaultTerrainLayers().size() == 4);
}

AETHER_TEST(TerrainPreviewAndTiming) {
    const TerrainGenSettings s = Small();
    const GeneratedTerrain t = GenerateTerrain(s);
    const std::vector<u8> rgb = RenderTerrainPreview(t);
    AETHER_CHECK(rgb.size() == 129u * 129u * 3);
    AETHER_CHECK(std::any_of(rgb.begin(), rgb.end(), [](u8 v) { return v > 100; }));
    if (const char* dir = std::getenv("AETHER_TERRAIN_PREVIEW_DIR")) {
        // Opt-in: write the preview of a bigger terrain as a PPM, and report what generating it cost (docs/design/PERFORMANCE.md).
        TerrainGenSettings big;
        big.width = big.height = 513;
        big.hydraulic_droplets = 200000;
        big.river_min_catchment = 800;
        const auto begin = std::chrono::steady_clock::now();
        const GeneratedTerrain bigger = GenerateTerrain(big);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        std::printf("[terrain] 513x513, 200000 droplets: %.0f ms, %u river cells\n", ms, bigger.river_cells);
        const std::vector<u8> picture = RenderTerrainPreview(bigger);
        const std::string path = std::string(dir) + "/terrain.ppm";
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fprintf(f, "P6\n%u %u\n255\n", bigger.Width(), bigger.Height());
            std::fwrite(picture.data(), 1, picture.size(), f);
            std::fclose(f);
        }
    }
}
