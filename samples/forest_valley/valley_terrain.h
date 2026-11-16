#pragma once

// The valley as terrain the renderer can draw: chunked heightfield, the four-layer splatmap and the layer colours, plus a headless "frame plan"
// that says what a camera at a given spot would have resident (chunks, LOD mix, triangles, GPU bytes). The plan runs the real TerrainRenderer
// against a counting TerrainGpu, so it measures the same meshes the GPU would be given, with no device.
//
// What this is not: drawing. The GPU pass (terrain::TerrainPass over terrain::RhiTerrainGpu) takes these chunks and the splatmap; it is not yet
// called from aether_player's SceneRenderer.

#include "forest_valley.h"

#include "aether/terrain/splat.h"
#include "aether/terrain/terrain.h"

#include <array>
#include <vector>

namespace aether::forest_valley {

struct ValleyTerrain {
    terrain::TerrainData data;
    terrain::TerrainSettings settings;
    std::vector<terrain::TerrainLayer> layers; // grass, rock, sand, snow
    terrain::SplatmapData splatmap;            // RGBA8 weights of those four, covering the whole world
};

inline constexpr u32 kSplatResolution = 512; // one weight texel per metre
inline constexpr u32 kVertsPerChunk = 33;    // 32 cells a side: 16 x 16 chunks over 512 m

ValleyTerrain BuildValleyTerrain(const World& world);

struct TerrainFramePlan {
    u32 chunks = 0;                // chunks with buffers for this camera
    std::array<u32, 8> lod_chunks{}; // chunks at each LOD level
    u64 triangles = 0;             // across the resident chunks
    u64 gpu_bytes = 0;             // vertex and index buffers
};

// The chunks resident for a camera at `camera` (LOD from its distance to each chunk) and what they cost.
TerrainFramePlan PlanTerrainFrame(const ValleyTerrain& valley, const Vec3& camera);

} // namespace aether::forest_valley
