#pragma once

#include "aether/gfx/rhi/device.h"
#include "aether/renderer/post.h"
#include "aether/renderer/render_scene.h"

#include <string>
#include <vector>

// The player's post pass (render roadmap step 1). Where the device has an HDR scene colour target (ISwapChain::SceneColorTexture: D3D12
// today) the scene draws into it in linear light, unclamped, and this pass turns that into the display image in the back buffer: exposure,
// the tone mapper (ACES or AgX, the CPU references in renderer/post.h), saturation, the vignette and the sRGB encoding, in one full-screen
// triangle that reads each pixel with a texel load. Where there is no such target (Vulkan for now) the scene shader tone maps itself, as it
// always did, and this pass is not made.
//
// Bloom (render roadmap step 4) runs between the scene pass and this one, into a chain of half-size render textures: a 13-tap downsample
// per level (with a Karis average on the first, so a single very bright pixel can't flash), then a 3x3 tent upsample back up the chain,
// each level added onto the one above it. The composite mixes the result into the scene before exposure: lerp(scene, bloom / levels,
// intensity), the energy-conserving form, so bright light spreads without the frame getting brighter overall.

namespace aether::player {

// The push constants: 32 bytes.
struct PostConstants {
    u32 scene_color = 0;  // bindless index of the HDR target
    f32 exposure = 1.0f;  // linear multiplier applied before the tone mapper
    u32 tonemapper = 0;   // Tonemapper: 0 ACES, 1 AgX, 2 None (clamp)
    f32 saturation = 1.0f;
    f32 vignette = 0.0f;  // 0..1
    u32 bloom = 0xffffffffu;    // bindless index of the bloom chain's top level, or none (anything >= kMaxBindlessTextures)
    f32 bloom_intensity = 0.0f; // 0..1, the composite's lerp weight
    f32 bloom_scale = 1.0f;     // 1 / levels: the accumulated chain back to the scene's brightness
};
static_assert(sizeof(PostConstants) == 32);

// The bloom passes' push constants: 32 bytes.
struct BloomConstants {
    u32 source = 0;                 // bindless index of the level read
    u32 karis = 0;                  // 1: the first downsample, with the Karis average
    f32 source_texel[2] = {0, 0};   // 1 / the source's size
    f32 target_inverse[2] = {0, 0}; // 1 / the target's size
    f32 radius = 1.0f;              // the upsample tent's radius, in source texels
    f32 padding = 0.0f;
};
static_assert(sizeof(BloomConstants) == 32);

// The volume settings as push constants: exposure is 2^exposure_compensation (scene light values are not physical units, so there is no
// EV100 metering yet), the rest are copied with their ranges clamped. Bloom is off in the result (PostPass::Draw turns it on when
// RenderBloom drew this frame); bloom_intensity holds the clamped setting either way.
PostConstants PackPostConstants(const PostProcessSettings& settings, u32 scene_color_index);

// What the pass does to one linear HDR value, on the CPU, as the shader does it: the reference the GPU test compares against. `u`, `v` are
// the pixel's position in 0..1 (for the vignette). Returns display-encoded (sRGB) values in 0..1.
Vec3 PostReference(const Vec3& linear, const PostConstants& constants, f32 u, f32 v);

// The HLSL (VSMain, PSMain), for the pipeline and for tests.
const char* PostShaderSource();
// The bloom passes' HLSL: the downsample, or with `upsample` the tent upsample.
std::string BloomShaderSource(bool upsample);

// The bloom chain's extents for a scene of the given size: BloomChain with at most kBloomLevels levels, none below 8 pixels a side.
constexpr u32 kBloomLevels = 5;
std::vector<Extent> PostBloomChain(u32 width, u32 height);

class PostPass {
public:
    // Makes the HDR target on `swap_chain` and the pipeline. Not Valid() where the device has no scene colour target or the pipeline fails.
    PostPass(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& swap_chain);
    ~PostPass();
    PostPass(const PostPass&) = delete;
    PostPass& operator=(const PostPass&) = delete;
    bool Valid() const { return pipeline_.IsValid() && scene_color_ != 0; }
    u32 SceneColorIndex() const { return scene_color_; }
    // The HDR image RenderBloom and Draw read: a bindless index of a texture the size of the scene colour target (the temporal
    // anti-aliasing's resolve), or 0 for the scene colour target itself (the default).
    void SetSource(u32 index) { source_ = index; }
    u32 Source() const { return source_ != 0 ? source_ : scene_color_; }
    // Records the full-screen draw inside the current back-buffer render pass, after the scene colour pass has ended. Composites the
    // bloom chain when RenderBloom drew it since the last Draw.
    void Draw(gfx::rhi::ICommandList& commands, const PostProcessSettings& settings);

    // Records the bloom chain from the scene colour target, outside any render pass, after the scene colour pass has ended and before the
    // back-buffer pass that Draws. Makes (or, after a resize, remakes) the chain's render textures first. False, with nothing recorded,
    // where the device has no render textures, the bindless table has no room for the chain (retried next call), or bloom_intensity <= 0.
    bool RenderBloom(gfx::rhi::ICommandList& commands, const PostProcessSettings& settings);
    // The chain as made for the last RenderBloom (empty before): its top level is what Draw composites.
    const std::vector<gfx::rhi::SampledTextureHandle>& BloomTextures() const { return bloom_levels_; }
    const std::vector<Extent>& BloomExtents() const { return bloom_extents_; }
    // Frees the chain's render textures (they come back on the next RenderBloom); for when bloom is turned off.
    void ReleaseBloom();

private:
    bool EnsureBloomChain(u32 width, u32 height);

    gfx::rhi::IDevice& device_;
    gfx::rhi::ISwapChain& swap_chain_;
    gfx::rhi::PipelineHandle pipeline_;
    gfx::rhi::PipelineHandle bloom_down_;
    gfx::rhi::PipelineHandle bloom_up_;
    u32 scene_color_ = 0;
    u32 source_ = 0; // SetSource's; 0: scene_color_
    std::vector<gfx::rhi::SampledTextureHandle> bloom_levels_;
    std::vector<Extent> bloom_extents_;
    u32 bloom_width_ = 0, bloom_height_ = 0; // the scene size the chain was made for
    bool bloom_ready_ = false;               // RenderBloom drew since the last Draw
    bool bloom_warned_ = false;              // the full-table warning, once
};

} // namespace aether::player
