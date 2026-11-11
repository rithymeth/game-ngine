#pragma once

#include "aether/core/base.h"
#include "aether/math/math.h"
#include "aether/terrain/foliage.h"
#include "aether/terrain/generator.h"

#include <span>
#include <string>
#include <vector>

namespace aether {
namespace terrain {

// Environment scatter: places trees, shrubs, grass, rocks and debris over a generated terrain by rules. Each rule says where one kind of
// object may stand (biomes, height, slope, distance from water, a patchiness noise), how much of it there is, how big, how it faces and how
// much room it takes. Rules run in order, and a later rule keeps clear of what the earlier ones placed, so the order is the pecking order:
// trees first, then shrubs in the gaps, then grass between them. Deterministic from the seed: the same terrain, rules and seed give the
// same instances on any machine, whatever else changes in the list after a rule (each rule draws from its own random stream).

inline constexpr u32 BiomeBit(Biome biome) { return 1u << static_cast<u32>(biome); }
inline constexpr u32 kAllBiomes = (1u << kBiomeCount) - 1u;

struct ScatterRule {
    std::string name;        // for the editor, and whatever the game maps to an asset
    u32 biomes = kAllBiomes; // BiomeBit() flags where it may stand

    // Where.
    f32 height_min = 0.0f, height_max = 1.0f;     // 0..1 of the terrain's height range
    f32 slope_min = 0.0f, slope_max = 0.7f;       // rise over run: 0.7 is about 35 degrees
    f32 water_distance_min = 0.0f;                // metres from the nearest water it must keep (0: may stand at the shore)
    f32 water_distance_max = 1.0e9f;              // metres it may be at most from water (for reeds and wet-ground plants)
    f32 patch_scale = 0.0f;                       // lattice cells per 100 m of the patchiness noise; 0 turns the noise off
    f32 patch_threshold = 0.5f;                   // 0..1: where the noise is below this it is bare, above it fills in

    // How many, and how they gather.
    f32 density = 1.0f;     // candidate positions per 100 square metres, before the rules above and the room below thin them
    u32 max_instances = 100000;
    u32 clump_size = 1;     // 1 scatters singly; more places that many around each parent (a stand of birches, a pile of rocks)
    f32 clump_radius = 3.0f; // metres around the parent

    // Size and pose.
    f32 scale_min = 0.8f, scale_max = 1.2f;
    f32 scale_steep_bias = 0.0f; // 0..1: how much smaller things grow on steep ground (0: none)
    bool random_yaw = true;
    f32 align_to_normal = 0.0f; // 0 stands straight up, 1 tilts fully to the ground's slope (rocks), in between leans

    // Room.
    f32 footprint = 0.5f;  // metres: the radius it takes at scale 1 (it grows with the instance's scale)
    f32 overlap = 0.0f;    // 0..1: the share of two footprints allowed to interpenetrate (grass overlaps; trees do not)
    f32 y_offset = 0.0f;   // metres below (negative) or above the ground it sits (a rock bedded in)
};

struct ScatterInstance {
    Vec3 position{0, 0, 0}; // world, on the terrain surface (plus the rule's y_offset)
    Vec3 normal{0, 1, 0};   // the terrain's normal there
    Quaternion rotation{0, 0, 0, 1};
    f32 scale = 1.0f;
    u32 rule = 0;           // index into the rules passed to Scatter
    Biome biome = Biome::Grassland;
};

struct ScatterSettings {
    u32 seed = 1;
    u32 max_total = 1000000; // across all rules
};

// Places every rule's instances. The terrain's heightmap origin is the world origin (x = column * cell_size, z = row * cell_size).
std::vector<ScatterInstance> Scatter(const GeneratedTerrain& terrain, const TerrainGenSettings& terrain_settings,
                                     std::span<const ScatterRule> rules, const ScatterSettings& settings);

// The distance in metres from each cell to the nearest water (a river, a lake or the sea): 0 in the water itself. Scatter makes this once;
// it is public for the editor's overlays and for tests.
std::vector<f32> WaterDistanceField(const GeneratedTerrain& terrain);

// The terrain's unit normal at a world position (central differences of the heightmap, one cell apart).
Vec3 TerrainNormalAt(const Heightmap& heightmap, f32 world_x, f32 world_z);

// A starter set for a temperate world, in pecking order: pines on forest slopes, birch stands, shrubs, grass, boulders (clustered), reeds
// at the water's edge, dead trees and cacti in the desert, snow-dusted rocks high up.
std::vector<ScatterRule> DefaultScatterRules();

// Maps instances onto the foliage system's layer: instance i of rule r gets type_index r (so a layer's `types` is the rules' assets, in
// order), its chunk from `chunk_world_size`, and the terrain normal. Hidden instances are not produced.
std::vector<FoliageInstance> ToFoliageInstances(std::span<const ScatterInstance> instances, f32 chunk_world_size);

// Draws the instances as dots onto a RenderTerrainPreview picture (RGB8, the terrain's size), one colour per rule.
void OverlayScatterPreview(std::vector<u8>& rgb, const GeneratedTerrain& terrain, std::span<const ScatterInstance> instances);

} // namespace terrain
} // namespace aether
