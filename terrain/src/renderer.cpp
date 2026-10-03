#include "aether/terrain/renderer.h"

#include "aether/core/log.h"
#include "aether/gfx/buffer.h"

#include <algorithm>

namespace aether {
namespace terrain {

using namespace gfx;

// --- Key utils ---

u64 TerrainRenderer::MakeKey(i32 cx, i32 cz, u32 lod) const {
    // Pack into 64 bits: [lod:16][cz:24][cx:24]
    return (static_cast<u64>(lod) << 48)
         | (static_cast<u64>(static_cast<u32>(cz) + 0x800000u) << 24)
         |  static_cast<u64>(static_cast<u32>(cx) + 0x800000u);
}

// --- Chunk bounds ---

namespace {

// Recompute a render chunk's world-space bounds from its heightmap region.
void ComputeChunkBounds(const TerrainData& data, const TerrainSettings& settings,
                        RenderChunk& chunk) {
    const u32 vps = data.chunk_size;
    const f32 step = (vps > 1) ? 1.0f / static_cast<f32>(vps - 1) : 0.0f;
    const f32 half_w = settings.chunk_world_size * 0.5f;

    const f32 x0 = static_cast<f32>(chunk.chunk_x) * settings.chunk_world_size;
    const f32 z0 = static_cast<f32>(chunk.chunk_z) * settings.chunk_world_size;

    // Bounding box of the chunk's sample region in world units.
    chunk.world_min = Vec3(x0, -10.0f, z0);
    chunk.world_max = Vec3(x0 + settings.chunk_world_size,
                           10.0f,
                           z0 + settings.chunk_world_size);
    (void)step;
    (void)half_w;
}

} // namespace

// --- Chunk upload ---

bool UploadChunkData(Device& device, RenderChunk& chunk,
                     span<const f32> vertices, span<const u32> indices) {
    if (vertices.empty() || indices.empty()) return false;

    const u64 vb_size = static_cast<u64>(vertices.size()) * sizeof(f32);
    const u64 ib_size = static_cast<u64>(indices.size()) * sizeof(u32);

    // Create and fill the vertex buffer (pos+normal+uv: 8 floats/vertex).
    chunk.vertex_buffer = std::make_unique<Buffer>(device, vb_size, BufferKind::Upload);
    chunk.vertex_buffer->Update(vertices.data(), vb_size);
    chunk.vertex_count = static_cast<u32>(vertices.size()) / 8;

    // Create and fill the index buffer (32-bit indices).
    chunk.index_buffer = std::make_unique<Buffer>(device, ib_size, BufferKind::Upload);
    chunk.index_buffer->Update(indices.data(), ib_size);
    chunk.index_count = static_cast<u32>(indices.size());

    chunk.dirty = false;
    return true;
}

bool RebuildChunk(Device& device, RenderChunk& chunk,
                  const TerrainData& data, const TerrainSettings& settings) {
    // Free existing buffers.
    chunk.vertex_buffer.reset();
    chunk.index_buffer.reset();

    // Generate geometry — pass a TerrainChunk wrapper built from the RenderChunk.
    TerrainChunk tc;
    tc.chunk_x = chunk.chunk_x;
    tc.chunk_z = chunk.chunk_z;
    tc.lod_level = chunk.lod_level;
    tc.verts_per_side = chunk.verts_per_side;
    tc.world_min = chunk.world_min;
    tc.world_max = chunk.world_max;
    auto verts = GenerateChunkVertices(data, settings, tc);
    auto idxs  = GenerateChunkIndices(data, tc);

    // Update bounds from the new geometry.
    ComputeChunkBounds(data, settings, chunk);

    return UploadChunkData(device, chunk,
                            {verts.data(), verts.size()},
                            {idxs.data(), idxs.size()});
}

void FreeChunk(Device& device, RenderChunk& chunk) {
    chunk.vertex_buffer.reset();
    chunk.index_buffer.reset();
    chunk.dirty = true;
    (void)device;
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
            chunks.push_back(std::move(chunk));
        }
    }
    return chunks;
}

// --- TerrainRenderer ---

TerrainRenderer::TerrainRenderer(Device& device) : device_(device) {}

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
        const f32 dist = (chunk_center - camera_pos).Length();
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
        FreeChunk(device_, c);
    }
    chunks_.clear();
    chunk_by_key_.clear();
}

usize TerrainRenderer::UploadedCount() const {
    usize count = 0;
    for (const RenderChunk& c : chunks_) {
        if (c.vertex_buffer) ++count;
    }
    return count;
}

u64 TerrainRenderer::EstimatedVRAM() const {
    u64 total = 0;
    for (const RenderChunk& c : chunks_) {
        if (c.vertex_buffer) {
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
    if (!chunk.vertex_buffer || chunk.dirty) {
        RebuildChunk(device_, chunk, data, settings);
    }
}

} // namespace terrain
} // namespace aether
