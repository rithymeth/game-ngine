#pragma once

#include "aether/core/base.h"
#include "aether/math/math.h"
#include "aether/reflection/reflection.h"

#include <functional>
#include <memory>
#include <vector>

namespace aether {

// Phase 21 step 1: heightmap terrain with chunked meshes and LOD.
// The engine module owns the terrain math and chunk generation.
// GPU buffer upload and rendering use the RHI ? the terrain module itself
// is engine-only so it can build headless.

namespace terrain {

// A 2D heightmap tile used as source data for terrain chunks.
// Heights are in world units.
struct Heightmap {
    u32 width = 0;      // number of samples along X
    u32 height = 0;     // number of samples along Z
    f32 cell_size = 1.0f; // world units per sample
    std::vector<f32> heights; // row-major: heights[z * width + x]

    f32 GetHeight(u32 x, u32 z) const;
    f32 SampleLinear(f32 wx, f32 wz) const;
};

// A terrain material layer (used in splatmaps).
struct TerrainLayer {
    std::string name;
    Vec3 albedo = Vec3(1.0f, 1.0f, 1.0f);
    f32 metallic = 0.0f;
    f32 roughness = 0.9f;
    f32 normal_strength = 1.0f;
};

// Terrain data asset: shared heightmap + layers + chunk layout.
// One asset can serve many TerrainComponent instances.
struct TerrainData {
    Heightmap heightmap;
    std::vector<TerrainLayer> layers;
    Vec3 bounds_min{0, 0, 0};
    Vec3 bounds_max{0, 0, 0};

    u32 chunk_size = 32;    // vertices per chunk side (power of 2)
    u32 chunk_count_x = 0;
    u32 chunk_count_z = 0;

    std::vector<f32> lod_distances = {64.0f, 128.0f, 256.0f};
    u32 max_lod = 3;
};

// Terrain settings for a world.
struct TerrainSettings {
    f32 vertical_scale = 1.0f;
    f32 chunk_world_size = 32.0f; // world units per chunk side
    u32 verts_per_chunk = 32;
    u32 max_lod = 4;
    f32 lod_distance = 64.0f; // distance per LOD level
};

// A terrain chunk at a specific LOD level.
struct TerrainChunk {
    i32 chunk_x = 0;
    i32 chunk_z = 0;
    u32 lod_level = 0;
    Vec3 world_min{0, 0, 0};
    Vec3 world_max{0, 0, 0};
    u32 verts_per_side = 0;
    bool is_dirty = true;
};

// Build the chunk layout from heightmap dimensions.
void InitTerrainData(TerrainData& data, const Heightmap& hm, const TerrainSettings& settings);

// Build all terrain chunks from heightmap data.
std::vector<TerrainChunk> BuildChunks(const TerrainData& data,
                                       const TerrainSettings& settings);

// Generate the vertex buffer data for a chunk.
// Format: position(3) + normal(3) + uv(2) = 8 floats per vertex.
std::vector<f32> GenerateChunkVertices(const TerrainData& data,
                                         const TerrainSettings& settings,
                                         const TerrainChunk& chunk);

// Generate the index buffer data for a chunk.
std::vector<u32> GenerateChunkIndices(const TerrainData& data,
                                       const TerrainChunk& chunk);

// Sample the terrain height at an arbitrary world position.
f32 SampleTerrainHeight(const TerrainData& data, f32 world_x, f32 world_z);

// Create a procedural heightmap for testing and prototyping.
Heightmap CreateProceduralHeightmap(u32 width, u32 height, f32 cell_size,
                                     u32 octaves, f32 persistence, f32 scale);

// Get the appropriate LOD level for a chunk given its distance to camera.
u32 SelectLod(const TerrainData& data, f32 camera_distance);

} // namespace terrain
} // namespace aether
