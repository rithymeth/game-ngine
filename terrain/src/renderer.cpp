#include "aether/terrain/renderer.h"
#include "terrain_math.h"

#include "aether/core/log.h"

#include <algorithm>

namespace aether {
namespace terrain {

namespace {
// A render chunk's bounds, through the terrain chunk it draws.
void ComputeChunkBounds(const TerrainData& data, const TerrainSettings& settings, RenderChunk& chunk) {
    TerrainChunk t = chunk.AsTerrainChunk();
    terrain::ComputeChunkBounds(data, settings, t);
    chunk.world_min = t.world_min;
    chunk.world_max = t.world_max;
}
} // namespace


// --- Key utils ---

u64 TerrainRenderer::MakeKey(i32 cx, i32 cz, u32 lod) const {
    // Pack into 64 bits: [lod:16][cz:24][cx:24]
    return (static_cast<u64>(lod) << 48)
         | (static_cast<u64>(static_cast<u32>(cz) + 0x800000u) << 24)
         |  static_cast<u64>(static_cast<u32>(cx) + 0x800000u);
}

// --- Chunk upload ---

bool UploadChunkData(TerrainGpu& gpu, RenderChunk& chunk,
                     std::span<const f32> vertices, std::span<const u32> indices) {
    if (vertices.empty() || indices.empty()) return false;

    const u64 vb_size = static_cast<u64>(vertices.size()) * sizeof(f32);
    const u64 ib_size = static_cast<u64>(indices.size()) * sizeof(u32);

    // Vertex buffer: pos+normal+uv, 8 floats = 32 bytes per vertex.
    chunk.vertex_buffer = gpu.CreateBuffer(vertices.data(), vb_size, false);
    if (chunk.vertex_buffer == 0) {
        AETHER_LOG_ERROR("Terrain", "Failed to create a terrain vertex buffer (%llu bytes)", static_cast<unsigned long long>(vb_size));
        return false;
    }
    chunk.vertex_count = static_cast<u32>(vertices.size()) / 8;

    // Index buffer (32-bit indices).
    chunk.index_buffer = gpu.CreateBuffer(indices.data(), ib_size, true);
    if (chunk.index_buffer == 0) {
        AETHER_LOG_ERROR("Terrain", "Failed to create a terrain index buffer (%llu bytes)", static_cast<unsigned long long>(ib_size));
        gpu.DestroyBuffer(chunk.vertex_buffer);
        chunk.vertex_buffer = 0;
        chunk.vertex_count = 0;
        return false;
    }
    chunk.index_count = static_cast<u32>(indices.size());

    chunk.dirty = false;
    return true;
}

bool UploadChunk(TerrainGpu& gpu, RenderChunk& chunk,
                 std::span<const f32> vertices, std::span<const u32> indices) {
    FreeChunk(gpu, chunk);
    return UploadChunkData(gpu, chunk, vertices, indices);
}

bool RebuildChunk(TerrainGpu& gpu, RenderChunk& chunk,
                  const TerrainData& data, const TerrainSettings& settings) {
    // Free existing buffers.
    if (chunk.vertex_buffer) gpu.DestroyBuffer(chunk.vertex_buffer);
    if (chunk.index_buffer)  gpu.DestroyBuffer(chunk.index_buffer);
    chunk.vertex_buffer = 0;
    chunk.index_buffer = 0;

    // Generate geometry.
    auto verts = GenerateChunkVertices(data, settings, chunk.AsTerrainChunk());
    auto idxs  = GenerateChunkIndices(data, chunk.AsTerrainChunk());

    // Update bounds from the new geometry.
    ComputeChunkBounds(data, settings, chunk);

    return UploadChunkData(gpu, chunk, std::span<const f32>(verts), std::span<const u32>(idxs));
}

void FreeChunk(TerrainGpu& gpu, RenderChunk& chunk) {
    if (chunk.vertex_buffer) { gpu.DestroyBuffer(chunk.vertex_buffer); chunk.vertex_buffer = 0; }
    if (chunk.index_buffer)  { gpu.DestroyBuffer(chunk.index_buffer);  chunk.index_buffer = 0; }
    chunk.dirty = true;
}

// --- Render chunk collection ---

std::vector<RenderChunk> BuildRenderChunks(const TerrainData& data,
                                             const TerrainSettings& settings) {
    std::vector<RenderChunk> chunks;
    const u32 vps = data.chunk_size;
    const u32 lvps0 = (vps - 1) / (1u << 0) + 1;

    for (u32 cz = 0; cz < data.chunk_count_z; ++cz) {
        for (u32 cx = 0; cx < data.chunk_count_x; ++cx) {
            RenderChunk chunk;
            chunk.chunk_x = static_cast<i32>(cx);
            chunk.chunk_z = static_cast<i32>(cz);
            chunk.lod_level = 0;
            chunk.verts_per_side = lvps0;
            ComputeChunkBounds(data, settings, chunk);
            chunks.push_back(chunk);
        }
    }
    return chunks;
}

// --- TerrainRenderer ---

TerrainRenderer::TerrainRenderer(TerrainGpu& gpu) : gpu_(gpu) {}

TerrainRenderer::~TerrainRenderer() { Destroy(); }

void TerrainRenderer::Build(const TerrainData& data, const TerrainSettings& settings) {
    Destroy();
    chunks_ = BuildRenderChunks(data, settings);

    // Build key map.
    for (RenderChunk& c : chunks_) {
        u64 key = MakeKey(c.chunk_x, c.chunk_z, c.lod_level);
        chunk_by_key_[key] = &c;
    }

    // Upload all chunks synchronously.
    for (RenderChunk& c : chunks_) {
        RebuildImpl(c, data, settings);
    }
    AETHER_LOG_INFO("Terrain", "Built %zu terrain chunks (%zu KB VRAM)",
        chunks_.size(), EstimatedVRAM() / 1024);
}

void TerrainRenderer::UpdateDirty(const TerrainData& data, const TerrainSettings& settings) {
    for (RenderChunk& c : chunks_) {
        if (c.dirty) {
            RebuildImpl(c, data, settings);
        }
    }
}

void TerrainRenderer::UpdateLOD(const TerrainData& data, const TerrainSettings& settings,
                                 const Vec3& camera_pos) {
    for (RenderChunk& c : chunks_) {
        // Approximate: use chunk center.
        const Vec3 chunk_center{
            (c.world_min.x + c.world_max.x) * 0.5f,
            (c.world_min.y + c.world_max.y) * 0.5f,
            (c.world_min.z + c.world_max.z) * 0.5f
        };
        const f32 dist = Length(chunk_center - camera_pos);
        const u32 target_lod = SelectLod(data, dist);

        if (target_lod != c.lod_level) {
            u64 old_key = MakeKey(c.chunk_x, c.chunk_z, c.lod_level);
            chunk_by_key_.erase(old_key);

            c.lod_level = target_lod;

            // Recompute LOD-specific data.
            const u32 vps = data.chunk_size;
            const u32 lod_step = 1u << target_lod;
            c.verts_per_side = (vps - 1) / lod_step + 1;
            ComputeChunkBounds(data, settings, c);
            c.dirty = true; // its mesh is for the old LOD
            RebuildImpl(c, data, settings);

            u64 new_key = MakeKey(c.chunk_x, c.chunk_z, c.lod_level);
            chunk_by_key_[new_key] = &c;
        }
    }
}

void TerrainRenderer::MarkAllDirty() {
    for (RenderChunk& c : chunks_) {
        c.dirty = true;
    }
}

void TerrainRenderer::Destroy() {
    for (RenderChunk& c : chunks_) {
        FreeChunk(gpu_, c);
    }
    chunks_.clear();
    chunk_by_key_.clear();
}

usize TerrainRenderer::UploadedCount() const {
    usize count = 0;
    for (const RenderChunk& c : chunks_) {
        if (c.vertex_buffer != 0) ++count;
    }
    return count;
}

u64 TerrainRenderer::EstimatedVRAM() const {
    u64 total = 0;
    for (const RenderChunk& c : chunks_) {
        if (c.vertex_buffer != 0) {
            // vertex: 8 floats * 4 bytes * count
            total += static_cast<u64>(c.vertex_count) * 8 * 4;
            // index: 4 bytes * count
            total += static_cast<u64>(c.index_count) * 4;
        }
    }
    return total;
}

RenderChunk* TerrainRenderer::GetChunk(i32 cx, i32 cz, u32 lod) {
    u64 key = MakeKey(cx, cz, lod);
    auto it = chunk_by_key_.find(key);
    return (it != chunk_by_key_.end()) ? it->second : nullptr;
}

void TerrainRenderer::RebuildImpl(RenderChunk& chunk, const TerrainData& data,
                                  const TerrainSettings& settings) {
    // Rebuild if dirty or no GPU buffers.
    if (chunk.vertex_buffer == 0) {
        RebuildChunk(gpu_, chunk, data, settings);
    } else if (chunk.dirty) {
        RebuildChunk(gpu_, chunk, data, settings);
    }
}

} // namespace terrain
} // namespace aether
