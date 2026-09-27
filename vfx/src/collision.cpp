#include "aether/vfx/collision.h"

#include <algorithm>
#include <cmath>

namespace aether::vfx {

Mat4 Inverse(const Mat4& in) {
    f32 m[16], inv[16];
    for (int c = 0; c < 4; ++c) {
        m[c * 4 + 0] = in.cols[c].x, m[c * 4 + 1] = in.cols[c].y, m[c * 4 + 2] = in.cols[c].z, m[c * 4 + 3] = in.cols[c].w;
    }
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const f32 det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    Mat4 out;
    if (std::abs(det) < 1e-20f) return out;
    const f32 k = 1.0f / det;
    for (int c = 0; c < 4; ++c) out.cols[c] = Vec4(inv[c * 4 + 0] * k, inv[c * 4 + 1] * k, inv[c * 4 + 2] * k, inv[c * 4 + 3] * k);
    return out;
}

DepthBufferCollider::DepthBufferCollider(const Mat4& view_projection, i32 width, i32 height, std::vector<f32> depth, f32 thickness, u32 steps)
    : view_projection_(view_projection), inverse_(Inverse(view_projection)), width_(std::max(width, 1)), height_(std::max(height, 1)),
      depth_(std::move(depth)), thickness_(thickness), steps_(std::max(steps, 1u)) {
    depth_.resize(static_cast<usize>(width_) * static_cast<usize>(height_), 1.0f);
}

bool DepthBufferCollider::SurfaceAt(i32 x, i32 y, Vec3& out) const {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return false;
    const f32 d = depth_[static_cast<usize>(y) * static_cast<usize>(width_) + static_cast<usize>(x)];
    if (d >= 1.0f) return false; // nothing drawn there
    const f32 nx = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(width_) * 2.0f - 1.0f;
    const f32 ny = 1.0f - (static_cast<f32>(y) + 0.5f) / static_cast<f32>(height_) * 2.0f;
    const Vec4 p = inverse_ * Vec4(nx, ny, d, 1.0f);
    if (std::abs(p.w) < 1e-20f) return false;
    out = Vec3(p.x / p.w, p.y / p.w, p.z / p.w);
    return true;
}

bool DepthBufferCollider::Raycast(const Vec3& from, const Vec3& to, Vec3& hit, Vec3& normal) const {
    for (u32 s = 1; s <= steps_; ++s) {
        const Vec3 p = from + (to - from) * (static_cast<f32>(s) / static_cast<f32>(steps_));
        const Vec4 clip = view_projection_ * Vec4(p.x, p.y, p.z, 1.0f);
        if (clip.w <= 1e-6f) continue;
        const f32 nx = clip.x / clip.w, ny = clip.y / clip.w, nz = clip.z / clip.w;
        if (nx < -1.0f || nx > 1.0f || ny < -1.0f || ny > 1.0f) continue; // off screen: nothing known
        const i32 x = std::clamp(static_cast<i32>((nx * 0.5f + 0.5f) * static_cast<f32>(width_)), 0, width_ - 1);
        const i32 y = std::clamp(static_cast<i32>((0.5f - ny * 0.5f) * static_cast<f32>(height_)), 0, height_ - 1);
        const f32 scene = depth_[static_cast<usize>(y) * static_cast<usize>(width_) + static_cast<usize>(x)];
        if (nz <= scene) continue; // still in front
        Vec3 surface;
        if (!SurfaceAt(x, y, surface) || (p - surface).Length() > thickness_) continue;
        // The normal from the neighbouring pixels' surface points.
        Vec3 sx, sy;
        const bool has_x = SurfaceAt(x + 1, y, sx) || SurfaceAt(x - 1, y, sx);
        const bool has_y = SurfaceAt(x, y + 1, sy) || SurfaceAt(x, y - 1, sy);
        Vec3 n(0, 1, 0);
        if (has_x && has_y) {
            const Vec3 c = (sx - surface).Cross(sy - surface);
            if (c.Length() > 1e-12f) n = c.Normalized();
        }
        if (n.Dot(from - surface) < 0.0f) n = n * -1.0f;
        hit = surface;
        normal = n;
        return true;
    }
    return false;
}

} // namespace aether::vfx
