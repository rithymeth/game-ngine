#pragma once

#include "aether/gfx/parity.h"
#include "aether/gfx/rhi/device.h"
#include "aether/terrain/renderer.h"
#include "aether/terrain/splat.h"

#include <array>
#include <span>
#include <vector>

namespace aether::terrain {

// Terrain on the GPU (Priority 3): the RHI side of the chunk meshes and splatmaps that terrain/ builds on the CPU, and a pass that draws them
// with the height-aware layer blend (BlendTerrainLayerHeights) in the pixel shader, on Vulkan and D3D12 from one HLSL source.
//
// A layer's texture holds its albedo in RGB (sRGB-encoded, like every other colour texture) and its height in alpha. The splatmap is an RGBA8
// texture of the four layers' painted weights, sampled with the same texel mapping as SplatmapData::GetWeights (u = 0 is the first texel's
// centre, u = 1 the last's), so the GPU reads the weights the CPU would. The blend, the same arithmetic as BlendTerrainLayerHeights, turns the
// painted weights and the layers' heights into the weights the albedos are mixed with. Lighting is diffuse only for now: a sky term and one sun.

// A TerrainGpu over the RHI: TerrainRenderer's chunk buffers become vertex and index buffers on the device. The RHI cannot free a buffer
// (it has no destroy), so DestroyBuffer forgets the handle and the device keeps the memory until it goes away.
class RhiTerrainGpu final : public TerrainGpu {
public:
    explicit RhiTerrainGpu(gfx::rhi::IDevice& device) : device_(device) {}
    u64 CreateBuffer(const void* data, u64 bytes, bool index) override;
    void DestroyBuffer(u64 buffer) override;
    // The device buffer for an id from CreateBuffer; invalid for 0, an unknown id or a destroyed one.
    gfx::rhi::BufferHandle Find(u64 buffer) const;
    usize Live() const { return live_; }

private:
    gfx::rhi::IDevice& device_;
    std::vector<gfx::rhi::BufferHandle> buffers_; // id - 1
    usize live_ = 0;
};

// An RGBA8 texture of the splatmap's weights on the device (an invalid handle when the device has no room or the splatmap is empty).
gfx::rhi::SampledTextureHandle UploadSplatmap(gfx::rhi::IDevice& device, const SplatmapData& splatmap);

struct TerrainLayerBinding {
    gfx::rhi::SampledTextureHandle albedo_height; // RGB albedo, A height
    f32 tile_scale = 0.1f;                        // texture repeats per world unit
};

struct TerrainDrawParams {
    std::array<TerrainLayerBinding, 4> layers;
    gfx::rhi::SampledTextureHandle splatmap;
    f32 splat_origin[2] = {0.0f, 0.0f}; // world x, z of the splatmap's corner (u = v = 0)
    f32 splat_size[2] = {1.0f, 1.0f};   // world width and depth it covers
    f32 height_contrast = 0.7f;         // BlendTerrainLayerHeights' contrast, 0..1
    Vec3 sun_direction{-0.4f, 0.8f, -0.35f}; // towards the sun (normalized in the shader)
    f32 sun_intensity = 3.0f;
    f32 sky_intensity = 1.0f;
};

// The push constants, exactly 128 bytes: the matrix, the splatmap's world rectangle, the tiling, the sun and the contrast, and the packed texture
// indices (four layers and the splatmap, 6 bits each) with the two intensities.
struct TerrainConstants {
    Mat4 view_projection;
    f32 splat_rect[4];  // origin x, origin z, 1 / width, 1 / depth
    f32 layer_tiles[4];
    f32 light[4];       // sun direction xyz (towards the sun), height contrast
    u32 packed[4];      // x: layer 0..3 texture indices at bits 0, 6, 12, 18 and the splatmap's at 24; y, z: sun and sky intensity as float bits
};
static_assert(sizeof(TerrainConstants) == 128);
TerrainConstants PackTerrainConstants(const Mat4& view_projection, const TerrainDrawParams& params);

// The HLSL (VSMain, PSMain) and the blend function it shares with the parity scene.
const char* TerrainShaderSource();
const char* TerrainBlendHlsl();

class TerrainPass {
public:
    TerrainPass(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& swap_chain);
    bool Valid() const { return pipeline_.IsValid(); }
    // Records the chunks' draws inside the current render pass (depth-tested against, and writing, the swap chain's depth). Returns how many
    // chunks were drawn (those with live buffers).
    u32 Draw(gfx::rhi::ICommandList& commands, const RhiTerrainGpu& gpu, std::span<const RenderChunk> chunks, const Mat4& view_projection,
             const TerrainDrawParams& params);

private:
    gfx::rhi::PipelineHandle pipeline_;
};

// Full-screen scenes for the backend parity harness (gfx/parity.h): `terrain_height_blend` draws the shader's blend of painted weights and layer
// heights across the target and the reference is BlendTerrainLayerHeights itself, so a backend that blends differently from the CPU shows up.
const std::vector<gfx::parity::Scene>& TerrainParityScenes();

// What the terrain shader outputs for one pixel of a surface facing up, on the CPU: the albedos (linear) mixed by the blended weights, lit by the
// sky and the sun, tone mapped and sRGB-encoded, as 8-bit channels. For tests and tools.
std::array<u8, 3> TerrainReferenceColor(const std::array<Vec3, 4>& albedo_linear, const std::array<f32, 4>& painted_weights,
                                        const std::array<f32, 4>& heights, const TerrainDrawParams& params);

} // namespace aether::terrain
