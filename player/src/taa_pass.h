#pragma once

#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"
#include "aether/math/math.h"
#include "aether/renderer/post.h"

#include <span>

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
// Objects that move on their own (a model transform different from last frame's, or a skinned mesh whose pose changed) are drawn
// first into a motion texture: each covered pixel gets the screen-space offset to where its surface was last frame, and the resolve
// follows that instead of the camera's reprojection. The pass tests against the scene depth copy, so a mover behind something writes
// nothing. A skinned mover's last-frame vertex positions come from a read buffer (TaaMover::previous_positions), indexed by vertex,
// where the device has frame read buffers; elsewhere it moves with its transform alone. AETHER_RENDER_TAA_MOTION=0 turns the motion
// pass off, for comparison.

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
    u32 motion = 0;         // bindless index of the motion texture; 0: nothing moved on its own this frame
    f32 padding = 0.0f;
};
static_assert(sizeof(TaaConstants) == 96);

// The motion pass's push constants: 144 bytes.
struct TaaMotionConstants {
    f32 mvp[16]{};          // this frame's, without jitter
    f32 previous_mvp[16]{}; // last frame's view-projection times last frame's model transform
    f32 jitter[2]{};        // this frame's offset in NDC (what JitterProjection adds per unit of w)
    u32 depth = 0;          // bindless index of the scene depth copy
    u32 previous_positions = 0xffffffffu; // byte offset of last frame's positions in the read buffer; none: this frame's
};
static_assert(sizeof(TaaMotionConstants) == 144);

constexpr u32 kTaaNoPreviousPositions = 0xffffffffu;

// An opaque draw that moved since last frame: its model transform, or (skinned) its vertices.
struct TaaMover {
    gfx::rhi::BufferHandle vertices;
    gfx::rhi::BufferHandle indices; // 32-bit
    u32 index_count = 0;
    u32 vertex_stride = 0;          // position first, as the scene's vertices (PositionNormalUvTangent)
    Mat4 model;
    Mat4 previous_model;
    // Where last frame's positions are in Resolve's `previous_positions` buffer: a byte offset to three floats per vertex, in vertex
    // order. kTaaNoPreviousPositions: the vertices didn't change, only the transform.
    u32 previous_positions = kTaaNoPreviousPositions;
};

// The jitter itself is renderer/post.h's: TaaJitter (Halton 2, 3) and JitterProjection.
// What the resolve shader does to find a pixel's history: last frame's view-projection times the inverse of this frame's (both
// without jitter). Identity when the camera hasn't moved. False when this frame's can't be inverted.
bool TaaReprojection(const Mat4& previous_view_projection, const Mat4& view_projection, Mat4& out);

// The resolve shader (VSMain, PSMain), for the pipeline and for tests.
const char* TaaShaderSource();
// The motion shader (VSMain, PSMain): a mover's offset to last frame, in UV, with alpha 1 where it wrote.
const char* TaaMotionShaderSource();

// Whether a model transform moved enough since last frame to need the motion pass (any element by more than a millionth of the
// largest).
bool TaaTransformMoved(const Mat4& previous, const Mat4& current);

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
    // `movers` are drawn into the motion texture first (when the history is kept), so the resolve follows them. `previous_positions`
    // is a buffer made by IDevice::CreateFrameConstantBuffer holding the skinned movers' last-frame positions (TaaMover::
    // previous_positions), written for this frame and not rewritten until it completes; invalid when no mover has any.
    u32 Resolve(gfx::rhi::ICommandList& commands, const Mat4& view_projection, u64 camera, std::span<const TaaMover> movers = {},
                gfx::rhi::BufferHandle previous_positions = {});
    // The bindless index of the motion texture the last Resolve drew, or 0 when it drew none (for tests).
    u32 LastMotion() const { return last_motion_; }
    // Whether movers' last-frame positions are read (the device has frame read buffers); without, they move with their transforms.
    bool ReadsPreviousPositions() const { return motion_reads_positions_; }
    // Starts the history over on the next Resolve.
    void Reset() { history_valid_ = false; }
    // Frees the history textures (they come back on the next Resolve).
    void Release();

private:
    bool EnsureHistory(u32 width, u32 height);
    u32 DrawMotion(gfx::rhi::ICommandList& commands, const Mat4& view_projection, std::span<const TaaMover> movers,
                   gfx::rhi::BufferHandle previous_positions);

    gfx::rhi::IDevice& device_;
    gfx::rhi::ISwapChain& swap_chain_;
    gfx::rhi::PipelineHandle pipeline_;
    gfx::rhi::PipelineHandle motion_pipeline_;
    bool motion_reads_positions_ = false;
    gfx::rhi::BufferHandle no_positions_; // bound when a frame has no skinned movers (the pipeline's read buffer must be bound)
    gfx::rhi::SampledTextureHandle motion_;
    u32 last_motion_ = 0;
    f32 jitter_ndc_[2]{};
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
