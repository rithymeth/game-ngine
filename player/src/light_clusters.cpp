#include "light_clusters.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace aether::player {

namespace {

constexpr f32 kDegreesToRadians = 3.14159265358979f / 180.0f;

void Set4(f32 (&out)[4], f32 x, f32 y, f32 z, f32 w) {
    out[0] = x;
    out[1] = y;
    out[2] = z;
    out[3] = w;
}

// The sphere that holds what a light can reach: the range's ball for a point light; for a spot, the ball's sector inside the cone,
// bounded by the smallest sphere through its apex and rim (a narrow cone) or round its rim (a wide one).
void BoundingSphere(const ClusterLight& light, Vec3& center, f32& radius) {
    const Vec3 p(light.position_range[0], light.position_range[1], light.position_range[2]);
    const f32 range = light.position_range[3];
    const f32 cos_outer = light.direction_outer[3];
    center = p;
    radius = range;
    if (cos_outer <= 0.0f) return; // a point light, or a cone of 90 degrees or more: the whole ball
    const Vec3 d(light.direction_outer[0], light.direction_outer[1], light.direction_outer[2]);
    if (cos_outer < 0.70710678f) { // wider than 45 degrees
        const f32 sin_outer = std::sqrt(std::max(0.0f, 1.0f - cos_outer * cos_outer));
        center = p + d * (range * cos_outer);
        radius = range * sin_outer;
    } else {
        const f32 half = range / (2.0f * cos_outer);
        center = p + d * half;
        radius = half;
    }
}

struct Binned {
    u32 light;
    u32 x0, x1, y0, y1, s0, s1;
};

} // namespace

ClusterLight MakePointClusterLight(const Vec3& position, f32 range, const Vec3& color) {
    ClusterLight light;
    Set4(light.position_range, position.x, position.y, position.z, range);
    // The cone test passes everywhere: smoothstep(-2, -1.5, x) is 1 for every cosine x.
    Set4(light.color_inner, color.x, color.y, color.z, -1.5f);
    Set4(light.direction_outer, 0.0f, 0.0f, -1.0f, -2.0f);
    return light;
}

ClusterLight MakeSpotClusterLight(const Vec3& position, f32 range, const Vec3& color, const Vec3& direction, f32 outer_degrees,
                                  f32 inner_degrees) {
    const f32 outer = std::clamp(outer_degrees, 0.0f, 89.0f), inner = std::clamp(inner_degrees, 0.0f, outer);
    const f32 length = std::sqrt(direction.Dot(direction));
    const Vec3 d = length > 1e-6f ? direction * (1.0f / length) : Vec3(0.0f, 0.0f, -1.0f);
    ClusterLight light;
    Set4(light.position_range, position.x, position.y, position.z, range);
    Set4(light.color_inner, color.x, color.y, color.z, std::cos(inner * kDegreesToRadians));
    Set4(light.direction_outer, d.x, d.y, d.z, std::cos(outer * kDegreesToRadians));
    return light;
}

u32 ClusterSlice(f32 view_depth, f32 depth_scale, f32 depth_bias) {
    const f32 slice = std::floor(std::log(std::max(view_depth, 1e-6f)) * depth_scale + depth_bias);
    return static_cast<u32>(std::clamp(slice, 0.0f, static_cast<f32>(kClusterSlices - 1)));
}

void BuildLightClusters(std::span<const ClusterLight> lights, const ClusterView& view, LightClusters& out) {
    const f32 near_plane = std::max(view.near_plane, 1e-4f);
    const f32 far_plane = std::max(view.far_plane, near_plane * 1.001f);
    out.depth_scale = static_cast<f32>(kClusterSlices) / std::log(far_plane / near_plane);
    out.depth_bias = -std::log(near_plane) * out.depth_scale;
    out.light_count = 0;
    out.index_count = 0;
    out.dropped_lights = 0;

    // Each light in view, with its cells, nearest first.
    struct Candidate {
        f32 key; // distance from the camera to the light's bounding sphere (0 inside it)
        Binned cells;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(lights.size());
    for (u32 i = 0; i < lights.size(); ++i) {
        const ClusterLight& light = lights[i];
        if (!(light.position_range[3] > 0.0f)) continue; // no range (or NaN): nothing lit
        Vec3 center;
        f32 radius = 0.0f;
        BoundingSphere(light, center, radius);
        const Vec4 c = view.view * Vec4(center.x, center.y, center.z, 1.0f);
        const f32 depth = -c.z;
        if (depth + radius < near_plane || depth - radius > far_plane) continue;
        Binned cells{i, 0, kClusterTilesX - 1, 0, kClusterTilesY - 1, 0, 0};
        cells.s0 = ClusterSlice(std::max(depth - radius, near_plane), out.depth_scale, out.depth_bias);
        cells.s1 = ClusterSlice(std::min(depth + radius, far_plane), out.depth_scale, out.depth_bias);
        if (depth - radius > near_plane) {
            // Wholly in front of the near plane: the screen rectangle of the sphere's view-space box (which holds the sphere's).
            f32 min_x = 1e30f, min_y = 1e30f, max_x = -1e30f, max_y = -1e30f;
            for (int corner = 0; corner < 8; ++corner) {
                const Vec4 p((corner & 1) ? c.x + radius : c.x - radius, (corner & 2) ? c.y + radius : c.y - radius,
                             (corner & 4) ? c.z + radius : c.z - radius, 1.0f);
                const Vec4 clip = view.projection * p;
                const f32 w = clip.w > 1e-6f ? clip.w : 1e-6f;
                min_x = std::min(min_x, clip.x / w), max_x = std::max(max_x, clip.x / w);
                min_y = std::min(min_y, clip.y / w), max_y = std::max(max_y, clip.y / w);
            }
            if (max_x < -1.0f || min_x > 1.0f || max_y < -1.0f || min_y > 1.0f) continue; // beside the frustum
            const auto tile = [](f32 t, u32 tiles) {
                return static_cast<u32>(std::clamp(std::floor(t * static_cast<f32>(tiles)), 0.0f, static_cast<f32>(tiles - 1)));
            };
            // Pixel rows run down the screen; NDC's y runs up.
            cells.x0 = tile(min_x * 0.5f + 0.5f, kClusterTilesX), cells.x1 = tile(max_x * 0.5f + 0.5f, kClusterTilesX);
            cells.y0 = tile(0.5f - max_y * 0.5f, kClusterTilesY), cells.y1 = tile(0.5f - min_y * 0.5f, kClusterTilesY);
        }
        const f32 distance = std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z);
        candidates.push_back({std::max(0.0f, distance - radius), cells});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.key < b.key; });

    // Keep them while they fit, then count each cell's lights, give each cell its run of the index list and fill the runs.
    std::vector<Binned> kept;
    kept.reserve(std::min<usize>(candidates.size(), kMaxClusterLights));
    u32 total = 0;
    for (const Candidate& candidate : candidates) {
        const Binned& b = candidate.cells;
        const u32 cells = (b.x1 - b.x0 + 1) * (b.y1 - b.y0 + 1) * (b.s1 - b.s0 + 1);
        if (kept.size() >= kMaxClusterLights || total + cells > kMaxClusterIndices) {
            ++out.dropped_lights;
            continue;
        }
        kept.push_back(b);
        total += cells;
    }
    const auto cell_index = [](u32 x, u32 y, u32 s) { return (s * kClusterTilesY + y) * kClusterTilesX + x; };
    std::vector<u32> counts(kClusterCells, 0);
    for (const Binned& b : kept) {
        for (u32 s = b.s0; s <= b.s1; ++s)
            for (u32 y = b.y0; y <= b.y1; ++y)
                for (u32 x = b.x0; x <= b.x1; ++x) ++counts[cell_index(x, y, s)];
    }
    out.light_count = static_cast<u32>(kept.size());
    out.index_count = total;
    out.cells_offset = out.light_count * static_cast<u32>(sizeof(ClusterLight));
    out.indices_offset = out.cells_offset + kClusterCells * 8;
    // 16-bit indices, the list padded to whole 32-bit words (the shader reads words).
    out.bytes.assign(static_cast<usize>(out.indices_offset) + (static_cast<usize>(total) * 2 + 3) / 4 * 4, 0);
    u8* bytes = out.bytes.data();
    std::vector<u32> cursor(kClusterCells, 0);
    u32 first = 0;
    for (u32 cell = 0; cell < kClusterCells; ++cell) {
        const u32 run[2] = {first, counts[cell]};
        std::memcpy(bytes + out.cells_offset + cell * 8, run, sizeof(run));
        cursor[cell] = first;
        first += counts[cell];
    }
    for (u32 k = 0; k < kept.size(); ++k) {
        const Binned& b = kept[k];
        std::memcpy(bytes + k * sizeof(ClusterLight), &lights[b.light], sizeof(ClusterLight));
        for (u32 s = b.s0; s <= b.s1; ++s)
            for (u32 y = b.y0; y <= b.y1; ++y)
                for (u32 x = b.x0; x <= b.x1; ++x) {
                    const u32 slot = cursor[cell_index(x, y, s)]++;
                    const u16 index = static_cast<u16>(k);
                    std::memcpy(bytes + out.indices_offset + slot * 2, &index, sizeof(index));
                }
    }
}

} // namespace aether::player
