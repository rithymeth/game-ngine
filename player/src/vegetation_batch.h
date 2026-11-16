#pragma once

// Vegetation for a ProceduralTerrain, drawn without instancing: the RHI has no instanced draw, so each terrain chunk's plants of one kind are
// merged into one mesh on the CPU (static batching) and drawn with the ordinary indexed draw, culled by chunk and distance. This file is the CPU
// half and runs headless: the procedural plant meshes, the scatter rules for a terrain, and the batching. vegetation_pass.h draws the result.
//
// The plants are placeholders: simple procedural shapes (a pine of cones, a birch of blobs, tufts of blades, a faceted boulder) with a flat
// palette colour, not authored meshes. They stand in until the showcase has art.

#include "aether/core/base.h"
#include "aether/math/math.h"
#include "aether/renderer/components.h"
#include "aether/terrain/scatter.h"

#include <array>
#include <span>
#include <vector>

namespace aether::player {

enum class PlantKind : u8 { Pine, Birch, Shrub, Grass, Boulder, Reeds };
inline constexpr u32 kPlantKindCount = 6;
const char* PlantKindName(PlantKind kind);

// What the vertex shader reads: position, normal, and uv = (palette column centre, sway weight). The sway weight is 0 for rigid parts (a trunk, a
// rock) and rises toward the tip of what bends in the wind; it also darkens the base of a tuft or crown a little. 32 bytes, the terrain's layout.
struct VegVertex {
    f32 position[3];
    f32 normal[3];
    f32 uv[2];
};
static_assert(sizeof(VegVertex) == 32);

inline constexpr u32 kPaletteColumns = 24;
// The palette texture: kPaletteColumns x 1 RGBA8 texels (sRGB), sampled at column centres.
std::array<u8, kPaletteColumns * 4> PalettePixels();

struct PlantMesh {
    std::vector<VegVertex> vertices;
    std::vector<u8> palette_span; // per vertex: 0, or how many palette shades after its column an instance may pick (the per-plant tint)
    std::vector<u32> indices;
    f32 height = 1.0f; // metres, at instance scale 1
    f32 radius = 0.5f; // the widest reach from the base's axis
};
// The shared shape of a kind: base at the origin, +y up, in metres.
const PlantMesh& PlantPrototype(PlantKind kind);

// The merged plants of one kind in one terrain chunk.
struct VegChunk {
    i32 chunk_x = 0, chunk_z = 0;
    PlantKind kind = PlantKind::Grass;
    Vec3 bounds_min{0, 0, 0}, bounds_max{0, 0, 0};
    std::vector<VegVertex> vertices;
    std::vector<u32> indices;
    u32 instances = 0;
};

struct VegetationBatch {
    std::vector<VegChunk> chunks;
    u32 instances = 0;
    u64 vertices = 0, triangles = 0;
    std::array<u32, kPlantKindCount> instances_by_kind{};
};

// The scatter rules for a terrain, in pecking order, and the plant each draws: pines, birches, shrubs, grass, boulders and reeds from the terrain
// module's default rules, with their counts capped by the showcase's asset budget (docs/design/FOREST_VALLEY_SHOWCASE.md: 800-1,500 trees,
// 12,000-30,000 plants, 500-1,000 rocks) times `vegetation_density`. The two vectors are parallel.
struct VegetationRules {
    std::vector<terrain::ScatterRule> rules;
    std::vector<PlantKind> kinds;
};
VegetationRules MakeVegetationRules(const ProceduralTerrain& terrain);

// Merges `instances` (rule index into `kinds`) into per-chunk, per-kind meshes over square chunks `chunk_size` metres a side. Deterministic.
VegetationBatch BatchVegetation(std::span<const terrain::ScatterInstance> instances, std::span<const PlantKind> kinds, f32 chunk_size);

} // namespace aether::player
