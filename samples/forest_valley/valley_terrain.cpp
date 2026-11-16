#include "valley_terrain.h"

#include "aether/terrain/renderer.h"

#include <algorithm>

namespace aether::forest_valley {

namespace {

// A TerrainGpu that keeps no data, only how much it was asked to hold.
class CountingGpu final : public terrain::TerrainGpu {
public:
    u64 CreateBuffer(const void*, u64 bytes, bool) override {
        live_[++next_] = bytes;
        return next_;
    }
    void DestroyBuffer(u64 id) override { live_.erase(id); }
    u64 Bytes() const {
        u64 total = 0;
        for (const auto& [id, bytes] : live_) total += bytes;
        return total;
    }

private:
    std::unordered_map<u64, u64> live_;
    u64 next_ = 0;
};

} // namespace

ValleyTerrain BuildValleyTerrain(const World& world) {
    ValleyTerrain v;
    v.settings.verts_per_chunk = kVertsPerChunk;
    v.settings.chunk_world_size = static_cast<f32>(kVertsPerChunk - 1) * world.settings.cell_size;
    v.data.chunk_size = kVertsPerChunk;
    terrain::InitTerrainData(v.data, world.terrain.heightmap, v.settings);
    v.layers = terrain::DefaultTerrainLayers();

    std::vector<terrain::SplatmapLayer> splat_layers;
    for (const terrain::TerrainLayer& l : v.layers) {
        terrain::SplatmapLayer s;
        s.name = l.name;
        s.albedo_texture_id = 0; // plain colours until the showcase has textures
        s.normal_texture_id = 0;
        s.albedo_tint = l.albedo;
        s.metallic = l.metallic;
        s.roughness = l.roughness;
        s.normal_strength = l.normal_strength;
        splat_layers.push_back(std::move(s));
    }
    v.splatmap = terrain::BuildTerrainSplatmap(world.terrain, world.settings, splat_layers, kSplatResolution);
    return v;
}

TerrainFramePlan PlanTerrainFrame(const ValleyTerrain& valley, const Vec3& camera) {
    CountingGpu gpu;
    terrain::TerrainRenderer renderer(gpu);
    renderer.Build(valley.data, valley.settings);
    renderer.UpdateLOD(valley.data, valley.settings, camera);
    TerrainFramePlan plan;
    for (const terrain::RenderChunk& c : renderer.Chunks()) {
        if (c.vertex_buffer == 0 || c.index_buffer == 0) continue;
        ++plan.chunks;
        ++plan.lod_chunks[std::min<u32>(c.lod_level, static_cast<u32>(plan.lod_chunks.size() - 1))];
        plan.triangles += c.index_count / 3;
    }
    plan.gpu_bytes = gpu.Bytes();
    renderer.Destroy();
    return plan;
}

} // namespace aether::forest_valley
