#pragma once

#include "aether/gfx/rhi/device.h"
#include "aether/renderer/render_scene.h"

// The player's post pass (render roadmap step 1). Where the device has an HDR scene colour target (ISwapChain::SceneColorTexture: D3D12
// today) the scene draws into it in linear light, unclamped, and this pass turns that into the display image in the back buffer: exposure,
// the tone mapper (ACES or AgX, the CPU references in renderer/post.h), saturation, the vignette and the sRGB encoding, in one full-screen
// triangle that reads each pixel with a texel load. Where there is no such target (Vulkan for now) the scene shader tone maps itself, as it
// always did, and this pass is not made.

namespace aether::player {

// The push constants: 32 bytes.
struct PostConstants {
    u32 scene_color = 0;  // bindless index of the HDR target
    f32 exposure = 1.0f;  // linear multiplier applied before the tone mapper
    u32 tonemapper = 0;   // Tonemapper: 0 ACES, 1 AgX, 2 None (clamp)
    f32 saturation = 1.0f;
    f32 vignette = 0.0f;  // 0..1
    f32 padding[3] = {0, 0, 0};
};
static_assert(sizeof(PostConstants) == 32);

// The volume settings as push constants: exposure is 2^exposure_compensation (scene light values are not physical units, so there is no
// EV100 metering yet), the rest are copied with their ranges clamped.
PostConstants PackPostConstants(const PostProcessSettings& settings, u32 scene_color_index);

// What the pass does to one linear HDR value, on the CPU, as the shader does it: the reference the GPU test compares against. `u`, `v` are
// the pixel's position in 0..1 (for the vignette). Returns display-encoded (sRGB) values in 0..1.
Vec3 PostReference(const Vec3& linear, const PostConstants& constants, f32 u, f32 v);

// The HLSL (VSMain, PSMain), for the pipeline and for tests.
const char* PostShaderSource();

class PostPass {
public:
    // Makes the HDR target on `swap_chain` and the pipeline. Not Valid() where the device has no scene colour target or the pipeline fails.
    PostPass(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& swap_chain);
    ~PostPass();
    PostPass(const PostPass&) = delete;
    PostPass& operator=(const PostPass&) = delete;
    bool Valid() const { return pipeline_.IsValid() && scene_color_ != 0; }
    u32 SceneColorIndex() const { return scene_color_; }
    // Records the full-screen draw inside the current back-buffer render pass, after the scene colour pass has ended.
    void Draw(gfx::rhi::ICommandList& commands, const PostProcessSettings& settings);

private:
    gfx::rhi::IDevice& device_;
    gfx::rhi::PipelineHandle pipeline_;
    u32 scene_color_ = 0;
};

} // namespace aether::player
