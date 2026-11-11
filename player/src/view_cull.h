#pragma once

#include "aether/core/base.h"
#include "aether/math/math.h"

#include <cmath>

namespace aether::player {

// The six planes of a view-projection's clip volume (-w <= x, y <= w, 0 <= z <= w), each as (normal, d) with a normal pointing in: a point is inside a
// plane when dot(normal, p) + d >= 0. Not normalised; `SphereVisible` divides by the length. Plane extraction is the Gribb-Hartmann one, from the matrix's rows.
struct ViewFrustum {
    Vec4 planes[6];
    Vec4 row3; // the clip w of a point: dot(row3, (p, 1))
    f32 pixels_per_unit_at_one = 0.0f; // projected size of one world unit one unit away, in pixels; 0 to skip the too-small test
};

namespace detail {
inline Vec4 MatrixRow(const Mat4& m, int r) {
    const f32 c0[4] = {m.cols[0].x, m.cols[0].y, m.cols[0].z, m.cols[0].w};
    const f32 c1[4] = {m.cols[1].x, m.cols[1].y, m.cols[1].z, m.cols[1].w};
    const f32 c2[4] = {m.cols[2].x, m.cols[2].y, m.cols[2].z, m.cols[2].w};
    const f32 c3[4] = {m.cols[3].x, m.cols[3].y, m.cols[3].z, m.cols[3].w};
    return Vec4(c0[r], c1[r], c2[r], c3[r]);
}
inline Vec4 Add(const Vec4& a, const Vec4& b) { return Vec4(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w); }
inline Vec4 Sub(const Vec4& a, const Vec4& b) { return Vec4(a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w); }
} // namespace detail

// `viewport_height` in pixels with `projection_y_scale` (the projection's [1][1], 1 / tan(fov / 2)), or 0 for either to skip the too-small test.
inline ViewFrustum MakeViewFrustum(const Mat4& view_projection, f32 projection_y_scale = 0.0f, f32 viewport_height = 0.0f) {
    using namespace detail;
    const Vec4 r0 = MatrixRow(view_projection, 0), r1 = MatrixRow(view_projection, 1), r2 = MatrixRow(view_projection, 2), r3 = MatrixRow(view_projection, 3);
    ViewFrustum f;
    f.planes[0] = Add(r3, r0); // left
    f.planes[1] = Sub(r3, r0); // right
    f.planes[2] = Add(r3, r1); // bottom
    f.planes[3] = Sub(r3, r1); // top
    f.planes[4] = r2;          // near (depth from 0)
    f.planes[5] = Sub(r3, r2); // far
    f.row3 = r3;
    const f32 y_scale = std::fabs(projection_y_scale); // a flipped projection has it negative
    f.pixels_per_unit_at_one = y_scale > 0.0f && viewport_height > 0.0f ? y_scale * viewport_height * 0.5f : 0.0f;
    return f;
}

// False when the sphere is wholly outside one plane, or so small on screen (its radius under `min_radius_pixels`) that it covers no pixel.
// Conservative: a sphere that might touch the view is visible.
inline bool SphereVisible(const ViewFrustum& frustum, const Vec3& centre, f32 radius, f32 min_radius_pixels = 0.0f) {
    for (const Vec4& p : frustum.planes) {
        const f32 length = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        if (length <= 0.0f) continue;
        if (p.x * centre.x + p.y * centre.y + p.z * centre.z + p.w < -radius * length) return false;
    }
    if (min_radius_pixels > 0.0f && frustum.pixels_per_unit_at_one > 0.0f) {
        const f32 w = frustum.row3.x * centre.x + frustum.row3.y * centre.y + frustum.row3.z * centre.z + frustum.row3.w;
        if (w > 0.0f && radius * frustum.pixels_per_unit_at_one / w < min_radius_pixels) return false;
    }
    return true;
}

} // namespace aether::player
