#pragma once

#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"
#include "aether/math/math.h"

#include <array>
#include <string>
#include <vector>

// Virtual shadow maps for the sun, layered over the cascades (render roadmap, phase 4). The cascades (shadow_pass.h) stay as they are and
// remain the fallback; this adds a clipmap of small pages with a much finer texel near the camera, where the cascades' texel is the coarsest
// compared with a pixel. Only the pages the frame looks at exist.
//
// The geometry, shared by the CPU and the HLSL below (the two must agree):
//  - The light has a fixed basis from the sun's direction alone (VsmLightBasis): u = p . right, v = p . up, depth = p . direction. Pages are
//    anchored in this space at the world origin, not at the camera, so a page keeps its meaning as the camera moves and its contents stay valid.
//  - Level k has texels of kVsmBaseTexel * 2^k metres and pages of kVsmPageTexels texels. A pixel picks the level whose texel is about its own
//    footprint on the surface (VsmLevelForFootprint), so the near ground gets the fine levels and the distance the coarse ones.
//  - A page is (level, x, y) with x = floor(u / page size) and y likewise. The pages around the camera form a window of kVsmWindowPages x
//    kVsmWindowPages per level, addressed toroidally (x & 31): VsmWindowOrigin and VsmRecoverPage turn a wrapped coordinate back into the page.

namespace aether::player {

constexpr u32 kVsmLevels = 6;
constexpr u32 kVsmPageTexels = 128;
constexpr u32 kVsmWindowPages = 32;
constexpr f32 kVsmBaseTexel = 0.002f; // metres per texel at level 0
constexpr u32 kVsmFeedbackScale = 8;  // the feedback image is the screen over this, each side
constexpr f32 kVsmDepthRange = 4096.0f; // a page stores depth along the light as z / range + 0.5, so z may lie within +-2048 m of the origin

f32 VsmTexelSize(u32 level);
f32 VsmPageSize(u32 level); // metres per page side
// The level whose texel is about `footprint` metres (a pixel's size on the surface): the smallest texel not smaller than it.
u32 VsmLevelForFootprint(f32 footprint);

struct VsmLight {
    Vec3 right{1, 0, 0}, up{0, 1, 0}, direction{0, 0, -1}; // direction: the way the light travels
};
VsmLight VsmLightBasis(const Vec3& light_direction);
// u, v and the depth along the light.
void VsmLightSpace(const VsmLight& light, const Vec3& p, f32& u, f32& v, f32& depth);

struct VsmPage {
    u32 level = 0;
    i32 x = 0, y = 0;
    bool operator==(const VsmPage&) const = default;
};
VsmPage VsmPageOf(const VsmLight& light, u32 level, const Vec3& p);

// The matrix that draws world space into a page: x and y across the page (clip -1 to 1 over u and v, v up), depth z / kVsmDepthRange + 0.5. The
// page is drawn into a viewport of kVsmPageTexels square.
Mat4 VsmPageMatrix(const VsmLight& light, const VsmPage& page);
// Whether a world-space sphere's circle in the light's space reaches the page (whatever its depth along the light).
bool VsmSphereTouchesPage(const VsmLight& light, const VsmPage& page, const Vec3& center, f32 radius);

// The window's first page along one axis for a camera in page `camera_page`, and a wrapped (x & 31) coordinate turned back into a page of the window.
i32 VsmWindowOrigin(i32 camera_page);
i32 VsmRecoverPage(i32 window_origin, u32 wrapped);
// The window origins (x, y) of every level for a camera at `camera_position`.
std::array<std::array<i32, 2>, kVsmLevels> VsmWindowOrigins(const VsmLight& light, const Vec3& camera_position);

// What a frame asked for: a page and how many feedback texels looked at it.
struct VsmRequest {
    VsmPage page;
    u32 hits = 0;
};

// The HLSL: VSM_* constants and VsmTexel, VsmLevelForFootprint, as above.
const char* VsmShaderLibrary();

// The feedback pass: a small full-screen pass reads the scene depth copy, puts each sampled pixel's world position in the light's space, and
// writes the page it looks at (level + 1, x & 31, y & 31, 1; zero where nothing was drawn) into an RGBA16F image a kVsmFeedbackScale-th the
// screen's size. The image is copied to a readback buffer in the same command list, and the CPU reads it when that frame's fence has passed.
struct VsmFeedbackParams {
    Mat4 inverse_view_projection;     // of the camera, without jitter
    Vec3 camera_position;
    f32 footprint_scale = 0.0f;       // a pixel's footprint on a surface is scale * distance + constant:
    f32 footprint_constant = 0.0f;    //   a perspective camera's tan(fov / 2) * 2 / height, 0; an orthographic one's 0, size / height
    VsmLight light;
    u32 depth_texture = 0;            // bindless index of the scene depth copy
    u32 screen_width = 0, screen_height = 0;
    u32 frame = 0;                    // moves the sampled pixel inside each block, so every pixel is looked at in time
};

class VsmFeedback {
public:
    VsmFeedback(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& swap_chain);
    ~VsmFeedback();
    VsmFeedback(const VsmFeedback&) = delete;
    VsmFeedback& operator=(const VsmFeedback&) = delete;
    // The device has render textures and readback, and the pipeline built.
    bool Valid() const { return pipeline_.IsValid(); }

    // Records the pass and the copy outside any render pass, after the scene depth copy. `slot` picks one of kSlots readback buffers: the one
    // Collect reads must not be the one a submission in flight writes, which holds for a slot reused only after its frame's fence.
    bool Record(gfx::rhi::ICommandList& commands, const VsmFeedbackParams& params, u32 slot);
    // The pages the frame last recorded in `slot` looked at, most looked at first; empty when nothing was recorded there. Only after the fence of
    // the submission that carried it.
    std::vector<VsmRequest> Collect(u32 slot);
    static constexpr u32 kSlots = 8;
    void Release();

private:
    bool Ensure(u32 width, u32 height);

    gfx::rhi::IDevice& device_;
    gfx::rhi::PipelineHandle pipeline_;
    gfx::rhi::SampledTextureHandle image_;
    u32 width_ = 0, height_ = 0;
    gfx::rhi::BufferHandle readback_[kSlots];
    struct Pending {
        bool valid = false;
        u32 width = 0, height = 0;
        std::array<std::array<i32, 2>, kVsmLevels> window_origins{};
    };
    Pending pending_[kSlots];
    std::vector<u8> scratch_; // the rows Collect reads, reused
};

// Decodes one feedback image (`rows` as copied: RGBA16F texels, `pitch` bytes apart) into requests.
std::vector<VsmRequest> VsmDecodeFeedback(const u8* rows, u32 width, u32 height, u32 pitch_bytes,
                                          const std::array<std::array<i32, 2>, kVsmLevels>& window_origins);

} // namespace aether::player
