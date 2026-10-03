#include "aether/terrain/terrain.h"
#include "test_framework.h"

using namespace aether;
using namespace aether::terrain;

AETHER_TEST(TerrainHeightmap_ProceduralGeneration) {
    Heightmap hm = CreateProceduralHeightmap(65, 65, 1.0f, 4, 0.5f, 16.0f);
    AETHER_CHECK(hm.width == 65);
    AETHER_CHECK(hm.height == 65);
    AETHER_CHECK(hm.heights.size() == 65 * 65);
    // Heights should be in a reasonable range (-16 to +16).
    for (f32 h : hm.heights) {
        AETHER_CHECK(h >= -18.0f && h <= 18.0f);
    }
}

AETHER_TEST(TerrainHeightmap_LinearSampling) {
    Heightmap hm;
    hm.width = 5;
    hm.height = 5;
    hm.cell_size = 1.0f;
    // Flat plane at height 5.
    hm.heights = std::vector<f32>(25, 5.0f);
    AETHER_CHECK_NEAR(hm.SampleLinear(2.0f, 2.0f), 5.0f, 1e-4f);
    // Corner.
    AETHER_CHECK_NEAR(hm.SampleLinear(0.0f, 0.0f), 5.0f, 1e-4f);
    AETHER_CHECK_NEAR(hm.SampleLinear(4.0f, 4.0f), 5.0f, 1e-4f);
}

AETHER_TEST(TerrainData_ChunkLayout) {
    Heightmap hm = CreateProceduralHeightmap(65, 65, 1.0f, 2, 0.5f, 8.0f);
    TerrainSettings settings;
    settings.verts_per_chunk = 32;
    settings.chunk_world_size = 31.0f;
    settings.vertical_scale = 1.0f;

    TerrainData data;
    InitTerrainData(data, hm, settings);

    AETHER_CHECK(data.chunk_count_x > 0);
    AETHER_CHECK(data.chunk_count_z > 0);
    AETHER_CHECK(data.chunk_size == 32);

    // Bounds should reflect the world size.
    AETHER_CHECK(data.bounds_max.x > data.bounds_min.x);
    AETHER_CHECK(data.bounds_max.z > data.bounds_min.z);
}

AETHER_TEST(TerrainChunks_BuildAndBounds) {
    Heightmap hm = CreateProceduralHeightmap(65, 65, 1.0f, 2, 0.5f, 8.0f);
    TerrainSettings settings;
    settings.verts_per_chunk = 32;
    settings.chunk_world_size = 31.0f;
    settings.vertical_scale = 1.0f;

    TerrainData data;
    InitTerrainData(data, hm, settings);

    auto chunks = BuildChunks(data, settings);
    AETHER_CHECK(chunks.size() == data.chunk_count_x * data.chunk_count_z);
    for (const auto& c : chunks) {
        // Every chunk should have valid world bounds.
        AETHER_CHECK(c.world_min.x < c.world_max.x);
        AETHER_CHECK(c.world_min.z < c.world_max.z);
        // LOD 0 chunk should have verts_per_side = chunk_size.
        AETHER_CHECK(c.verts_per_side == data.chunk_size);
        AETHER_CHECK(c.is_dirty == true);
    }
}

AETHER_TEST(TerrainChunks_VertexGeneration) {
    Heightmap hm = CreateProceduralHeightmap(32, 32, 1.0f, 1, 0.5f, 4.0f);
    TerrainSettings settings;
    settings.verts_per_chunk = 32;
    settings.chunk_world_size = 31.0f;
    settings.vertical_scale = 1.0f;

    TerrainData data;
    InitTerrainData(data, hm, settings);

    auto chunks = BuildChunks(data, settings);
    AETHER_CHECK(chunks.size() == 1);

    auto verts = GenerateChunkVertices(data, settings, chunks[0]);
    // 32x32 vertices, 8 floats each.
    AETHER_CHECK(verts.size() == 32u * 32u * 8);

    // Check position is in world bounds.
    AETHER_CHECK(verts[0] >= 0.0f && verts[0] <= 31.0f); // x
    AETHER_CHECK(verts[2] >= 0.0f && verts[2] <= 31.0f); // z
    // Normal should be normalized.
    const f32 nx = verts[3], ny = verts[4], nz = verts[5];
    AETHER_CHECK_NEAR(std::sqrt(nx*nx + ny*ny + nz*nz), 1.0f, 1e-4f);
}

AETHER_TEST(TerrainChunks_IndexGeneration) {
    Heightmap hm = CreateProceduralHeightmap(32, 32, 1.0f, 1, 0.5f, 4.0f);
    TerrainSettings settings;
    settings.verts_per_chunk = 32;
    settings.chunk_world_size = 31.0f;
    settings.vertical_scale = 1.0f;

    TerrainData data;
    InitTerrainData(data, hm, settings);
    auto chunks = BuildChunks(data, settings);
    auto indices = GenerateChunkIndices(data, chunks[0]);

    // 31x31 quads * 2 triangles * 3 indices.
    AETHER_CHECK(indices.size() == 31u * 31u * 2u * 3u);

    // All indices should be in range.
    for (u32 idx : indices) {
        AETHER_CHECK(idx < 32u * 32u);
    }
}

AETHER_TEST(TerrainHeightmap_LODSelection) {
    TerrainData data;
    data.lod_distances = {32.0f, 64.0f, 128.0f};
    data.max_lod = 3;

    AETHER_CHECK(SelectLod(data, 10.0f) == 0);
    AETHER_CHECK(SelectLod(data, 32.0f) == 0);
    AETHER_CHECK(SelectLod(data, 33.0f) == 1);
    AETHER_CHECK(SelectLod(data, 64.0f) == 1);
    AETHER_CHECK(SelectLod(data, 65.0f) == 2);
    AETHER_CHECK(SelectLod(data, 200.0f) == 3); // beyond all thresholds
}

AETHER_TEST(TerrainHeight_Sampling) {
    Heightmap hm = CreateProceduralHeightmap(65, 65, 1.0f, 1, 0.5f, 8.0f);
    TerrainSettings settings;
    settings.verts_per_chunk = 32;
    settings.chunk_world_size = 31.0f;
    settings.vertical_scale = 2.0f;

    TerrainData data;
    InitTerrainData(data, hm, settings);

    // Sample at various positions.
    const f32 h = SampleTerrainHeight(data, 32.0f, 32.0f);
    AETHER_CHECK(h != 0.0f || hm.heights[0] != 0.0f); // non-trivial height
}
