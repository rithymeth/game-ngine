#pragma once

#include "aether/gfx/rhi/device.h"
#include "aether/renderer/post.h"
#include "aether/renderer/render_scene.h"

#include <string>
#include <vector>

// The player's post pass (render roadmap step 1). Where the device has an HDR scene colour target (ISwapChain::SceneColorTexture: D3D12, and
// Vulkan where the texture table is update-after-bind) the scene draws into it in linear light, unclamped, and this pass turns that into the display image in the back buffer: exposure,
// the tone mapper (ACES or AgX, the CPU references in renderer/post.h), saturation, the vignette and the sRGB encoding, in one full-screen
// triangle that reads each pixel with a texel load. Where there is no such target (an older Vulkan device) the scene shader tone maps itself, as it
// always did, and this pass is not made.
//
// Bloom (render roadmap step 4) runs between the scene pass and this one, into a chain of half-size render textures: a 13-tap downsample
// per level (with a Karis average on the first, so a single very bright pixel can't flash), then a 3x3 tent upsample back up the chain,
// each level added onto the one above it. The composite mixes the result into the scene before exposure: lerp(scene, bloom / levels,
// intensity), the energy-conserving form, so bright light spreads without the frame getting brighter overall.
//
// Lens effects (chromatic aberration, sharpen and film grain) are in the composite too. The scene colour is read through chromatic
// aberration (red and blue from opposite sides of the pixel, more toward the edges) and a luminance-only sharpen before anything else
// touches it; the film grain is the last thing added, to the display-encoded colour. Their strengths, and the vignette's, travel as four 8-bit
// values in one push-constant word (PostConstants::lens) so the constants stay within the 128 bytes every device guarantees.
//
// Screen-space reflections are in the composite as well (PostProcessSettings::screen_space_reflections). A smooth surface needs its
// roughness and the ray a pixel reflects; there is no G-buffer, so the scene shader writes the roughness into the HDR target's alpha
// (AETHER_SSR, opaque draws only: water already mirrors, transparent draws blend it) and the depth copy gives the view-space position and
// normal. The mirror ray is marched in view space against that depth, refined by binary search, and the colour where it lands replaces the
// surface's sky-lit reflection by the Fresnel term. A miss (off screen, behind something, or the ray into the surface), a rough surface and
// the frame's edge all fade it back to what the sky lighting already gives, so nothing needs a reflection probe.

namespace aether { struct CineCamera; }

namespace aether::player {

struct DepthOfFieldConstants {
    u32 source = 0;
    u32 depth = 0xffffffffu;
    f32 near_plane = 0.1f;
    f32 far_plane = 1000.0f;
    f32 focus_distance = 10.0f;
    f32 radius_scale = 0.0f;
    f32 max_radius = 12.0f;
    f32 padding = 0.0f;
};
static_assert(sizeof(DepthOfFieldConstants) == 32);

DepthOfFieldConstants PackDepthOfField(const CineCamera& camera, u32 height, f32 near_plane, f32 far_plane);
f32 DepthOfFieldRadius(const DepthOfFieldConstants& constants, f32 distance);

// The push constants: 128 bytes.
struct PostConstants {
    u32 scene_color = 0;  // bindless index of the HDR target
    f32 exposure = 1.0f;  // linear multiplier applied before the tone mapper
    u32 tonemapper = 0;   // low 2 bits: Tonemapper (0 ACES, 1 AgX, 2 None (clamp)); bits 2-9: the screen-space reflections' strength; the rest: the frame number, which moves the film grain
    f32 saturation = 1.0f;
    u32 lens = 0;         // four 8-bit strengths, each 0..1: vignette (bits 0-7), chromatic aberration (8-15), film grain (16-23), sharpen (24-31)
    u32 textures = 0xffffffffu; // bloom index in bits 0-7, exposure index in bits 8-15; 255 means absent
    f32 bloom_intensity = 0.0f; // 0..1, the composite's lerp weight
    f32 bloom_scale = 1.0f;     // 1 / levels: the accumulated chain back to the scene's brightness
    u32 depth = 0xffffffffu;    // bindless index of the scene depth copy, or none: ambient occlusion off
    f32 ao_strength = 0.0f;     // 0..1
    f32 ao_radius = 1.0f;       // metres
    f32 near_plane = 0.1f;
    f32 far_plane = 1000.0f;
    f32 proj_x = 1.0f;          // the projection's [0][0] and [1][1] (1 / (aspect * tan(fov/2)), 1 / tan(fov/2))
    f32 proj_y = 1.0f;
    f32 fog_density = 0.0f;     // 1 / m
    f32 fog_color[4] = {0, 0, 0, 0}; // linear, the sky's
    f32 sun_view[4] = {0, 0, 0, 0};  // xyz: the unit vector toward the sun in view space; w: 1 when there is a sun to scatter
    f32 sun_color[4] = {0, 0, 0, 0}; // rgb: the sun's light the fog takes toward the sun (linear); w: the height fog's falloff, 1 / m
    f32 height_fog[4] = {0, 1, 0, 0}; // xyz: world up in view space; w: the camera's height above the height fog's base, metres (fog_color.w: its density at the base)
};
static_assert(sizeof(PostConstants) == 128);

constexpr u32 PackPostTextures(u32 bloom = 255u, u32 exposure = 255u) {
    return (bloom & 255u) | ((exposure & 255u) << 8);
}
constexpr u32 PostBloomOf(const PostConstants& c) { return c.textures & 255u; }
constexpr u32 PostExposureOf(const PostConstants& c) { return (c.textures >> 8) & 255u; }

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

// The lens word: each strength clamped to 0..1 (anything that is not a number counts as 0) and kept in 8 bits.
u32 PackLens(f32 vignette, f32 chromatic_aberration, f32 film_grain, f32 sharpen);
f32 LensVignette(u32 lens);
f32 LensChromaticAberration(u32 lens);
f32 LensFilmGrain(u32 lens);
f32 LensSharpen(u32 lens);
// The halves of PostConstants::tonemapper: the tone mapper, the screen-space reflections' strength (0..255, 0 off) and the frame number.
constexpr u32 PackTonemapperFrame(u32 tonemapper, u32 frame, u32 screen_space_reflections = 0) {
    return (tonemapper & 3u) | ((screen_space_reflections & 255u) << 2) | (frame << 10);
}
constexpr u32 PostTonemapperOf(const PostConstants& c) { return c.tonemapper & 3u; }
constexpr u32 PostScreenSpaceReflectionsOf(const PostConstants& c) { return (c.tonemapper >> 2) & 255u; }
constexpr u32 PostFrameOf(const PostConstants& c) { return c.tonemapper >> 10; }

// The volume settings as push constants: exposure is 2^exposure_compensation; GPU metering is bound separately by Draw.
// The rest are copied with their ranges clamped. Bloom is off in the result (PostPass::Draw turns it on when
// RenderBloom drew this frame); bloom_intensity holds the clamped setting either way. The reflections ride in the tone mapper's word
// (PostScreenSpaceReflectionsOf); they are off when the scene colour carries no roughness (alpha 1 everywhere) and the depth is not bound.
PostConstants PackPostConstants(const PostProcessSettings& settings, u32 scene_color_index);

// What the pass does to one linear HDR value, on the CPU, as the shader does it: the reference the GPU test compares against. `u`, `v` are
// the pixel's position in 0..1 (for the vignette). Returns display-encoded (sRGB) values in 0..1. The lens effects that read neighbouring
// pixels (chromatic aberration, sharpen) are not here, and neither is the grain: see PostReferenceWithGrain.
Vec3 PostReference(const Vec3& linear, const PostConstants& constants, f32 u, f32 v);
// PostReference, then the film grain of the pixel at column `x`, row `y`, as the shader adds it (the same noise on all three channels).
Vec3 PostReferenceWithGrain(const Vec3& linear, const PostConstants& constants, f32 u, f32 v, u32 x, u32 y);

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
    // Screen-space ambient occlusion for the next Draw only: the scene colour is darkened where the depth copy shows nearby surfaces closing
    // in (a sphere of `radius` metres, up to `strength` 0..1), before exposure and the tone mapper. `projection` is the camera's.
    struct AmbientOcclusion {
        u32 depth = 0xffffffffu;
        f32 strength = 0.0f, radius = 1.0f, near_plane = 0.1f, far_plane = 1000.0f, proj_x = 1.0f, proj_y = 1.0f;
    };
    void SetAmbientOcclusion(const AmbientOcclusion& ao) { ao_ = ao; }
    // The sky colour distance fog fades toward, for the next Draw (with the depth from SetAmbientOcclusion, whose strength may be 0).
    void SetFogColor(const Vec3& color) { fog_color_ = color; }
    // Where the camera is relative to the height fog: world up in view space and the camera's height above the fog's base (the pass needs them to know how high each pixel is).
    void SetHeightFogView(const Vec3& up_view, f32 camera_height_above_base) { fog_up_view_ = up_view; fog_camera_height_ = camera_height_above_base; }
    // The sun's in-scatter in the fog, for the next Draw: toward the sun (`direction_view`, a unit vector in view space) the fog takes `color`.
    void SetFogSun(const Vec3& direction_view, const Vec3& color) { sun_view_ = direction_view; sun_color_ = color; sun_set_ = true; }
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

    // After RenderBloom, outside a render pass: blur the HDR composite before Draw applies display effects.
    bool RenderDepthOfField(gfx::rhi::ICommandList& commands, const PostProcessSettings& settings, const CineCamera& camera);

    bool RenderAutoExposure(gfx::rhi::ICommandList& commands, const PostProcessSettings& settings, f32 dt, u64 camera);
    void ResetAutoExposure() { exposure_history_valid_ = false; exposure_ready_ = false; }

private:
    bool EnsureAutoExposure(u32 width, u32 height);
    void ReleaseAutoExposure();
    PostConstants FrameConstants(const PostProcessSettings& settings) const;
    bool EnsureDepthOfField(u32 width, u32 height);
    void ReleaseDepthOfField();
    bool EnsureBloomChain(u32 width, u32 height);
    u32 frame_ = 0; // counts the Draws: it moves the film grain

    gfx::rhi::IDevice& device_;
    gfx::rhi::ISwapChain& swap_chain_;
    gfx::rhi::PipelineHandle pipeline_;
    gfx::rhi::PipelineHandle bloom_down_;
    gfx::rhi::PipelineHandle bloom_up_;
    gfx::rhi::PipelineHandle hdr_composite_, depth_of_field_;
    gfx::rhi::SampledTextureHandle dof_composite_, dof_result_;
    u32 dof_width_ = 0, dof_height_ = 0;
    bool dof_ready_ = false;
    gfx::rhi::PipelineHandle exposure_pipeline_;
    std::vector<gfx::rhi::SampledTextureHandle> exposure_levels_;
    std::vector<Extent> exposure_extents_;
    gfx::rhi::SampledTextureHandle exposure_history_[2];
    u32 exposure_width_ = 0, exposure_height_ = 0, exposure_current_ = 0;
    u64 exposure_camera_ = 0;
    bool exposure_history_valid_ = false, exposure_ready_ = false;
    u32 scene_color_ = 0;
    u32 source_ = 0; // SetSource's; 0: scene_color_
    std::vector<gfx::rhi::SampledTextureHandle> bloom_levels_;
    std::vector<Extent> bloom_extents_;
    u32 bloom_width_ = 0, bloom_height_ = 0; // the scene size the chain was made for
    AmbientOcclusion ao_;
    Vec3 fog_color_{0, 0, 0};
    Vec3 sun_view_{0, 0, 1}, sun_color_{0, 0, 0};
    bool sun_set_ = false;
    Vec3 fog_up_view_{0, 1, 0};
    f32 fog_camera_height_ = 0.0f;
    bool bloom_ready_ = false;               // RenderBloom drew since the last Draw
    bool bloom_warned_ = false;              // the full-table warning, once
};

} // namespace aether::player
