#pragma once

#include "aether/core/base.h"
#include "aether/terrain/terrain.h"

#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace aether {
namespace terrain {

// Phase 21 step 3: terrain rendering -- GPU buffer upload and indexed draw
// calls. The terrain module asks a TerrainGpu for buffers (the renderer
// implements it over the RHI), and the render loop owns the actual
// dispatch. This module produces ready-to-render chunk meshes, and builds
// and tests headless with a fake TerrainGpu.

// What the terrain needs from a GPU backend.
class TerrainGpu {
public:
    virtual ~TerrainGpu() = default;
    // A buffer holding `data` (vertices, or 32-bit indices when `index`); 0 when it fails.
    virtual u64 CreateBuffer(const void* data, u64 bytes, bool index) = 0;
    virtual void DestroyBuffer(u64 buffer) = 0;
};

// A chunk mesh ready for the GPU: vertex + index buffers from the TerrainGpu.
struct RenderChunk {
    i32 chunk_x = 0;
    i32 chunk_z = 0;
    u32 lod_level = 0;
    Vec3 world_min{0, 0, 0};
    Vec3 world_max{0, 0, 0};

    // Buffer handles from TerrainGpu::CreateBuffer (0: none).
    u64 vertex_buffer = 0;
    u64 index_buffer = 0;

    u32 verts_per_side = 0;
    u32 vertex_count = 0;
    u32 index_count = 0;

    // Dirty flag: set when heightmap or LOD changed -- need to rebuild.
    bool dirty = true;

    TerrainChunk AsTerrainChunk() const {
        TerrainChunk c;
        c.chunk_x = chunk_x, c.chunk_z = chunk_z, c.lod_level = lod_level;
        c.world_min = world_min, c.world_max = world_max;
        c.verts_per_side = verts_per_side;
        return c;
    }
};

// Uploads one chunk's vertex + index data to GPU buffers from the TerrainGpu.
// Replaces existing buffers if the chunk was already uploaded.
bool UploadChunk(TerrainGpu& gpu, RenderChunk& chunk,
                 std::span<const f32> vertices, std::span<const u32> indices);

// Creates or re-creates GPU buffers for a dirty chunk.
// Frees existing buffers first if they're present.
bool RebuildChunk(TerrainGpu& gpu, RenderChunk& chunk,
                  const TerrainData& data, const TerrainSettings& settings);

// Uploads vertex + index data through the TerrainGpu and records the handles.
bool UploadChunkData(TerrainGpu& gpu, RenderChunk& chunk,
                     std::span<const f32> vertices, std::span<const u32> indices);

// Free GPU buffers for a chunk (called when the chunk is destroyed).
void FreeChunk(TerrainGpu& gpu, RenderChunk& chunk);

// --- Batch management ---

// Manages all render chunks for a terrain. Handles chunk creation, LOD
// switching, and GPU buffer upload. The caller owns the TerrainGpu.
class TerrainRenderer {
public:
    explicit TerrainRenderer(TerrainGpu& gpu);
    ~TerrainRenderer();

    // Build initial chunk set from terrain data.
    void Build(const TerrainData& data, const TerrainSettings& settings);

    // Update chunks that need rebuilding (dirty flag).
    // Call this when heightmap or settings change.
    void UpdateDirty(const TerrainData& data, const TerrainSettings& settings);

    // Switch chunks to the appropriate LOD based on camera position.
    // Rebuilds any chunk whose LOD changed.
    void UpdateLOD(const TerrainData& data, const TerrainSettings& settings,
                   const Vec3& camera_position);

    // Free all GPU resources.
    void Destroy();

    // Access the render chunks for rendering.
    std::span<const RenderChunk> Chunks() const { return chunks_; }

    // Mark all chunks dirty (e.g. after heightmap edit).
    void MarkAllDirty();

    // Number of chunks with active GPU buffers.
    usize UploadedCount() const;

    // Estimate GPU memory used by all chunks (bytes).
    u64 EstimatedVRAM() const;

private:
    TerrainGpu& gpu_;
    std::vector<RenderChunk> chunks_;
    std::unordered_map<u64, RenderChunk*> chunk_by_key_; // key = (cx,cz,lod)

    u64 MakeKey(i32 cx, i32 cz, u32 lod) const;
    RenderChunk* GetChunk(i32 cx, i32 cz, u32 lod);
    void RebuildImpl(RenderChunk& chunk, const TerrainData& data,
                     const TerrainSettings& settings);
};

// Build the list of render chunks that should exist for the given terrain.
std::vector<RenderChunk> BuildRenderChunks(const TerrainData& data,
                                             const TerrainSettings& settings);

} // namespace terrain
} // namespace aether
