#pragma once

#include "aether/core/base.h"
#include "aether/math/math.h"
#include "aether/reflection/reflection.h"

#include <random>
#include <span>
#include <vector>

namespace aether {
namespace terrain {

using std::span;

// Phase 21 step 4: GPU-culled foliage instancing with LOD and impostors.

// A foliage type: one mesh placed many times with random variation.
// Examples: grass clump, small rock, tree, bush.
struct FoliageType {
    u64 mesh_id = 0;           // asset ID of the static mesh
    u32 mesh_lod_count = 1;    // number of LODs in the mesh
    f32 lod_distance = 20.0f; // switch to next LOD beyond this
    f32 imposter_distance = 50.0f; // switch to billboard impostor beyond this
    u64 impostor_texture_id = 0; // cross-shaped billboard atlas texture

    // Per-instance variation bounds.
    f32 scale_min = 0.8f;
    f32 scale_max = 1.2f;
    f32 rotation_range = 3.14159f; // ?180? around Y
    bool align_to_normal = true;   // tilt with terrain slope

    // Ground offset: place this far above ground (e.g. trees anchor at base).
    f32 anchor_offset = 0.0f;
};

// One placed foliage instance.
struct FoliageInstance {
    Vec3 position{0, 0, 0};  // world position (on terrain surface)
    Vec3 normal{0, 1, 0};   // terrain normal at this position
    Vec3 scale{1, 1, 1};     // random scale
    Quaternion rotation{0, 0, 0, 1}; // random Y rotation
    u32 type_index = 0;       // index into FoliageLayer::types
    u16 chunk_x = 0;          // which terrain chunk this belongs to
    u16 chunk_z = 0;
    u32 lod_level = 0;        // current LOD level (computed at paint time)
    bool hidden = false;       // culled or editor-hidden
};

// One foliage layer: a set of foliage types and their placed instances.
// One layer per terrain (e.g. "Forest", "Desert").
struct FoliageLayer {
    std::string name;
    std::vector<FoliageType> types;

    // Density settings.
    u32 max_instances = 50000; // GPU limit
    f32 density = 0.5f;        // 0..1: instances per square meter
    u32 seed = 0;              // random seed for procedural placement

    // Stored instances (loaded from .afoliage asset).
    std::vector<FoliageInstance> instances;
};

// Paint density map: how many instances to place per 1m x 1m cell.
// Higher density = more instances. Stored as u8 (0..255 = 0..1 normalized).
struct DensityMap {
    u32 width = 0;   // cells along X
    u32 height = 0;  // cells along Z
    std::vector<u8> cells; // row-major
};

// Generate instances procedurally using a density map and terrain normals.
// Uses a deterministic RNG seeded per layer.
void GenerateInstances(FoliageLayer& layer, const DensityMap& density,
                     span<const Vec3> sample_positions,
                     span<const Vec3> sample_normals,
                     std::mt19937& rng);

// Generate from noise: per-cell density from Perlin-like noise.
DensityMap GenerateDensityMap(u32 width, u32 height, f32 world_size,
                              u32 octaves, f32 persistence, f32 seed);

// Random per-instance variation.
Vec3 RandomScale(std::mt19937& rng, const FoliageType& type);
Quaternion RandomRotation(std::mt19937& rng, const FoliageType& type);

// Cull instances not on the given terrain chunk.
// Returns only instances with matching chunk_x, chunk_z.
std::vector<FoliageInstance> FilterByChunk(span<const FoliageInstance> instances,
                                           u32 chunk_x, u32 chunk_z);

// Sort instances by type to minimize state changes during GPU instanced draw.
std::vector<u32> SortInstancesByType(span<const FoliageInstance> instances);

// For the editor: add one instance at a world position.
void AddInstance(FoliageLayer& layer, const Vec3& position,
                const Vec3& normal, std::mt19937& rng);

// Remove instances near a world position (for eraser tool).
void RemoveInstancesNear(FoliageLayer& layer, const Vec3& position, f32 radius);

// Paint instances: add if density > threshold, remove if density <= threshold.
// density is in world-space cells.
void PaintInstances(FoliageLayer& layer, const DensityMap& density,
                   const Vec3& center, f32 brush_radius,
                   std::mt19937& rng);

// Compute the bounding box of all instances.
void ComputeBounds(span<const FoliageInstance> instances,
                   Vec3* out_min, Vec3* out_max);

} // namespace terrain
} // namespace aether
