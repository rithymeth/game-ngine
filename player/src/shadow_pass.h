#pragma once

#include "aether/gfx/rhi/device.h"
#include "aether/renderer/shadows.h"
#include "aether/renderer/view.h"

#include <span>
#include <vector>

// Cascaded sun shadows (render roadmap step 2). Up to four cascades (renderer/shadows.h: practical splits, bounding-sphere fit, texel
// snapping) are drawn into one D32 atlas, one bindless slot, laid out two by two; the scene shader picks the first cascade a point
// projects into and filters it with a 3x3 bilinear PCF of texel loads. Where the device has no depth textures (Vulkan for now) the pass
// is not Valid and the scene draws unshadowed, as before.

namespace aether::player {

// The caster pipelines' push constants: 80 bytes.
struct ShadowCasterConstants {
    Mat4 light_mvp;
    u32 base_color_texture = 255; // bindless index, 255 = none (alpha-tested casters only)
    f32 alpha = 1.0f;             // the material's base colour alpha
    f32 alpha_cutoff = 0.5f;
    f32 padding = 0.0f;
};
static_assert(sizeof(ShadowCasterConstants) == 80);

// One shadow-casting draw: an opaque or alpha-tested (MASK) primitive. Blended surfaces don't cast.
struct ShadowCaster {
    gfx::rhi::BufferHandle vertices; // SceneVertex layout (PositionNormalUvTangent, 48 bytes)
    gfx::rhi::BufferHandle indices;  // u32
    u32 index_count = 0;
    Mat4 model;
    Vec3 bounds_center{0, 0, 0}; // world-space bounding sphere; radius < 0: always drawn (e.g. skinned meshes)
    f32 bounds_radius = -1.0f;
    bool alpha_tested = false;
    u32 base_color_texture = 255;
    f32 alpha = 1.0f;
    f32 alpha_cutoff = 0.5f;
};

// What the scene shader reads (the shadow part of FrameData, player/src/frame_lights.h).
struct ShadowFrameData {
    f32 view_projection[4][16]{}; // per cascade, column-major (Mat4's own layout)
    f32 cascade[4][4]{};          // x, y: the cascade's origin in the atlas (texels); z: world units per texel; w: 1 / depth range
    u32 info[4]{255u, 0u, 0u, 0u}; // x: the atlas's bindless index (255: no shadows); y: cascades; z: texels per cascade side
};

// The atlas's layout for `count` cascades of `resolution` texels: one row of up to two, then a second row.
void ShadowAtlasSize(u32 resolution, u32 count, u32& width, u32& height);
void ShadowCascadeOrigin(u32 resolution, u32 cascade, u32& x, u32& y);

// The cascades for a view and a sun travelling along `light_direction`, as ShadowPass::Render fits them, and the frame data they make.
std::vector<ShadowCascade> FitShadowCascades(const View& view, const Vec3& light_direction, u32 resolution, u32 count, f32 distance);
ShadowFrameData PackShadowFrameData(const std::vector<ShadowCascade>& cascades, u32 resolution, u32 atlas_index);

// Whether a caster's bounding sphere can touch a cascade's map (a caster nearer the light than the map's near plane still can: the
// pipelines clamp depth rather than clip).
bool CasterTouchesCascade(const ShadowCaster& caster, const ShadowCascade& cascade);

// The HLSL (VSMain; PSMain for alpha-tested casters).
const char* ShadowCasterShaderSource();

class ShadowPass {
public:
    ShadowPass(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& swap_chain);
    ~ShadowPass();
    ShadowPass(const ShadowPass&) = delete;
    ShadowPass& operator=(const ShadowPass&) = delete;
    // The device draws depth textures and both caster pipelines built.
    bool Valid() const { return opaque_.IsValid() && masked_.IsValid(); }

    // Texels per cascade side and cascade count (a quality preset's shadow_resolution and shadow_cascades); 0 for either turns shadows
    // off and frees the atlas. The atlas is (re)made at the next Render. Distance: how far from the camera shadows reach (metres).
    void Configure(u32 resolution, u32 cascades, f32 distance = 100.0f);
    u32 Resolution() const { return resolution_; }
    u32 CascadeCount() const { return cascades_; }

    // Draws every caster into each cascade it touches, outside any render pass, before the scene pass that reads the atlas. Returns the
    // frame data for the scene shader: shadows off (info.x 255) when not Valid, unconfigured, or the atlas can't be made.
    ShadowFrameData Render(gfx::rhi::ICommandList& commands, const View& view, const Vec3& light_direction, std::span<const ShadowCaster> casters);
    // Casters drawn by the last Render, over all cascades (for tests and the profiler).
    u32 LastDrawCount() const { return last_draws_; }
    gfx::rhi::SampledTextureHandle Atlas() const { return atlas_; }
    void ReleaseAtlas();

private:
    gfx::rhi::IDevice& device_;
    gfx::rhi::PipelineHandle opaque_;
    gfx::rhi::PipelineHandle masked_;
    gfx::rhi::SampledTextureHandle atlas_;
    u32 atlas_resolution_ = 0, atlas_cascades_ = 0; // what the atlas was made for
    u32 resolution_ = 2048, cascades_ = 4;
    f32 distance_ = 100.0f;
    u32 last_draws_ = 0;
    bool warned_ = false;
};

} // namespace aether::player
