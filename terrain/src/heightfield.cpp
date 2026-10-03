#include "aether/terrain/heightfield.h"

#include <algorithm>
#include <cmath>

namespace aether {
namespace terrain {

namespace {

f32 ExtentX(const Heightmap& hm) { return hm.width > 1 ? static_cast<f32>(hm.width - 1) * hm.cell_size : 0.0f; }
f32 ExtentZ(const Heightmap& hm) { return hm.height > 1 ? static_cast<f32>(hm.height - 1) * hm.cell_size : 0.0f; }

} // namespace

bool HeightfieldContains(const Heightmap& hm, f32 x, f32 z) {
    if (hm.width == 0 || hm.height == 0) return false;
    return x >= 0.0f && z >= 0.0f && x <= ExtentX(hm) && z <= ExtentZ(hm);
}

f32 HeightfieldHeightAt(const Heightmap& hm, f32 vertical_scale, f32 x, f32 z) {
    if (hm.width == 0 || hm.height == 0) return 0.0f;
    const f32 cx = std::clamp(x, 0.0f, ExtentX(hm));
    const f32 cz = std::clamp(z, 0.0f, ExtentZ(hm));
    return hm.SampleLinear(cx, cz) * vertical_scale;
}

Vec3 HeightfieldNormalAt(const Heightmap& hm, f32 vertical_scale, f32 x, f32 z) {
    const f32 d = hm.cell_size > 0.0f ? hm.cell_size : 1.0f;
    const f32 dhdx = (HeightfieldHeightAt(hm, vertical_scale, x + d, z) -
                      HeightfieldHeightAt(hm, vertical_scale, x - d, z)) / (2.0f * d);
    const f32 dhdz = (HeightfieldHeightAt(hm, vertical_scale, x, z + d) -
                      HeightfieldHeightAt(hm, vertical_scale, x, z - d)) / (2.0f * d);
    return Vec3(-dhdx, 1.0f, -dhdz).Normalized();
}

HeightfieldHit HeightfieldRaycast(const Heightmap& hm, f32 vertical_scale,
                                  const Vec3& origin, const Vec3& direction,
                                  f32 max_distance) {
    HeightfieldHit result;
    const f32 dir_len = direction.Length();
    if (hm.width < 2 || hm.height < 2 || dir_len <= 0.0f || max_distance <= 0.0f) return result;
    const Vec3 dir = direction * (1.0f / dir_len);

    // Signed height of a ray point above the surface; outside the footprint it
    // counts as above so the ray can enter from the side.
    auto above = [&](f32 t) {
        const Vec3 p = origin + dir * t;
        if (!HeightfieldContains(hm, p.x, p.z)) return 1.0f;
        return p.y - HeightfieldHeightAt(hm, vertical_scale, p.x, p.z);
    };

    // Sub-cell steps so a ray cannot skip over a ridge between samples.
    const f32 step = std::max(hm.cell_size * 0.25f, 1e-3f);
    if (above(0.0f) < 0.0f) return result;

    f32 prev_t = 0.0f;
    for (f32 t = step;; t += step) {
        const f32 cur_t = std::min(t, max_distance);
        if (above(cur_t) < 0.0f) {
            f32 lo = prev_t, hi = cur_t;
            for (int i = 0; i < 24; ++i) {
                const f32 mid = 0.5f * (lo + hi);
                (above(mid) < 0.0f ? hi : lo) = mid;
            }
            const f32 hit_t = 0.5f * (lo + hi);
            result.hit = true;
            result.distance = hit_t;
            result.point = origin + dir * hit_t;
            result.normal = HeightfieldNormalAt(hm, vertical_scale, result.point.x, result.point.z);
            return result;
        }
        if (cur_t >= max_distance) break;
        prev_t = cur_t;
    }
    return result;
}

f32 HeightfieldSpherePenetration(const Heightmap& hm, f32 vertical_scale,
                                 const Vec3& center, f32 radius) {
    if (!HeightfieldContains(hm, center.x, center.z)) return 0.0f;
    const f32 surface = HeightfieldHeightAt(hm, vertical_scale, center.x, center.z);
    const Vec3 normal = HeightfieldNormalAt(hm, vertical_scale, center.x, center.z);
    // Signed distance from the center to the tangent plane at the surface point.
    const f32 plane_distance = (center.y - surface) * normal.y;
    return std::max(0.0f, radius - plane_distance);
}

} // namespace terrain
} // namespace aether
