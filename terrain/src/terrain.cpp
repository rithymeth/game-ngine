#include ""aether/terrain/terrain.h""

#include <algorithm>
#include <cmath>

namespace aether {
namespace terrain {

// --- Heightmap ---

f32 Heightmap::GetHeight(u32 x, u32 z) const {
    if (x >= width || z >= height) return 0.0f;
    return heights[static_cast<usize>(z) * width + x];
}

f32 Heightmap::SampleLinear(f32 wx, f32 wz) const {
    if (width == 0 || height == 0) return 0.0f;
    const f32 gx = wx / cell_size;
    const f32 gz = wz / cell_size;
    const i32 x0 = static_cast<i32>(std::floor(gx));
    const i32 z0 = static_cast<i32>(std::floor(gz));
    const u32 x1 = static_cast<u32>(x0 + 1);
    const u32 z1 = static_cast<u32>(z0 + 1);
    const f32 fx = gx - static_cast<f32>(x0);
    const f32 fz = gz - static_cast<f32>(z0);

    const f32 h00 = GetHeight(static_cast<u32>(std::max(0, x0)), static_cast<u32>(std::max(0, z0)));
    const f32 h10 = GetHeight(x1, static_cast<u32>(std::max(0, z0)));
    const f32 h01 = GetHeight(static_cast<u32>(std::max(0, x0)), z1);
    const f32 h11 = GetHeight(x1, z1);

    const f32 h0 = h00 + (h10 - h00) * fx;
    const f32 h1 = h01 + (h11 - h01) * fx;
    return h0 + (h1 - h0) * fz;
}

// --- Noise ---

namespace {

f32 Smoothstep(f32 t) { return t * t * (3.0f - 2.0f * t); }

i32 Hash(i32 x, i32 z) {
    i32 n = x + z * 57;
    n = (n << 13) ^ n;
    return n * (n * n * 15731 + 789221) + 1376312589;
}

f32 FloatFromHash(i32 h) {
    return static_cast<f32>(static_cast<i32>(h & 0x0fffffff) ^ 0x40000000) / 2147483648.0f;
}

f32 Noise2D(i32 x, i32 z) {
    const f32 fx = static_cast<f32>(x) - std::floor(static_cast<f32>(x));
    const f32 fz = static_cast<f32>(z) - std::floor(static_cast<f32>(z));
    const f32 sx = Smoothstep(fx);
    const f32 sz = Smoothstep(fz);

    const f32 n00 = FloatFromHash(Hash(x,     z));
    const f32 n10 = FloatFromHash(Hash(x + 1, z));
    const f32 n01 = FloatFromHash(Hash(x,     z + 1));
    const f32 n11 = FloatFromHash(Hash(x + 1, z + 1));

    const f32 nx0 = n00 + (n10 - n00) * sx;
    const f32 nx1 = n01 + (n11 - n01) * sx;
    return nx0 + (nx1 - nx0) * sz;
}

} // namespace

Heightmap CreateProceduralHeightmap(u32 width, u32 height, f32 cell_size,
                                     u32 octaves, f32 persistence, f32 scale) {
    Heightmap hm;
    hm.width = width;
    hm.height = height;
    hm.cell_size = cell_size;
    hm.heights.resize(static_cast<usize>(width) * height, 0.0f);

    for (u32 z = 0; z < height; ++z) {
        for (u32 x = 0; x < width; ++x) {
            f32 height_value = 0.0f;
            f32 amplitude = 1.0f;
            f32 frequency = 1.0f;
            f32 max_val = 0.0f;

            for (u32 o = 0; o < octaves; ++o) {
                height_value += amplitude * Noise2D(
                    static_cast<i32>(x) * static_cast<i32>(frequency),
                    static_cast<i32>(z) * static_cast<i32>(frequency));
                max_val += amplitude;
                amplitude *= persistence;
                frequency *= 2.0f;
            }

            hm.heights[static_cast<usize>(z) * width + x] =
                (height_value / max_val) * scale;
        }
    }
    return hm;
}

// --- Init ---

void InitTerrainData(TerrainData& data, const Heightmap& hm, const TerrainSettings& settings) {
    data.heightmap = hm;
    if (data.chunk_size == 0) data.chunk_size = settings.verts_per_chunk;
    if (data.chunk_size == 0) data.chunk_size = 32;

    // Count chunks needed to cover the heightmap.
    const u32 verts_x = hm.width;
    const u32 verts_z = hm.height;
    const u32 vpc = data.chunk_size; // vertices per chunk side

    // Overlap by 1 vertex at the edges so chunks stitch together.
    const u32 step = vpc > 1 ? vpc - 1 : 1;
    data.chunk_count_x = (verts_x + step - 1) / step;
    data.chunk_count_z = (verts_z + step - 1) / step;

    // Compute bounds from heightmap.
    f32 min_h =  1e9f;
    f32 max_h = -1e9f;
    for (f32 h : hm.heights) {
        min_h = std::min(min_h, h);
        max_h = std::max(max_h, h);
    }
    const f32 world_w = static_cast<f32>(verts_x - 1) * hm.cell_size;
    const f32 world_d = static_cast<f32>(verts_z - 1) * hm.cell_size;
    data.bounds_min = {0.0f, min_h * settings.vertical_scale, 0.0f};
    data.bounds_max = {world_w, max_h * settings.vertical_scale, world_d};

    // Default layer.
    if (data.layers.empty()) {
        data.layers.push_back(TerrainLayer{
            .name = ""Default"",
            .albedo = Vec3(0.5f, 0.7f, 0.3f), // grass green
            .metallic = 0.0f,
            .roughness = 0.9f
        });
    }
}

// --- Chunk bounds ---

static void ComputeChunkBounds(const TerrainData& data, const TerrainSettings& settings,
                               TerrainChunk& chunk) {
    const f32 world_size = settings.chunk_world_size;
    const u32 vps = data.chunk_size;
    const u32 lod_step = 1u << chunk.lod_level;
    const u32 lvps = (vps - 1) / lod_step + 1;
    const u32 step = vps - 1;

    const f32 origin_x = static_cast<f32>(chunk.chunk_x) * world_size;
    const f32 origin_z = static_cast<f32>(chunk.chunk_z) * world_size;
    const f32 step_x = world_size / static_cast<f32>(step);
    const f32 step_z = world_size / static_cast<f32>(step);
    const f32 vs = settings.vertical_scale;

    f32 min_h =  1e9f;
    f32 max_h = -1e9f;

    for (u32 z = 0; z < lvps; ++z) {
        for (u32 x = 0; x < lvps; ++x) {
            const u32 hx = std::min(x * lod_step, step);
            const u32 hz = std::min(z * lod_step, step);
            const f32 h = data.heightmap.GetHeight(hx, hz) * vs;
            min_h = std::min(min_h, h);
            max_h = std::max(max_h, h);
        }
    }

    chunk.world_min = {origin_x, min_h, origin_z};
    chunk.world_max = {origin_x + world_size, max_h, origin_z + world_size};
}

// --- Chunk building ---

std::vector<TerrainChunk> BuildChunks(const TerrainData& data,
                                       const TerrainSettings& settings) {
    std::vector<TerrainChunk> chunks;
    const u32 vps = data.chunk_size;
    const u32 lvps0 = (vps - 1) / (1u << 0) + 1; // LOD 0 vertex count

    for (u32 cz = 0; cz < data.chunk_count_z; ++cz) {
        for (u32 cx = 0; cx < data.chunk_count_x; ++cx) {
            TerrainChunk chunk;
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

// --- Vertex generation ---

std::vector<f32> GenerateChunkVertices(const TerrainData& data,
                                         const TerrainSettings& settings,
                                         const TerrainChunk& chunk) {
    const u32 vps = data.chunk_size;
    const f32 world_size = settings.chunk_world_size;
    const f32 step = world_size / static_cast<f32>(vps - 1);
    const f32 vs = settings.vertical_scale;

    const u32 lod_step = 1u << chunk.lod_level;
    const u32 lvps = (vps - 1) / lod_step + 1;
    const usize vert_count = static_cast<usize>(lvps) * lvps;

    // Layout: position(3) + normal(3) + uv(2) = 8 floats per vertex.
    std::vector<f32> vertices(vert_count * 8, 0.0f);

    const f32 origin_x = static_cast<f32>(chunk.chunk_x) * world_size;
    const f32 origin_z = static_cast<f32>(chunk.chunk_z) * world_size;
    const u32 edge_verts = vps - 1;

    for (u32 z = 0; z < lvps; ++z) {
        for (u32 x = 0; x < lvps; ++x) {
            const u32 hx = std::min(x * lod_step, edge_verts);
            const u32 hz = std::min(z * lod_step, edge_verts);
            const f32 lx = origin_x + static_cast<f32>(x * lod_step) * step;
            const f32 lz = origin_z + static_cast<f32>(z * lod_step) * step;
            const f32 ly = data.heightmap.GetHeight(hx, hz) * vs;

            // Central-difference normal.
            const u32 hxm = hx > 0 ? hx - 1 : 0;
            const u32 hxp = std::min(hx + 1, edge_verts);
            const u32 hzm = hz > 0 ? hz - 1 : 0;
            const u32 hzp = std::min(hz + 1, edge_verts);
            const f32 hx0 = data.heightmap.GetHeight(hxm, hz) * vs;
            const f32 hx1 = data.heightmap.GetHeight(hxp, hz) * vs;
            const f32 hz0 = data.heightmap.GetHeight(hx, hzm) * vs;
            const f32 hz1 = data.heightmap.GetHeight(hx, hzp) * vs;

            const f32 ds = step * static_cast<f32>(lod_step);
            const Vec3 tangent{2.0f * ds, hx1 - hx0, 0.0f};
            const Vec3 bitan{0.0f, hz1 - hz0, 2.0f * ds};
            const Vec3 normal = Normalize(Cross(tangent, bitan));

            const usize i = static_cast<usize>(z * lvps + x) * 8;
            vertices[i + 0] = lx;
            vertices[i + 1] = ly;
            vertices[i + 2] = lz;
            vertices[i + 3] = normal.x;
            vertices[i + 4] = normal.y;
            vertices[i + 5] = normal.z;
            vertices[i + 6] = static_cast<f32>(x) / static_cast<f32>(lvps - 1);
            vertices[i + 7] = static_cast<f32>(z) / static_cast<f32>(lvps - 1);
        }
    }
    return vertices;
}

// --- Index generation ---

std::vector<u32> GenerateChunkIndices(const TerrainData& data,
                                       const TerrainChunk& chunk) {
    const u32 vps = data.chunk_size;
    const u32 lod_step = 1u << chunk.lod_level;
    const u32 lvps = (vps - 1) / lod_step + 1;

    // Indexed triangle list.
    const usize tri_count = static_cast<usize>(lvps - 1) * (lvps - 1);
    std::vector<u32> indices;
    indices.reserve(tri_count * 6);

    for (u32 z = 0; z < lvps - 1; ++z) {
        for (u32 x = 0; x < lvps - 1; ++x) {
            const u32 i0 = z * lvps + x;
            const u32 i1 = i0 + 1;
            const u32 i2 = (z + 1) * lvps + x;
            const u32 i3 = i2 + 1;

            // Two CCW triangles per quad.
            indices.push_back(i0);
            indices.push_back(i2);
            indices.push_back(i1);
            indices.push_back(i1);
            indices.push_back(i2);
            indices.push_back(i3);
        }
    }
    return indices;
}

// --- LOD selection ---

u32 SelectLod(const TerrainData& data, f32 camera_distance) {
    for (usize i = 0; i < data.lod_distances.size(); ++i) {
        if (camera_distance <= data.lod_distances[i]) {
            return static_cast<u32>(i);
        }
    }
    return data.max_lod;
}

// --- Height sampling ---

f32 SampleTerrainHeight(const TerrainData& data, f32 world_x, f32 world_z) {
    return data.heightmap.SampleLinear(world_x, world_z) * 1.0f;
}

} // namespace terrain
} // namespace aether
