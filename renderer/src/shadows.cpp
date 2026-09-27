#include "aether/renderer/shadows.h"

#include <algorithm>
#include <cmath>

namespace aether {

namespace {

// A rotation-only view matrix looking along `forward`.
Mat4 LightRotation(const Vec3& forward, Vec3& up) {
    up = std::fabs(forward.y) > 0.99f ? Vec3(0, 0, 1) : Vec3(0, 1, 0);
    return Mat4::LookAtRH(Vec3(0, 0, 0), forward, up);
}

Vec3 Apply(const Mat4& m, const Vec3& p) {
    const Vec4 r = m * Vec4(p.x, p.y, p.z, 1.0f);
    return Vec3(r.x, r.y, r.z);
}

// The inverse of a rotation-only matrix: its transpose.
Vec3 ApplyTransposed(const Mat4& m, const Vec3& p) {
    return Vec3(m.cols[0].x * p.x + m.cols[0].y * p.y + m.cols[0].z * p.z, m.cols[1].x * p.x + m.cols[1].y * p.y + m.cols[1].z * p.z,
                m.cols[2].x * p.x + m.cols[2].y * p.y + m.cols[2].z * p.z);
}

} // namespace

std::vector<f32> CascadeSplits(f32 near_plane, f32 far_plane, u32 count, f32 lambda) {
    std::vector<f32> splits(count + 1);
    const f32 n = std::max(near_plane, 1e-4f), f = std::max(far_plane, n);
    lambda = std::clamp(lambda, 0.0f, 1.0f);
    for (u32 i = 0; i <= count; ++i) {
        const f32 t = count == 0 ? 0.0f : static_cast<f32>(i) / static_cast<f32>(count);
        const f32 uniform = n + (f - n) * t;
        const f32 logarithmic = n * std::pow(f / n, t);
        splits[i] = lambda * logarithmic + (1.0f - lambda) * uniform;
    }
    splits.front() = n;
    splits.back() = f;
    return splits;
}

void FrustumSliceCorners(const View& view, f32 near_depth, f32 far_depth, Vec3 corners[8]) {
    const Vec3 right = view.Right(), up = view.Up(), forward = view.forward;
    int i = 0;
    for (const f32 d : {near_depth, far_depth}) {
        const f32 half_h = view.perspective ? d * std::tan(Radians(view.fov_degrees) * 0.5f) : view.ortho_height * 0.5f;
        const f32 half_w = half_h * view.aspect;
        const Vec3 center = view.position + forward * d;
        for (const f32 sy : {-1.0f, 1.0f}) {
            for (const f32 sx : {-1.0f, 1.0f}) corners[i++] = center + right * (sx * half_w) + up * (sy * half_h);
        }
    }
}

std::vector<ShadowCascade> ComputeCascades(const View& view, const Vec3& light_direction, const CascadeSettings& settings) {
    std::vector<ShadowCascade> out;
    if (settings.count == 0 || settings.resolution < 8) return out;
    const f32 far_d = std::min(view.far_plane, settings.shadow_distance);
    const std::vector<f32> splits = CascadeSplits(view.near_plane, far_d, settings.count, settings.lambda);
    const Vec3 dir = light_direction.LengthSq() > 0.0f ? light_direction.Normalized() : Vec3(0, -1, 0);
    Vec3 up;
    const Mat4 rotation = LightRotation(dir, up);
    const f32 res = static_cast<f32>(settings.resolution);

    for (u32 i = 0; i < settings.count; ++i) {
        ShadowCascade c;
        c.split_near = splits[i];
        c.split_far = splits[i + 1];
        Vec3 corners[8];
        FrustumSliceCorners(view, c.split_near, c.split_far, corners);
        // A bounding sphere: its size depends only on the slice's shape, so it
        // doesn't change as the camera turns. Rounded up to 1/16 m so float
        // noise can't change it between frames either.
        Vec3 center(0, 0, 0);
        for (const Vec3& p : corners) center = center + p * 0.125f;
        f32 radius = 0.0f;
        for (const Vec3& p : corners) radius = std::max(radius, (p - center).Length());
        radius = std::ceil(radius * 16.0f) / 16.0f;
        // Two texels of margin on each side absorb the snap below.
        const f32 half = radius * res / (res - 4.0f);
        c.texel_size = 2.0f * half / res;
        // Snap the center to whole texels in light space.
        Vec3 ls = Apply(rotation, center);
        ls.x = std::floor(ls.x / c.texel_size) * c.texel_size;
        ls.y = std::floor(ls.y / c.texel_size) * c.texel_size;
        center = ApplyTransposed(rotation, ls);
        c.center = center;
        c.radius = half;
        const f32 back = half + settings.depth_padding;
        c.view = Mat4::LookAtRH(center - dir * back, center, up);
        c.projection = OrthographicRH(2.0f * half, 2.0f * half, 0.0f, back + half);
        c.view_projection = c.projection * c.view;
        out.push_back(c);
    }
    return out;
}

} // namespace aether
