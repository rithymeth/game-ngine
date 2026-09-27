#pragma once

#include "aether/renderer/view.h"

#include <vector>

namespace aether {

// Cascaded shadow maps for the sun (Phase 14 step 2).
//
// The view's depth range (up to `shadow_distance`) is split into cascades
// with the practical scheme: a blend (`lambda`) of logarithmic splits, which
// give near cascades the detail, and uniform ones. Each cascade covers its
// slice of the view frustum with a bounding sphere, so its size doesn't
// change as the camera turns, and its light-space origin is snapped to whole
// shadow-map texels, so shadows don't shimmer as the camera moves.

struct CascadeSettings {
    u32 count = 4;
    f32 shadow_distance = 100.0f; // shadows end here (or at the far plane, if nearer)
    f32 lambda = 0.75f;           // 0 uniform .. 1 logarithmic
    u32 resolution = 2048;        // texels per side of each cascade's map
    f32 depth_padding = 50.0f;    // extra depth toward the light, for casters outside the view
};

struct ShadowCascade {
    f32 split_near = 0.0f, split_far = 0.0f; // view-space depths it covers
    Vec3 center{0, 0, 0};                     // of its bounding sphere (after snapping)
    f32 radius = 0.0f;
    f32 texel_size = 0.0f; // world units per shadow-map texel
    Mat4 view, projection, view_projection;
};

// count + 1 depths, from near to far.
std::vector<f32> CascadeSplits(f32 near_plane, f32 far_plane, u32 count, f32 lambda);

// `light_direction` is the way the light travels (RenderDirectionalLight::direction).
std::vector<ShadowCascade> ComputeCascades(const View& view, const Vec3& light_direction, const CascadeSettings& settings = {});

// The eight world-space corners of the view frustum between two view-space depths.
void FrustumSliceCorners(const View& view, f32 near_depth, f32 far_depth, Vec3 corners[8]);

} // namespace aether
