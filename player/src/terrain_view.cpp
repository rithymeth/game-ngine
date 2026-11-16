#include "terrain_view.h"

#include "aether/assets/image.h"
#include "aether/core/log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace aether::player {

namespace {

bool SameTerrain(const ProceduralTerrain& a, const ProceduralTerrain& b) {
    return a.seed == b.seed && a.samples == b.samples && a.cell_size == b.cell_size && a.height_scale == b.height_scale && a.frequency == b.frequency &&
           a.ridge_amount == b.ridge_amount && a.sea_level == b.sea_level && a.river_min_catchment == b.river_min_catchment &&
           a.hydraulic_droplets == b.hydraulic_droplets;
}

f32 Hash01(u32 x, u32 y, u32 seed) {
    u32 h = x * 374761393u + y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<f32>(h & 0xffffffu) / 16777215.0f;
}

// Smooth noise on an 8 x 8 periodic lattice, so the 64 x 64 texture tiles without a seam.
f32 TileNoise(f32 u, f32 v, u32 seed) {
    constexpr u32 kLattice = 8;
    const f32 x = u * kLattice, y = v * kLattice;
    const u32 x0 = static_cast<u32>(std::floor(x)) % kLattice, y0 = static_cast<u32>(std::floor(y)) % kLattice;
    const u32 x1 = (x0 + 1) % kLattice, y1 = (y0 + 1) % kLattice;
    f32 fx = x - std::floor(x), fy = y - std::floor(y);
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    const f32 a = Hash01(x0, y0, seed), b = Hash01(x1, y0, seed), c = Hash01(x0, y1, seed), d = Hash01(x1, y1, seed);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

f32 LinearToSrgb(f32 c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// A layer's tiling texture: its colour with a little grain, and a height in alpha that the layer blend uses to let rock poke through grass.
gfx::rhi::SampledTextureHandle MakeLayerTexture(gfx::rhi::IDevice& device, const terrain::TerrainLayer& layer, u32 seed) {
    constexpr u32 kSize = 64;
    std::vector<u8> pixels(static_cast<usize>(kSize) * kSize * 4);
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            const f32 u = static_cast<f32>(x) / kSize, v = static_cast<f32>(y) / kSize;
            const f32 coarse = TileNoise(u, v, seed), fine = TileNoise(u * 4.0f, v * 4.0f, seed + 7);
            const f32 grain = 0.78f + 0.3f * coarse + 0.12f * fine; // about 0.78 to 1.2
            u8* p = &pixels[(static_cast<usize>(y) * kSize + x) * 4];
            p[0] = static_cast<u8>(std::lround(LinearToSrgb(layer.albedo.x * grain) * 255.0f));
            p[1] = static_cast<u8>(std::lround(LinearToSrgb(layer.albedo.y * grain) * 255.0f));
            p[2] = static_cast<u8>(std::lround(LinearToSrgb(layer.albedo.z * grain) * 255.0f));
            p[3] = static_cast<u8>(std::lround(std::clamp(0.2f + 0.65f * (0.6f * coarse + 0.4f * fine), 0.0f, 1.0f) * 255.0f));
        }
    }
    u32 mips = 0;
    const std::vector<u8> chain = assets::BuildMipChain(kSize, kSize, pixels, assets::MipFilter::Srgb, mips);
    if (mips > 1) return device.CreateTextureWithMips(kSize, kSize, mips, chain.data());
    return device.CreateTexture(kSize, kSize, pixels.data());
}

} // namespace

struct TerrainView::Impl {
    gfx::rhi::IDevice& device;
    gfx::rhi::ISwapChain& swap_chain;
    terrain::RhiTerrainGpu gpu; // before `renderer`: the renderer frees its chunks through it
    terrain::TerrainRenderer renderer;
    terrain::TerrainPass pass;

    ProceduralTerrain built;
    bool have_ground = false;
    terrain::GeneratedTerrain ground;
    terrain::TerrainData data;
    terrain::TerrainSettings settings;
    terrain::TerrainDrawParams params;
    Stats stats;

    Impl(gfx::rhi::IDevice& d, gfx::rhi::ISwapChain& s, bool hdr) : device(d), swap_chain(s), gpu(d), renderer(gpu), pass(d, s, hdr) {}

    // The splatmap and layer textures this view made (the device retires them once the frames still drawing with them finish).
    void FreeTextures() {
        if (params.splatmap.IsValid()) device.DestroyTexture(params.splatmap);
        for (terrain::TerrainLayerBinding& layer : params.layers) {
            if (layer.albedo_height.IsValid()) device.DestroyTexture(layer.albedo_height);
        }
        params = terrain::TerrainDrawParams{};
    }

    void Build(const ProceduralTerrain& t) {
        const auto start = std::chrono::steady_clock::now();
        renderer.Destroy();
        FreeTextures();
        have_ground = false;
        const terrain::TerrainGenSettings gen = ToGenSettings(t);
        ground = terrain::GenerateTerrain(gen);

        settings = terrain::TerrainSettings{};
        settings.verts_per_chunk = 33;
        settings.chunk_world_size = 32.0f * gen.cell_size;
        data = terrain::TerrainData{};
        data.chunk_size = settings.verts_per_chunk;
        terrain::InitTerrainData(data, ground.heightmap, settings);
        // Detail steps out with the world's scale, not a fixed 64 m: the first level covers a few chunks around the camera.
        const f32 chunk = settings.chunk_world_size;
        data.lod_distances = {chunk * 2.0f, chunk * 4.0f, chunk * 8.0f};
        renderer.Build(data, settings);

        // The splatmap, one weight texel per height sample, and the four layers' textures.
        const std::vector<terrain::TerrainLayer> layers = terrain::DefaultTerrainLayers();
        std::vector<terrain::SplatmapLayer> splat_layers;
        for (const terrain::TerrainLayer& l : layers) {
            terrain::SplatmapLayer s;
            s.name = l.name;
            s.albedo_texture_id = 0;
            s.normal_texture_id = 0;
            s.albedo_tint = l.albedo;
            splat_layers.push_back(std::move(s));
        }
        const u32 resolution = std::clamp<u32>(gen.width - 1, 8, 1024);
        const terrain::SplatmapData splat = terrain::BuildTerrainSplatmap(ground, gen, splat_layers, resolution);
        params = terrain::TerrainDrawParams{};
        params.splatmap = terrain::UploadSplatmap(device, splat);
        const f32 size_x = static_cast<f32>(gen.width - 1) * gen.cell_size, size_z = static_cast<f32>(gen.height - 1) * gen.cell_size;
        params.splat_origin[0] = params.splat_origin[1] = 0.0f;
        params.splat_size[0] = size_x;
        params.splat_size[1] = size_z;
        bool textures_ok = params.splatmap.IsValid();
        const f32 tiles[4] = {0.22f, 0.12f, 0.2f, 0.16f}; // texture repeats per metre: grass, rock, sand, snow
        for (usize i = 0; i < 4; ++i) {
            params.layers[i].albedo_height = MakeLayerTexture(device, layers[i], static_cast<u32>(31 + i * 17));
            params.layers[i].tile_scale = tiles[i];
            params.layers[i].roughness = layers[i].roughness;
            params.layers[i].metallic = layers[i].metallic;
            textures_ok = textures_ok && params.layers[i].albedo_height.IsValid();
        }
        have_ground = textures_ok && renderer.UploadedCount() > 0;
        if (!textures_ok) AETHER_LOG_WARN("Terrain", "The device has no room for the terrain's textures; terrain is not drawn");
        built = t;
        ++stats.builds;
        stats.build_ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
        AETHER_LOG_INFO("Terrain", "Built a %ux%u terrain (%.0f m a side, seed %u) in %.0f ms: %zu chunks", gen.width, gen.height,
                        static_cast<double>(size_x), gen.seed, stats.build_ms, static_cast<size_t>(renderer.UploadedCount()));
    }
};

TerrainView::TerrainView(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& swap_chain, bool hdr_target)
    : impl_(std::make_unique<Impl>(device, swap_chain, hdr_target)) {}

TerrainView::~TerrainView() {
    if (!impl_) return;
    impl_->renderer.Destroy();
    impl_->FreeTextures();
}

bool TerrainView::Valid() const { return impl_->pass.Valid(); }

bool TerrainView::Update(const ProceduralTerrain& terrain, const Vec3& camera_position) {
    if (!Valid()) return false;
    if (!impl_->have_ground && impl_->stats.builds > 0 && SameTerrain(impl_->built, terrain)) return false; // tried and failed: not every frame
    if (impl_->stats.builds == 0 || !SameTerrain(impl_->built, terrain)) impl_->Build(terrain);
    if (impl_->have_ground) impl_->renderer.UpdateLOD(impl_->data, impl_->settings, camera_position);
    return impl_->have_ground;
}

u32 TerrainView::Draw(gfx::rhi::ICommandList& commands, const Mat4& view_projection, const Vec3& camera_position, const Vec3& sun_direction,
                      f32 sun_intensity) {
    Impl& s = *impl_;
    s.stats.chunks_drawn = 0;
    s.stats.triangles_drawn = 0;
    if (!s.have_ground || !s.pass.Valid()) return 0;
    s.params.camera_position = camera_position;
    s.params.sun_direction = sun_direction;
    s.params.sun_intensity = sun_intensity;
    const u32 drawn = s.pass.Draw(commands, s.gpu, s.renderer.Chunks(), view_projection, s.params);
    s.stats.chunks_drawn = drawn;
    for (const terrain::RenderChunk& c : s.renderer.Chunks()) s.stats.triangles_drawn += c.index_count / 3;
    return drawn;
}

TerrainView::Stats TerrainView::GetStats() const { return impl_->stats; }

const terrain::GeneratedTerrain* TerrainView::Ground() const { return impl_->have_ground ? &impl_->ground : nullptr; }

} // namespace aether::player
