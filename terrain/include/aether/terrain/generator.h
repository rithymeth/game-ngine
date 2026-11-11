#pragma once

#include "aether/core/base.h"
#include "aether/terrain/splat.h"
#include "aether/terrain/terrain.h"

#include <span>
#include <vector>

namespace aether {
namespace terrain {

// Procedural terrain: a heightfield of mountains and valleys from layered noise, worn by thermal and hydraulic erosion, cut by rivers that
// follow the water downhill, and classified into biomes and material weights. Headless and deterministic: the same settings (seed
// included) give the same terrain on every machine, because nothing here reads a clock or a thread.
//
// The stages, in order (each can be switched off by its setting):
//   1. Shape: domain-warped fractal noise blended with ridged noise (the ridges are the mountain chains), normalised to 0..1, then
//      scaled by `height_scale` metres.
//   2. Thermal erosion: material slides off slopes steeper than `talus` into lower neighbours, which rounds the noise's spikes into scree.
//   3. Hydraulic erosion: `hydraulic_droplets` droplets run downhill, pick up sediment on steep ground and drop it where they slow, which
//      carves gullies and fills the valley floors.
//   4. Rivers: depressions are filled so every cell drains to the map's edge (those deeper than `lake_depth` become lakes), the water that reaches each cell is accumulated, and the
//      cells with at least `river_min_catchment` cells upstream are cut into channels.
//   5. Biomes and materials from height, slope, moisture (rain shadow aside: noise plus nearness to water) and temperature (cooler with height).

enum class Biome : u8 { Water, Beach, Desert, Grassland, Forest, Rock, Snow };
inline constexpr u32 kBiomeCount = 7;
const char* BiomeName(Biome biome);

struct TerrainGenSettings {
    u32 seed = 1;
    u32 width = 257;  // samples along X (at least 8)
    u32 height = 257; // samples along Z (at least 8)
    f32 cell_size = 1.0f; // metres per sample

    // Shape.
    f32 height_scale = 60.0f;  // metres from the lowest point to the highest
    f32 frequency = 2.5f;      // features across the map: the noise's lattice cells per map width
    u32 octaves = 6;
    f32 persistence = 0.5f;    // each octave's amplitude relative to the one before
    f32 ridge_amount = 0.55f;  // 0 rolling hills, 1 all mountain chains
    f32 warp = 0.25f;          // how far the noise coordinates are bent (a share of the map width), for winding rather than grid-aligned features
    f32 lowland_power = 1.4f;  // above 1 flattens the valleys and keeps the peaks

    // Erosion.
    u32 thermal_iterations = 20;
    f32 talus = 0.8f;                 // the steepest slope (rise over run) that stays put
    u32 hydraulic_droplets = 60000;   // 0 skips it

    // Rivers.
    u32 river_min_catchment = 400; // cells upstream before a cell is a river
    f32 river_depth = 0.04f;       // the deepest cut, as a share of height_scale
    f32 lake_depth = 0.25f;        // metres a depression must be filled by before it counts as a lake

    // Biomes.
    f32 sea_level = 0.18f;   // 0..1 of the height range: below it is water
    f32 beach_band = 0.03f;  // the height range above the sea that is sand
    f32 tree_line = 0.62f;   // above this no forest grows
    f32 snow_line = 0.82f;   // above this the ground is snow where it is not too steep to hold it
    f32 rock_slope = 0.9f;   // steeper than this (rise over run) is bare rock
};

struct GeneratedTerrain {
    Heightmap heightmap;         // metres
    f32 sea_height = 0.0f;       // metres: where `sea_level` falls in this terrain
    std::vector<f32> slope;      // rise over run at each cell
    std::vector<f32> flow;       // cells upstream of each cell, itself included
    std::vector<u8> river;       // 1 where a river runs
    std::vector<u8> lake;        // 1 where a depression holds standing water (not cut: the water sits level)
    std::vector<f32> moisture;   // 0..1
    std::vector<f32> temperature; // 0..1
    std::vector<Biome> biome;
    u32 river_cells = 0;
    u32 lake_cells = 0;
    f32 height_min = 0.0f, height_max = 0.0f; // metres, after carving

    u32 Width() const { return heightmap.width; }
    u32 Height() const { return heightmap.height; }
    usize Index(u32 x, u32 z) const { return static_cast<usize>(z) * heightmap.width + x; }
};

// Clamps the settings that would break a stage (sizes, ranges) and generates. Never fails.
TerrainGenSettings SanitizeSettings(const TerrainGenSettings& settings);
GeneratedTerrain GenerateTerrain(const TerrainGenSettings& settings);

// The material weights of four layers per pixel, row-major at `resolution` x `resolution` (the layout BuildSplatmap takes): grass, rock,
// sand, snow, each pixel normalised to sum to 1. Rock follows slope, snow follows height, sand the beaches and deserts, grass the rest.
std::vector<f32> BuildMaterialWeights(const GeneratedTerrain& terrain, const TerrainGenSettings& settings, u32 resolution);

// The splatmap for those weights, with the layers' texture and tint settings from `layers` (up to four).
SplatmapData BuildTerrainSplatmap(const GeneratedTerrain& terrain, const TerrainGenSettings& settings,
                                  std::span<const SplatmapLayer> layers, u32 resolution);

// Rock, grass, sand and snow as terrain layers with plain colours, for terrain with no textures yet.
std::vector<TerrainLayer> DefaultTerrainLayers();

// A shaded-relief picture, RGB8 row-major `width * height * 3` bytes: biome colours lit from the north-west, rivers blue. For the editor's
// preview and for checking a generator setting by eye.
std::vector<u8> RenderTerrainPreview(const GeneratedTerrain& terrain);

} // namespace terrain
} // namespace aether
