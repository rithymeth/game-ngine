#pragma once

#include ""aether/core/base.h""
#include ""aether/gfx/device.h""
#include ""aether/terrain/terrain.h""

#include <memory>
#include <unordered_map>
#include <vector>

namespace aether {
namespace terrain {

// Phase 21 step 3: terrain rendering ? GPU buffer upload and indexed draw
// calls. The terrain module knows about the RHI (Device) for buffer creation,
// but the render loop (RenderGraph) owns the actual dispatch. This module
// produces ready-to-render chunk meshes.

// A chunk mesh ready for the GPU: vertex + index buffers on the Device.
struct RenderChunk {
    i32 chunk_x = 0;
    i32 chunk_z = 0;
    u32 lod_level = 0;
    Vec3 world_min{0, 0, 0};
    Vec3 world_max{0, 0, 0};

    // RHI resource IDs (Device::CreateBuffer / CreateIndexBuffer).
    // These are opaque u64 handles from gfx::Device.
    u64 vertex_buffer = 0;
    u64 index_buffer = 0;

    u32 vertex_count = 0;
    u32 index_count = 0;

    // Dirty flag: set when heightmap or LOD changed ? need to rebuild.
    bool dirty = true;
};

// Uploads one chunk's vertex + index data to GPU buffers on the Device.
// Replaces existing buffers if the chunk was already uploaded.
bool UploadChunk(gfx::Device& device, RenderChunk& chunk,
                 span<const f32> vertices, span<const u32> indices);

// Creates or re-creates GPU buffers for a dirty chunk.
// Frees existing buffers first if they're present.
bool RebuildChunk(gfx::Device& device, RenderChunk& chunk,
                  const TerrainData& data, const TerrainSettings& settings);

// Uploads vertex + index data to the Device and records the handles.
bool UploadChunkData(gfx::Device& device, RenderChunk& chunk,
                     span<const f32> vertices, span<const u32> indices);

// Free GPU buffers for a chunk (called when the chunk is destroyed).
void FreeChunk(gfx::Device& device, RenderChunk& chunk);

// --- Batch management ---

// Manages all render chunks for a terrain. Handles chunk creation, LOD
// switching, and GPU buffer upload. The caller owns the Device.
class TerrainRenderer {
public:
    explicit TerrainRenderer(gfx::Device& device);
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
    span<const RenderChunk> Chunks() const { return chunks_; }

    // Mark all chunks dirty (e.g. after heightmap edit).
    void MarkAllDirty();

    // Number of chunks with active GPU buffers.
    usize UploadedCount() const;

    // Estimate GPU memory used by all chunks (bytes).
    u64 EstimatedVRAM() const;

private:
    gfx::Device& device_;
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
