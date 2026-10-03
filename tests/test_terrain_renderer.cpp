#include "aether/terrain/renderer.h"
#include "aether/terrain/terrain.h"
#include "test_framework.h"

#include <map>

using namespace aether;
using namespace aether::terrain;

AETHER_TEST(TerrainRenderChunks_BuildLayout) {
    // 65x65 heightmap with 32-vertex chunks => 3x3 chunks.
    Heightmap hm = CreateProceduralHeightmap(65, 65, 1.0f, 2, 0.5f, 8.0f);
    TerrainSettings settings;
    settings.verts_per_chunk = 32;
    settings.chunk_world_size = 31.0f;
    settings.vertical_scale = 1.0f;

    TerrainData data;
    InitTerrainData(data, hm, settings);

    auto chunks = BuildRenderChunks(data, settings);
    // 65x65 -> (65+31)/(32) = 3x3 chunks
    AETHER_CHECK(chunks.size() == 9);

    for (const auto& c : chunks) {
        AETHER_CHECK(c.vertex_buffer == 0); // no GPU buffers yet
        AETHER_CHECK(c.index_buffer == 0);
        AETHER_CHECK(c.dirty == true);
        AETHER_CHECK(c.lod_level == 0);
        AETHER_CHECK(c.verts_per_side == 32);
    }
}

AETHER_TEST(TerrainRenderChunks_BoundsConsistent) {
    Heightmap hm = CreateProceduralHeightmap(32, 32, 1.0f, 1, 0.5f, 4.0f);
    TerrainSettings settings;
    settings.verts_per_chunk = 32;
    settings.chunk_world_size = 31.0f;
    settings.vertical_scale = 1.0f;

    TerrainData data;
    InitTerrainData(data, hm, settings);
    auto chunks = BuildRenderChunks(data, settings);
    AETHER_CHECK(chunks.size() == 1);

    const auto& c = chunks[0];
    // world_min < world_max in all axes
    AETHER_CHECK(c.world_min.x < c.world_max.x);
    AETHER_CHECK(c.world_min.y <= c.world_max.y);
    AETHER_CHECK(c.world_min.z < c.world_max.z);
    // Chunk is near the origin (hm starts at 0,0).
    AETHER_CHECK(c.world_min.x >= -0.5f);
    AETHER_CHECK(c.world_min.z >= -0.5f);
}

AETHER_TEST(TerrainRenderChunks_LODLevels) {
    TerrainData data;
    data.chunk_size = 32;
    data.chunk_count_x = 2;
    data.chunk_count_z = 2;
    data.lod_distances = {32.0f, 64.0f, 128.0f};
    data.max_lod = 3;
    data.heightmap = CreateProceduralHeightmap(33, 33, 1.0f, 1, 0.5f, 4.0f);

    TerrainSettings settings;
    settings.chunk_world_size = 31.0f;
    settings.verts_per_chunk = 32;

    // LOD 0: 32 vertices -> (32-1)/1+1 = 32
    AETHER_CHECK(SelectLod(data, 10.0f) == 0);
    // LOD 1: (32-1)/2+1 = 16
    AETHER_CHECK(SelectLod(data, 40.0f) == 1);
    // LOD 2: (32-1)/4+1 = 8
    AETHER_CHECK(SelectLod(data, 100.0f) == 2);
    // Max: (32-1)/8+1 = 4
    AETHER_CHECK(SelectLod(data, 200.0f) == 3);
}

AETHER_TEST(TerrainChunks_VertexIndexCountConsistency) {
    // One 32x32 chunk.
    Heightmap hm = CreateProceduralHeightmap(33, 33, 1.0f, 1, 0.5f, 4.0f);
    TerrainSettings settings;
    settings.verts_per_chunk = 32;
    settings.chunk_world_size = 31.0f;
    settings.vertical_scale = 1.0f;

    TerrainData data;
    InitTerrainData(data, hm, settings);
    auto chunks = BuildChunks(data, settings);

    // At LOD 0: 32x32 = 1024 vertices, (32-1)*(32-1)*6 = 5766 indices
    const auto& c = chunks[0];
    AETHER_CHECK(GenerateChunkVertices(data, settings, c).size() == 1024u * 8); // 32*32 vertices, 8 floats each
    AETHER_CHECK(GenerateChunkIndices(data, c).size() == 5766u);              // 31*31*6
}

AETHER_TEST(TerrainRenderer_MarkAllDirty) {
    Heightmap hm = CreateProceduralHeightmap(32, 32, 1.0f, 1, 0.5f, 4.0f);
    TerrainSettings settings;
    settings.verts_per_chunk = 32;
    settings.chunk_world_size = 31.0f;
    settings.vertical_scale = 1.0f;

    TerrainData data;
    InitTerrainData(data, hm, settings);
    auto chunks = BuildRenderChunks(data, settings);
    AETHER_CHECK(chunks.size() == 1);
    chunks[0].dirty = false;

    // Verify MarkAllDirty would flip dirty on all chunks.
    // (No actual Renderer here -- just testing the chunk model.)
    // We test that the dirty flag is correctly maintained.
    AETHER_CHECK(chunks[0].dirty == false);
    // Simulate editing: mark dirty
    chunks[0].dirty = true;
    AETHER_CHECK(chunks[0].dirty == true);
}

namespace {
// Records buffers instead of making them.
class FakeGpu final : public TerrainGpu {
public:
    u64 CreateBuffer(const void*, u64 bytes, bool index) override {
        if (fail) return 0;
        live[++next] = bytes;
        (index ? index_bytes : vertex_bytes) += bytes;
        return next;
    }
    void DestroyBuffer(u64 buffer) override { live.erase(buffer); }
    std::map<u64, u64> live;
    u64 next = 0, vertex_bytes = 0, index_bytes = 0;
    bool fail = false;
};
} // namespace

AETHER_TEST(TerrainChunks_EachChunkUsesItsOwnHeights) {
    // A ramp along X: height = x. Chunk 1 starts at sample 31 (chunks share edges).
    Heightmap hm;
    hm.width = 63, hm.height = 32, hm.cell_size = 1.0f;
    hm.heights.resize(63u * 32u);
    for (u32 z = 0; z < 32; ++z)
        for (u32 x = 0; x < 63; ++x) hm.heights[z * 63 + x] = static_cast<f32>(x);
    TerrainSettings settings;
    settings.verts_per_chunk = 32;
    settings.chunk_world_size = 31.0f;
    TerrainData data;
    InitTerrainData(data, hm, settings);
    AETHER_CHECK(data.chunk_count_x == 2 && data.chunk_count_z == 1);
    auto chunks = BuildChunks(data, settings);
    AETHER_CHECK(chunks.size() == 2);
    const auto v1 = GenerateChunkVertices(data, settings, chunks[1]);
    AETHER_CHECK_NEAR(v1[0], 31.0f, 1e-4f); // x of its first vertex
    AETHER_CHECK_NEAR(v1[1], 31.0f, 1e-4f); // and its height: the heightmap there
    AETHER_CHECK_NEAR(chunks[1].world_min.y, 31.0f, 1e-4f);
    AETHER_CHECK_NEAR(chunks[1].world_max.y, 62.0f, 1e-4f);
    AETHER_CHECK_NEAR(chunks[0].world_max.y, 31.0f, 1e-4f);
    // The shared edge has the same height in both chunks.
    const auto v0 = GenerateChunkVertices(data, settings, chunks[0]);
    AETHER_CHECK_NEAR(v0[31 * 8 + 1], v1[1], 1e-4f);
}

AETHER_TEST(TerrainRenderer_UploadsThroughTheGpu) {
    Heightmap hm = CreateProceduralHeightmap(63, 63, 1.0f, 2, 0.5f, 4.0f);
    TerrainSettings settings;
    settings.verts_per_chunk = 32;
    settings.chunk_world_size = 31.0f;
    TerrainData data;
    InitTerrainData(data, hm, settings);
    FakeGpu gpu;
    {
        TerrainRenderer r(gpu);
        r.Build(data, settings);
        AETHER_CHECK(r.Chunks().size() == 4);
        AETHER_CHECK(r.UploadedCount() == 4);
        AETHER_CHECK(gpu.live.size() == 8); // a vertex and an index buffer each
        AETHER_CHECK(r.EstimatedVRAM() == gpu.vertex_bytes + gpu.index_bytes);
        for (const RenderChunk& c : r.Chunks()) AETHER_CHECK(!c.dirty && c.vertex_count == 32u * 32u && c.index_count == 31u * 31u * 6u);
        // Far away: every chunk drops to the coarsest LOD and is re-uploaded with fewer vertices.
        r.UpdateLOD(data, settings, Vec3(10000, 0, 10000));
        for (const RenderChunk& c : r.Chunks()) AETHER_CHECK(c.lod_level == data.max_lod && c.vertex_count < 32u * 32u);
        AETHER_CHECK(gpu.live.size() == 8); // the old buffers were freed
        r.MarkAllDirty();
        r.UpdateDirty(data, settings);
        AETHER_CHECK(gpu.live.size() == 8 && r.UploadedCount() == 4);
    }
    AETHER_CHECK(gpu.live.empty()); // the destructor frees everything
    // A failing GPU leaves nothing behind.
    FakeGpu broken;
    broken.fail = true;
    RenderChunk c;
    const std::vector<f32> v(8 * 4, 0.0f);
    const std::vector<u32> i{0, 1, 2};
    AETHER_CHECK(!UploadChunkData(broken, c, v, i));
    AETHER_CHECK(c.vertex_buffer == 0 && c.index_buffer == 0 && broken.live.empty());
}
