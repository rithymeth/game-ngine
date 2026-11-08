#pragma once

#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"
#include "aether/math/math.h"
#include "aether/renderer/post.h"

// Temporal anti-aliasing (render roadmap step 6). Each frame the scene's projection is moved by a sub-pixel offset (the Halton 2, 3
// sequence, 8 frames long), so successive frames sample each pixel at different points. After the scene colour pass, a resolve pass
// blends the frame into a history of the frames before it:
//   - the history is found where this pixel was last frame, from the scene depth and the camera's movement (the nearest depth of the
//     3x3 neighbourhood, so edges follow the surface in front);
//   - it is read with a 5-tap Catmull-Rom filter, which keeps it sharp under motion;
//   - it is clipped to the colour box of this frame's 3x3 neighbourhood (YCoCg, mean +- 1.25 sigma), which rejects history that no
//     longer belongs (disocclusion, moving objects, lighting changes);
//   - and it is blended at kTaaBlend with this frame, both weighted by 1 / (1 + luma) so one bright sample can't flicker.
// The result is a render texture that becomes next frame's history and the image bloom and the tone mapper read (PostPass::SetSource).
// Motion is the camera's only: objects that move on their own are held to their neighbourhood by the clip, which limits their ghosting
// but does not remove it.

namespace aether::player {

constexpr u32 kTaaSequenceLength = 8;
constexpr f32 kTaaBlend = 0.1f; // the weight of the newest frame

// The push constants: 96 bytes.
struct TaaConstants {
    f32 reprojection[16]{}; // this frame's NDC (x, y, depth) to last frame's clip space, column-major
    u32 current = 0;        // bindless index of the scene colour
    u32 history = 0;        // of last frame's resolve
    u32 depth = 0;          // of the scene depth copy
    f32 blend = 1.0f;       // the newest frame's weight; 1: no history (the first frame, a cut, a resize)
    f32 texel[2]{};         // 1 / the target's size
    f32 padding[2]{};
};
static_assert(sizeof(TaaConstants) == 96);

// The jitter itself is renderer/post.h's: TaaJitter (Halton 2, 3) and JitterProjection.
// What the resolve shader does to find a pixel's history: last frame's view-projection times the inverse of this frame's (both
// without jitter). Identity when the camera hasn't moved. False when this frame's can't be inverted.
bool TaaReprojection(const Mat4& previous_view_projection, const Mat4& view_projection, Mat4& out);

// The resolve shader (VSMain, PSMain), for the pipeline and for tests.
const char* TaaShaderSource();

class TaaPass {
public:
    // Makes the pipeline, and the scene depth copy it reads. Not Valid() where the device has no render textures, no scene depth copy or
    // no scene colour target.
    TaaPass(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& swap_chain);
    ~TaaPass();
    TaaPass(const TaaPass&) = delete;
    TaaPass& operator=(const TaaPass&) = delete;
    bool Valid() const { return pipeline_.IsValid() && scene_color_ != 0 && scene_depth_ != 0; }

    // This frame's offset in pixels (call once a frame, before drawing; it advances the sequence).
    void NextJitter(f32& x, f32& y);
    // Records the resolve outside any render pass, after the scene colour pass has ended (with the depth copied by CopySceneDepth).
    // `view_projection` is this frame's without jitter; `camera` names the camera (a different one starts the history over). Returns
    // the bindless index of the resolved image, or 0 with nothing recorded (no room in the bindless table for the history, a zero size).
    u32 Resolve(gfx::rhi::ICommandList& commands, const Mat4& view_projection, u64 camera);
    // Starts the history over on the next Resolve.
    void Reset() { history_valid_ = false; }
    // Frees the history textures (they come back on the next Resolve).
    void Release();

private:
    bool EnsureHistory(u32 width, u32 height);

    gfx::rhi::IDevice& device_;
    gfx::rhi::ISwapChain& swap_chain_;
    gfx::rhi::PipelineHandle pipeline_;
    u32 scene_color_ = 0;
    u32 scene_depth_ = 0;
    gfx::rhi::SampledTextureHandle history_[2];
    u32 width_ = 0, height_ = 0;
    u32 current_ = 0; // which of history_ the next Resolve writes
    u32 frame_ = 0;
    bool history_valid_ = false;
    Mat4 previous_view_projection_;
    u64 previous_camera_ = 0;
};

} // namespace aether::player
