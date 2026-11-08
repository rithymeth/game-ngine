#pragma once

#include "aether/gfx/rhi/device.h"
#include "aether/renderer/decal.h"

#include <string>
#include <vector>

// The player's decal pass (Phase 39.4). Each decal (a Decal component's box, renderer/decal.h) draws as one screen-space rectangle after the opaque
// scene: for every pixel in it the shader reads the scene's depth (the swap chain's SceneDepthTexture, copied just before), rebuilds that pixel's
// surface point in the decal's unit box, and, when it is inside, paints the decal's texture there with alpha blending. A surface turning edge-on
// to the projection fades the decal out (the normal comes from the screen-space derivatives of the box coordinates). Decals are unlit: the colour is
// the texture times the tint, blended over the surface's already-lit, tone-mapped colour. Without a scene depth texture on the device
// (the backend, depth format or a full bindless table) the pass is not valid and decals are not drawn.

namespace aether::player {

// The "no texture" bindless index (what the scene shader uses too): the decal draws as a flat tint.
constexpr u32 kDecalNoTexture = 255; // past the 128-slot table

// One decal ready to draw: the geometry (mat::BuildDecalDraws), its texture's bindless index (kDecalNoTexture: flat tint) and its look.
struct DecalPassItem {
    mat::DecalDraw draw;
    u32 texture = kDecalNoTexture;
    f32 color[4] = {1, 1, 1, 1}; // tint (display space, see DecalTint) with alpha = the decal's opacity
    f32 fade_start = 0.5f;       // see Decal::angle_fade_start / angle_fade_end
    f32 fade_end = 0.1f;
};

// The push constants, exactly 128 bytes: the matrix, the ndc rectangle, the tint, the angle fade and the box's size (the shader measures the
// surface's tilt in metres, so a stretched box doesn't skew it), then the texture and depth indices.
struct DecalConstants {
    Mat4 clip_to_unit;
    f32 rect[4];  // ndc min x, min y, max x, max y
    f32 color[4];
    f32 fade[4];  // start, end, size x, size y
    f32 extra[4]; // size z, texture index, scene depth index, 0
};
static_assert(sizeof(DecalConstants) == 128);

DecalConstants PackDecalConstants(const DecalPassItem& item, u32 scene_depth_index);

// A linear tint as the display-space multiplier the shader uses (the pass blends in the display's encoding, like the particle pass): the sRGB
// transfer curve applied to each channel.
f32 DecalTint(f32 linear);

// The HLSL (VSMain, PSMain), for the pipeline and for tests.
const char* DecalShaderSource();

class DecalPass {
public:
    // `hdr_target`: draws into the HDR scene colour target in linear light (the texture decoded from sRGB, the tint linear: pass the
    // Decal's colour as it is, not through DecalTint); otherwise in the back buffer's display encoding, as before.
    DecalPass(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& swap_chain, bool hdr_target = false);
    ~DecalPass();
    DecalPass(const DecalPass&) = delete;
    DecalPass& operator=(const DecalPass&) = delete;
    bool Valid() const { return pipeline_.IsValid() && scene_depth_ != 0; }
    bool HdrTarget() const { return hdr_target_; }
    // Records the draws inside the current render pass, after the opaque surfaces. Copies the scene depth first (ICommandList::CopySceneDepth),
    // so the caller's pipeline and bindings are gone afterwards. Returns how many were drawn (0 when invalid, empty, or the copy failed).
    u32 Draw(gfx::rhi::ICommandList& commands, const std::vector<DecalPassItem>& items);

private:
    gfx::rhi::IDevice& device_;
    gfx::rhi::PipelineHandle pipeline_;
    u32 scene_depth_ = 0;
    bool hdr_target_ = false;
};

} // namespace aether::player
