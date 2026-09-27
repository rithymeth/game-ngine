#include "aether/renderer/clusters.h"

#include <algorithm>
#include <cmath>

namespace aether {

namespace {

struct Box {
    Vec3 min, max;
};

// Squared distance from a point to a box (0 inside).
f32 DistanceSq(const Box& b, const Vec3& p) {
    auto axis = [](f32 v, f32 lo, f32 hi) { return v < lo ? lo - v : v > hi ? v - hi : 0.0f; };
    const f32 dx = axis(p.x, b.min.x, b.max.x), dy = axis(p.y, b.min.y, b.max.y), dz = axis(p.z, b.min.z, b.max.z);
    return dx * dx + dy * dy + dz * dz;
}

// Half the view's width and height at view-space depth `d` (orthographic: constant).
void HalfSize(const View& view, f32 d, f32& half_w, f32& half_h) {
    if (view.perspective) {
        half_h = d * std::tan(Radians(view.fov_degrees) * 0.5f);
    } else {
        half_h = view.ortho_height * 0.5f;
    }
    half_w = half_h * view.aspect;
}

} // namespace

u32 LightClusters::SliceOf(f32 depth) const {
    // slice_depths is sorted: find the slice whose range holds `depth`.
    const auto it = std::upper_bound(slice_depths.begin(), slice_depths.end(), depth);
    const i64 slice = static_cast<i64>(it - slice_depths.begin()) - 1;
    return static_cast<u32>(std::clamp<i64>(slice, 0, static_cast<i64>(grid.z) - 1));
}

bool LightClusters::ClusterOf(const View& view, const Vec3& world_point, u32& index) const {
    const Vec3 v = view.ToViewSpace(world_point);
    const f32 depth = -v.z;
    if (slice_depths.empty() || depth < slice_depths.front() || depth > slice_depths.back()) return false;
    f32 half_w, half_h;
    HalfSize(view, depth, half_w, half_h);
    const f32 nx = v.x / half_w, ny = v.y / half_h; // -1..1 across the screen
    if (std::fabs(nx) > 1.0f || std::fabs(ny) > 1.0f) return false;
    const u32 ix = std::min(grid.x - 1, static_cast<u32>((nx * 0.5f + 0.5f) * static_cast<f32>(grid.x)));
    const u32 iy = std::min(grid.y - 1, static_cast<u32>((ny * 0.5f + 0.5f) * static_cast<f32>(grid.y)));
    index = grid.Index(ix, iy, SliceOf(depth));
    return true;
}

void SpotBoundingSphere(const RenderSpotLight& spot, Vec3& center, f32& radius) {
    // The smallest sphere around a cone of half-angle a and length r: for
    // wide cones it's centred on the cap; for narrow ones it passes through
    // the apex and the cap's rim.
    const f32 cos_a = std::clamp(spot.cos_outer, 0.0f, 1.0f);
    const f32 sin_a = std::sqrt(1.0f - cos_a * cos_a);
    if (cos_a < 0.7071068f) {
        center = spot.position + spot.direction * (cos_a * spot.range);
        radius = sin_a * spot.range;
    } else {
        const f32 half = spot.range / (2.0f * cos_a);
        center = spot.position + spot.direction * half;
        radius = half;
    }
}

LightClusters BuildLightClusters(const RenderScene& scene, const View& view, ClusterGrid grid, u32 max_per_cluster) {
    LightClusters out;
    out.grid = grid;
    out.max_per_cluster = max_per_cluster;
    out.clusters.resize(grid.Count());

    // Slices: exponential for perspective (thin near the camera), linear for orthographic.
    const f32 near_d = std::max(view.near_plane, 1e-4f), far_d = std::max(view.far_plane, near_d * 1.001f);
    out.slice_depths.resize(grid.z + 1);
    for (u32 k = 0; k <= grid.z; ++k) {
        const f32 t = static_cast<f32>(k) / static_cast<f32>(grid.z);
        out.slice_depths[k] = view.perspective ? near_d * std::pow(far_d / near_d, t) : near_d + (far_d - near_d) * t;
    }

    // Lights in view space, as spheres.
    struct Sphere {
        Vec3 center;
        f32 radius;
    };
    std::vector<Sphere> points, spots;
    for (const RenderPointLight& l : scene.point_lights) points.push_back({view.ToViewSpace(l.position), l.range});
    for (const RenderSpotLight& l : scene.spot_lights) {
        Vec3 c;
        f32 r;
        SpotBoundingSphere(l, c, r);
        spots.push_back({view.ToViewSpace(c), r});
    }

    std::vector<u32> scratch_points, scratch_spots;
    for (u32 iz = 0; iz < grid.z; ++iz) {
        const f32 d0 = out.slice_depths[iz], d1 = out.slice_depths[iz + 1];
        f32 w0, h0, w1, h1;
        HalfSize(view, d0, w0, h0);
        HalfSize(view, d1, w1, h1);
        for (u32 iy = 0; iy < grid.y; ++iy) {
            const f32 ny0 = -1.0f + 2.0f * static_cast<f32>(iy) / static_cast<f32>(grid.y);
            const f32 ny1 = -1.0f + 2.0f * static_cast<f32>(iy + 1) / static_cast<f32>(grid.y);
            for (u32 ix = 0; ix < grid.x; ++ix) {
                const f32 nx0 = -1.0f + 2.0f * static_cast<f32>(ix) / static_cast<f32>(grid.x);
                const f32 nx1 = -1.0f + 2.0f * static_cast<f32>(ix + 1) / static_cast<f32>(grid.x);
                // The froxel's view-space box: its four screen corners at both depths.
                Box box{Vec3(1e30f, 1e30f, -d1), Vec3(-1e30f, -1e30f, -d0)};
                for (const f32 nx : {nx0, nx1}) {
                    for (const f32 ny : {ny0, ny1}) {
                        for (const auto& [w, h] : {std::pair<f32, f32>{w0, h0}, {w1, h1}}) {
                            box.min.x = std::min(box.min.x, nx * w);
                            box.max.x = std::max(box.max.x, nx * w);
                            box.min.y = std::min(box.min.y, ny * h);
                            box.max.y = std::max(box.max.y, ny * h);
                        }
                    }
                }
                scratch_points.clear();
                scratch_spots.clear();
                for (u32 i = 0; i < points.size(); ++i) {
                    if (DistanceSq(box, points[i].center) <= points[i].radius * points[i].radius) scratch_points.push_back(i);
                }
                for (u32 i = 0; i < spots.size(); ++i) {
                    if (DistanceSq(box, spots[i].center) <= spots[i].radius * spots[i].radius) scratch_spots.push_back(i);
                }
                const usize total = scratch_points.size() + scratch_spots.size();
                if (total > max_per_cluster) {
                    out.dropped += total - max_per_cluster;
                    // Keep points first, then as many spots as fit.
                    if (scratch_points.size() > max_per_cluster) scratch_points.resize(max_per_cluster);
                    scratch_spots.resize(max_per_cluster - scratch_points.size());
                }
                ClusterLightRange& range = out.clusters[grid.Index(ix, iy, iz)];
                range.offset = static_cast<u32>(out.indices.size());
                range.point_count = static_cast<u16>(scratch_points.size());
                range.spot_count = static_cast<u16>(scratch_spots.size());
                out.indices.insert(out.indices.end(), scratch_points.begin(), scratch_points.end());
                out.indices.insert(out.indices.end(), scratch_spots.begin(), scratch_spots.end());
            }
        }
    }
    return out;
}

} // namespace aether
