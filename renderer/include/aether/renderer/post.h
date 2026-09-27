#pragma once

#include "aether/renderer/render_scene.h"

#include <vector>

namespace aether {

// Post-processing math (Phase 14 step 4): the CPU references the GPU passes
// are checked against, and the per-frame values the renderer computes on the
// CPU (exposure adaptation, the bloom chain, TAA jitter).

// --- Auto exposure -----------------------------------------------------------------
// Luminance is measured in a histogram of log2 luminance between min_ev and
// max_ev; the darkest `low_percent` and brightest `1 - high_percent` of
// pixels are ignored (black sky, the sun), and the rest averaged.
struct ExposureSettings {
    f32 min_log2 = -10.0f, max_log2 = 10.0f; // histogram range (log2 of luminance)
    u32 bins = 256;
    f32 low_percent = 0.5f, high_percent = 0.95f;
    f32 speed_up = 3.0f, speed_down = 1.0f; // adaptation rates (1/s): brightening, darkening
    f32 min_ev100 = -4.0f, max_ev100 = 16.0f;
};

f32 Luminance(const Vec3& linear_rgb); // Rec. 709
std::vector<u32> LuminanceHistogram(const std::vector<f32>& luminances, const ExposureSettings& settings);
// The scene's average luminance from a histogram, after the percentile cut.
f32 AverageLuminance(const std::vector<u32>& histogram, const ExposureSettings& settings);
// Saturation-based EV100 for an average luminance (ISO 100, K = 12.5).
f32 EV100FromLuminance(f32 average_luminance);
// The multiplier for scene values: 1 / (1.2 * 2^EV100), then compensation (in EV).
f32 ExposureFromEV100(f32 ev100, f32 compensation = 0.0f);
// Moves `current` toward `target` over `dt`: fast when it gets brighter, slower darker.
f32 AdaptEV100(f32 current, f32 target, f32 dt, const ExposureSettings& settings);

// --- Tone mapping and grading -------------------------------------------------------
Vec3 TonemapACES(const Vec3& linear_rgb); // Stephen Hill's fit of the ACES RRT + ODT
Vec3 TonemapAgX(const Vec3& linear_rgb);  // Troy Sobotka's AgX, polynomial fit of the base curve
Vec3 Tonemap(const Vec3& linear_rgb, Tonemapper mapper);
f32 LinearToSrgb(f32 linear);
f32 SrgbToLinear(f32 encoded);
// Saturation around Rec. 709 luminance (1 = unchanged, 0 = grey).
Vec3 AdjustSaturation(const Vec3& rgb, f32 saturation);
// The vignette's darkening at a screen point (uv in 0..1): 1 at the center.
f32 VignetteFactor(f32 u, f32 v, f32 strength);

// --- Bloom ---------------------------------------------------------------------------
struct Extent {
    u32 width = 0, height = 0;
    bool operator==(const Extent& o) const { return width == o.width && height == o.height; }
};
// The downsample chain: half size each level (rounded up) from half the
// source, until a side would drop below `min_size` or `max_levels` is reached.
std::vector<Extent> BloomChain(u32 width, u32 height, u32 max_levels = 6, u32 min_size = 8);

// --- TAA -------------------------------------------------------------------------------
f32 Halton(u32 index, u32 base); // index from 1
// Sub-pixel offsets in [-0.5, 0.5], Halton(2, 3), repeating every `count` frames.
Vec3 TaaJitter(u64 frame, u32 count = 8); // z unused
// Shifts a projection by a sub-pixel offset (in pixels) for a viewport of the given size.
Mat4 JitterProjection(const Mat4& projection, f32 offset_x_pixels, f32 offset_y_pixels, u32 width, u32 height);

} // namespace aether
