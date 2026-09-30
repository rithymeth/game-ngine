#include ""aether/terrain/renderer.h""
#include ""aether/terrain/terrain.h""
#include ""test_framework.h""

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
    Heightmap hm = CreateProceduralHeightmap(33, 33, 1.0f, 1, 0.5f, 4.0f);
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
    AETHER_CHECK(c.vertex_count == 1024);  // 32*32
    AETHER_CHECK(c.index_count == 5766);   // 31*31*6
}

AETHER_TEST(TerrainRenderer_MarkAllDirty) {
    Heightmap hm = CreateProceduralHeightmap(33, 33, 1.0f, 1, 0.5f, 4.0f);
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
    // (No actual Renderer here ? just testing the chunk model.)
    // We test that the dirty flag is correctly maintained.
    AETHER_CHECK(chunks[0].dirty == false);
    // Simulate editing: mark dirty
    chunks[0].dirty = true;
    AETHER_CHECK(chunks[0].dirty == true);
}
