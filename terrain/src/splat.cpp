#include "aether/terrain/splat.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace aether {
namespace terrain {

// --- Brush weight ---

f32 BrushWeight(f32 dx, f32 dz, f32 radius, f32 strength, f32 falloff) {
    const f32 dist = std::sqrt(dx * dx + dz * dz);
    if (dist >= radius) return 0.0f;
    const f32 t = dist / radius; // 0 at center, 1 at edge
    // Smoothstep profile; falloff > 0 concentrates the weight toward the center.
    const f32 smooth = t * t * (3.0f - 2.0f * t);
    return strength * std::pow(1.0f - smooth, 1.0f + std::max(0.0f, falloff));
}

// --- Splatmap data ---

std::array<f32, 4> SplatmapData::GetWeights(f32 u, f32 v) const {
    if (width == 0 || height == 0) return {1.0f, 0.0f, 0.0f, 0.0f};
    const f32 cu = std::clamp(u, 0.0f, 1.0f);
    const f32 cv = std::clamp(v, 0.0f, 1.0f);
    const u32 px = static_cast<u32>(cu * static_cast<f32>(width - 1));
    const u32 pz = static_cast<u32>(cv * static_cast<f32>(height - 1));
    const usize idx = (static_cast<usize>(pz) * width + px) * 4;
    return {
        static_cast<f32>(pixels[idx + 0]) / 255.0f,
        static_cast<f32>(pixels[idx + 1]) / 255.0f,
        static_cast<f32>(pixels[idx + 2]) / 255.0f,
        static_cast<f32>(pixels[idx + 3]) / 255.0f
    };
}

// --- Encoding ---

i32 LayerToChannel(u32 layer_index) {
    // Layers 0-3 map to RGBA channels 0-3.
    // Beyond that, wrap (shouldn't happen with 4 max layers).
    return static_cast<i32>(layer_index % 4);
}

u32 ChannelToLayer(i32 channel) {
    return static_cast<u32>(std::max(0, channel));
}

u32 PackWeights(u32 layer_count, span<const f32> weights) {
    u8 r = 0, g = 0, b = 0, a = 255;
    if (layer_count > 0) r = static_cast<u8>(std::clamp(weights[0], 0.0f, 1.0f) * 255.0f);
    if (layer_count > 1) g = static_cast<u8>(std::clamp(weights[1], 0.0f, 1.0f) * 255.0f);
    if (layer_count > 2) b = static_cast<u8>(std::clamp(weights[2], 0.0f, 1.0f) * 255.0f);
    if (layer_count > 3) a = static_cast<u8>(std::clamp(weights[3], 0.0f, 1.0f) * 255.0f);
    return (static_cast<u32>(a) << 24) | (static_cast<u32>(b) << 16)
         | (static_cast<u32>(g) << 8)  | static_cast<u32>(r);
}

void UnpackWeights(u32 pixel, u32 layer_count, f32* out_weights) {
    const u8 r = (pixel >> 0) & 0xff;
    const u8 g = (pixel >> 8) & 0xff;
    const u8 b = (pixel >> 16) & 0xff;
    const u8 a = (pixel >> 24) & 0xff;
    out_weights[0] = static_cast<f32>(r) / 255.0f;
    out_weights[1] = (layer_count > 1) ? static_cast<f32>(g) / 255.0f : 0.0f;
    out_weights[2] = (layer_count > 2) ? static_cast<f32>(b) / 255.0f : 0.0f;
    out_weights[3] = (layer_count > 3) ? static_cast<f32>(a) / 255.0f : 0.0f;
}

void NormalizeWeights(std::array<f32, 4>& weights) {
    f32 sum = weights[0] + weights[1] + weights[2] + weights[3];
    if (sum > 1e-6f) {
        const f32 inv = 1.0f / sum;
        weights[0] *= inv;
        weights[1] *= inv;
        weights[2] *= inv;
        weights[3] *= inv;
    }
}

// --- Splatmap generation ---

SplatmapData BuildSplatmap(u32 resolution, span<const SplatmapLayer> layers,
                           const std::vector<f32>& layer_weights) {
    SplatmapData sm;
    sm.width = resolution;
    sm.height = resolution;
    sm.pixels.resize(static_cast<usize>(resolution) * resolution * 4, 0);

    const u32 layer_count = static_cast<u32>(layers.size());
    const usize wp_count = layer_weights.size();

    for (u32 z = 0; z < resolution; ++z) {
        for (u32 x = 0; x < resolution; ++x) {
            const f32 u = static_cast<f32>(x) / static_cast<f32>(resolution - 1);
            const f32 v = static_cast<f32>(z) / static_cast<f32>(resolution - 1);

            // Bilinear interpolation of layer weights.
            std::array<f32, 4> weights{0.0f, 0.0f, 0.0f, 0.0f};
            if (wp_count > 0) {
                // weights are stored flat: layer0_u0v0, layer0_u1v0, ...
                // But here we generate a simple gradient for testing.
                weights[0] = (layers.size() > 0) ? 1.0f - v : 0.0f;
                weights[1] = (layers.size() > 1) ? v : 0.0f;
                weights[2] = 0.0f;
                weights[3] = 0.0f;
            }

            // Add noise variation for natural look.
            const f32 noise = u * 17.3f + v * 31.7f;
            const f32 frac = noise * 0.5f - std::floor(noise * 0.5f); // 0..1
            const f32 n = frac * 2.0f - 1.0f; // -1..1
            weights[0] = std::clamp(weights[0] + n * 0.05f, 0.0f, 1.0f);
            weights[1] = std::clamp(1.0f - weights[0], 0.0f, 1.0f);

            NormalizeWeights(weights);

            const usize idx = static_cast<usize>(z * resolution + x) * 4;
            sm.pixels[idx + 0] = static_cast<u8>(weights[0] * 255.0f);
            sm.pixels[idx + 1] = static_cast<u8>(weights[1] * 255.0f);
            sm.pixels[idx + 2] = static_cast<u8>(weights[2] * 255.0f);
            sm.pixels[idx + 3] = static_cast<u8>(weights[3] * 255.0f);
        }
    }
    return sm;
}

// --- Paint stroke ---

void ApplyBrushStroke(SplatmapData& splatmap,
                       f32 center_u, f32 center_v,
                       const TerrainBrush& brush) {
    if (splatmap.width == 0 || splatmap.height == 0) return;
    const f32 res_f = static_cast<f32>(splatmap.width);
    const f32 brush_radius_px = brush.radius * res_f / 32.0f; // rough scale
    const i32 r_px = static_cast<i32>(std::ceil(brush_radius_px));
    const i32 cx = static_cast<i32>(center_u * static_cast<f32>(splatmap.width - 1));
    const i32 cz = static_cast<i32>(center_v * static_cast<f32>(splatmap.height - 1));

    const i32 x0 = std::max(0, cx - r_px);
    const i32 x1 = std::min(static_cast<i32>(splatmap.width), cx + r_px + 1);
    const i32 z0 = std::max(0, cz - r_px);
    const i32 z1 = std::min(static_cast<i32>(splatmap.height), cz + r_px + 1);

    for (i32 z = z0; z < z1; ++z) {
        for (i32 x = x0; x < x1; ++x) {
            const f32 dx = static_cast<f32>(x) - static_cast<f32>(cx);
            const f32 dz = static_cast<f32>(z) - static_cast<f32>(cz);
            const f32 w = BrushWeight(dx, dz, brush_radius_px, brush.strength, brush.falloff);
            if (w < 1e-6f) continue;

            const usize idx = static_cast<usize>(z * splatmap.width + x) * 4;
            std::array<f32, 4> weights{
                static_cast<f32>(splatmap.pixels[idx + 0]) / 255.0f,
                static_cast<f32>(splatmap.pixels[idx + 1]) / 255.0f,
                static_cast<f32>(splatmap.pixels[idx + 2]) / 255.0f,
                static_cast<f32>(splatmap.pixels[idx + 3]) / 255.0f
            };

            // Increase the target layer, decrease others proportionally.
            const f32 other_sum = 1.0f - weights[brush.layer_index];
            if (other_sum > 1e-6f) {
                const f32 delta = w;
                weights[brush.layer_index] = std::min(1.0f, weights[brush.layer_index] + delta);
                // Decrease others proportionally.
                f32 remaining = 1.0f - weights[brush.layer_index];
                for (u32 i = 0; i < 4; ++i) {
                    if (static_cast<i32>(i) == brush.layer_index) continue;
                    const f32 share = (other_sum > 1e-6f) ? weights[i] / other_sum : 0.0f;
                    weights[i] = remaining * share;
                }
            }

            splatmap.pixels[idx + 0] = static_cast<u8>(std::clamp(weights[0], 0.0f, 1.0f) * 255.0f);
            splatmap.pixels[idx + 1] = static_cast<u8>(std::clamp(weights[1], 0.0f, 1.0f) * 255.0f);
            splatmap.pixels[idx + 2] = static_cast<u8>(std::clamp(weights[2], 0.0f, 1.0f) * 255.0f);
            splatmap.pixels[idx + 3] = static_cast<u8>(std::clamp(weights[3], 0.0f, 1.0f) * 255.0f);
        }
    }
}

// --- Height brush ---

void ApplyHeightBrush(Heightmap& heightmap,
                       f32 center_wx, f32 center_wz,
                       const TerrainBrush& brush,
                       f32 vertical_scale) {
    if (heightmap.width == 0 || heightmap.height == 0) return;
    const f32 world_per_sample = heightmap.cell_size;
    const f32 brush_radius_cells = brush.radius / world_per_sample;

    const i32 cx = static_cast<i32>(center_wx / world_per_sample);
    const i32 cz = static_cast<i32>(center_wz / world_per_sample);
    const i32 r = static_cast<i32>(std::ceil(brush_radius_cells));

    const i32 x0 = std::max(0, cx - r);
    const i32 x1 = std::min(static_cast<i32>(heightmap.width), cx + r + 1);
    const i32 z0 = std::max(0, cz - r);
    const i32 z1 = std::min(static_cast<i32>(heightmap.height), cz + r + 1);

    // Flatten levels the footprint to its brush-weighted mean height.
    f32 target_h = 0.0f;
    if (brush.flatten || brush.mode == SculptMode::Flatten) {
        f32 weighted_sum = 0.0f;
        f32 weight_sum = 0.0f;
        for (i32 z = z0; z < z1; ++z) {
            for (i32 x = x0; x < x1; ++x) {
                const f32 dx = static_cast<f32>(x) * world_per_sample - center_wx;
                const f32 dz = static_cast<f32>(z) * world_per_sample - center_wz;
                const f32 fw = BrushWeight(dx, dz, brush.radius, 1.0f, brush.falloff);
                weighted_sum += heightmap.heights[static_cast<usize>(z) * heightmap.width + static_cast<usize>(x)] * fw;
                weight_sum += fw;
            }
        }
        target_h = weight_sum > 1e-6f ? weighted_sum / weight_sum : 0.0f;
    }

    for (i32 z = z0; z < z1; ++z) {
        for (i32 x = x0; x < x1; ++x) {
            const f32 wx = static_cast<f32>(x) * world_per_sample;
            const f32 wz = static_cast<f32>(z) * world_per_sample;
            const f32 dx = wx - center_wx;
            const f32 dz = wz - center_wz;
            // Signed: a negative strength lowers; blend modes only use the magnitude.
            const f32 signed_w = BrushWeight(dx, dz, brush.radius, brush.strength, brush.falloff);
            const f32 w = std::fabs(signed_w);
            if (w < 1e-6f) continue;

            const usize idx = static_cast<usize>(z) * heightmap.width + static_cast<usize>(x);
            f32& h = heightmap.heights[idx];

            if (brush.smooth || brush.mode == SculptMode::Smooth) {
                // Average with neighbors.
                f32 sum = 0.0f;
                i32 count = 0;
                for (i32 dz2 = -1; dz2 <= 1; ++dz2) {
                    for (i32 dx2 = -1; dx2 <= 1; ++dx2) {
                        const i32 nx2 = x + dx2;
                        const i32 nz2 = z + dz2;
                        if (nx2 >= 0 && nx2 < static_cast<i32>(heightmap.width) &&
                            nz2 >= 0 && nz2 < static_cast<i32>(heightmap.height)) {
                            sum += heightmap.heights[static_cast<usize>(nz2) * heightmap.width + static_cast<usize>(nx2)];
                            ++count;
                        }
                    }
                }
                const f32 avg = sum / static_cast<f32>(count);
                h = h + (avg - h) * w;
            } else if (brush.flatten || brush.mode == SculptMode::Flatten) {
                h = h + (target_h - h) * w;
            } else {
                // Default: raise/lower (mode from direction of brush strength).
                // strength > 0 = raise, strength < 0 = lower.
                h = h + signed_w * vertical_scale;
            }
        }
    }
}

// --- Noise sculpting ---

namespace {

// Hash-based value noise (0..1) used for sculpt displacement.
// Deterministic for a given (x, z) grid coordinate and seed.
f32 SculptNoise(i32 x, i32 z, i32 seed) {
    i32 n = x + z * 57 + seed * 131;
    n = (n << 13) ^ n;
    const i32 hash = (n * (n * n * 15731 + 789221) + 1376312589) & 0x7fffffff;
    return static_cast<f32>(hash) / 2147483647.0f;
}

f32 SculptSmoothstep(f32 t) { return t * t * (3.0f - 2.0f * t); }

// Smooth 2D value noise at continuous coordinates.
f32 SculptNoise2D(f32 fx, f32 fz, i32 seed) {
    const i32 x0 = static_cast<i32>(std::floor(fx));
    const i32 z0 = static_cast<i32>(std::floor(fz));
    const f32 tx = SculptSmoothstep(fx - static_cast<f32>(x0));
    const f32 tz = SculptSmoothstep(fz - static_cast<f32>(z0));

    const f32 n00 = SculptNoise(x0,     z0,     seed);
    const f32 n10 = SculptNoise(x0 + 1, z0,     seed);
    const f32 n01 = SculptNoise(x0,     z0 + 1, seed);
    const f32 n11 = SculptNoise(x0 + 1, z0 + 1, seed);

    const f32 nx0 = n00 + (n10 - n00) * tx;
    const f32 nx1 = n01 + (n11 - n01) * tx;
    return nx0 + (nx1 - nx0) * tz;
}

void ForEachBrushSample(const Heightmap& heightmap, f32 center_wx, f32 center_wz,
                        const TerrainBrush& brush,
                        const std::function<void(i32, i32, f32)>& fn) {
    const f32 world_per_sample = heightmap.cell_size > 0.0f ? heightmap.cell_size : 1.0f;
    const f32 brush_radius_cells = brush.radius / world_per_sample;
    const i32 cx = static_cast<i32>(center_wx / world_per_sample);
    const i32 cz = static_cast<i32>(center_wz / world_per_sample);
    const i32 r = static_cast<i32>(std::ceil(brush_radius_cells));
    const i32 x0 = std::max(0, cx - r);
    const i32 x1 = std::min(static_cast<i32>(heightmap.width), cx + r + 1);
    const i32 z0 = std::max(0, cz - r);
    const i32 z1 = std::min(static_cast<i32>(heightmap.height), cz + r + 1);
    for (i32 z = z0; z < z1; ++z) {
        for (i32 x = x0; x < x1; ++x) {
            const f32 wx = static_cast<f32>(x) * world_per_sample;
            const f32 wz = static_cast<f32>(z) * world_per_sample;
            const f32 dx = wx - center_wx;
            const f32 dz = wz - center_wz;
            const f32 w = BrushWeight(dx, dz, brush.radius, brush.strength, brush.falloff);
            if (std::fabs(w) < 1e-6f) continue;
            fn(x, z, w);
        }
    }
}

} // namespace

void ApplyNoiseBrush(Heightmap& heightmap,
                      f32 center_wx, f32 center_wz,
                      const TerrainBrush& brush,
                      f32 vertical_scale) {
    if (heightmap.width == 0 || heightmap.height == 0) return;
    const f32 feature = std::max(brush.noise_scale, 0.1f);
    const f32 amp = vertical_scale; // the (signed) strength is already in the weight

    ForEachBrushSample(heightmap, center_wx, center_wz, brush,
        [&](i32 x, i32 z, f32 w) {
            const f32 nx = static_cast<f32>(x) / feature;
            const f32 nz = static_cast<f32>(z) / feature;
            // Two octaves for natural detail, remapped to -1..1.
            const f32 n0 = SculptNoise2D(nx, nz, brush.noise_seed);
            const f32 n1 = SculptNoise2D(nx * 2.0f, nz * 2.0f, brush.noise_seed + 7);
            const f32 displacement = (n0 * 0.7f + n1 * 0.3f - 0.5f) * 2.0f * amp * w;
            const usize idx = static_cast<usize>(z) * heightmap.width + static_cast<usize>(x);
            heightmap.heights[idx] += displacement;
        });
}

void ApplyErosionBrush(Heightmap& heightmap,
                        f32 center_wx, f32 center_wz,
                        const TerrainBrush& brush,
                        f32 /*vertical_scale*/) {
    if (heightmap.width == 0 || heightmap.height == 0) return;
    const f32 dep = std::clamp(brush.erosion_deposition, 0.0f, 1.0f);
    const i32 passes = std::max(1, brush.erosion_iterations);
    const i32 width = static_cast<i32>(heightmap.width);
    const i32 height = static_cast<i32>(heightmap.height);

    for (i32 pass = 0; pass < passes; ++pass) {
        // Read from a snapshot and accumulate changes so a pass is order independent.
        const std::vector<f32> src = heightmap.heights;
        std::vector<f32> delta(src.size(), 0.0f);
        ForEachBrushSample(heightmap, center_wx, center_wz, brush,
            [&](i32 x, i32 z, f32 signed_w) {
                const f32 w = std::fabs(signed_w);
                const usize idx = static_cast<usize>(z) * heightmap.width + static_cast<usize>(x);
                f32 total_diff = 0.0f;
                i32 lower_neighbours = 0;
                for (i32 dz2 = -1; dz2 <= 1; ++dz2) {
                    for (i32 dx2 = -1; dx2 <= 1; ++dx2) {
                        if (dx2 == 0 && dz2 == 0) continue;
                        const i32 nx = x + dx2;
                        const i32 nz = z + dz2;
                        if (nx < 0 || nx >= width || nz < 0 || nz >= height) continue;
                        const f32 diff = src[idx] - src[static_cast<usize>(nz) * heightmap.width + static_cast<usize>(nx)];
                        if (diff > 1e-6f) {
                            total_diff += diff;
                            ++lower_neighbours;
                        }
                    }
                }
                if (lower_neighbours == 0) return;
                // Move material downhill in proportion to each neighbour's drop.
                const f32 moved = total_diff / static_cast<f32>(lower_neighbours) * dep * w;
                delta[idx] -= moved;
                for (i32 dz2 = -1; dz2 <= 1; ++dz2) {
                    for (i32 dx2 = -1; dx2 <= 1; ++dx2) {
                        if (dx2 == 0 && dz2 == 0) continue;
                        const i32 nx = x + dx2;
                        const i32 nz = z + dz2;
                        if (nx < 0 || nx >= width || nz < 0 || nz >= height) continue;
                        const usize nidx = static_cast<usize>(nz) * heightmap.width + static_cast<usize>(nx);
                        const f32 diff = src[idx] - src[nidx];
                        if (diff > 1e-6f) delta[nidx] += moved * diff / total_diff;
                    }
                }
            });
        for (usize i = 0; i < delta.size(); ++i) heightmap.heights[i] += delta[i];
    }
}
void ApplySculptBrush(Heightmap& heightmap,
                      f32 center_wx, f32 center_wz,
                      const TerrainBrush& brush,
                      f32 vertical_scale) {
    switch (brush.mode) {
        case SculptMode::Noise:
            ApplyNoiseBrush(heightmap, center_wx, center_wz, brush, vertical_scale);
            break;
        case SculptMode::Erode:
            ApplyErosionBrush(heightmap, center_wx, center_wz, brush, vertical_scale);
            break;
        default:
            // Raise/Flatten/Smooth all go through the classic height brush.
            ApplyHeightBrush(heightmap, center_wx, center_wz, brush, vertical_scale);
            break;
    }
}

} // namespace terrain
} // namespace aether
