#include "aether/terrain/foliage.h"
#include "terrain_math.h"
#include "terrain_noise.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>

namespace aether {
namespace terrain {

// --- Random helpers ---

Vec3 RandomScale(std::mt19937& rng, const FoliageType& type) {
    std::uniform_real_distribution<f32> scale_dist(type.scale_min, type.scale_max);
    const f32 s = scale_dist(rng);
    return Vec3(s, s, s);
}

Quaternion RandomRotation(std::mt19937& rng, const FoliageType& type) {
    std::uniform_real_distribution<f32> angle_dist(-type.rotation_range, type.rotation_range);
    const f32 angle = angle_dist(rng);
    return Quaternion::FromAxisAngle({0, 1, 0}, angle);
}

DensityMap GenerateDensityMap(u32 width, u32 height, f32 world_size,
                              u32 octaves, f32 persistence, f32 seed) {
    DensityMap dm;
    dm.width = width;
    dm.height = height;
    dm.cells.resize(static_cast<usize>(width) * height, 0);

    const f32 step_x = world_size / static_cast<f32>(width);
    const f32 step_z = world_size / static_cast<f32>(height);

    for (u32 z = 0; z < height; ++z) {
        for (u32 x = 0; x < width; ++x) {
            const f32 wx = static_cast<f32>(x) * step_x;
            const f32 wz = static_cast<f32>(z) * step_z;
            const f32 n = 0.5f * (FractalNoise(wx * 0.05f, wz * 0.05f, octaves, persistence,
                                               static_cast<u32>(seed * 17.0f + 3.0f)) + 1.0f);
            dm.cells[static_cast<usize>(z) * width + x] =
                static_cast<u8>(std::clamp(n, 0.0f, 1.0f) * 255.0f);
        }
    }
    return dm;
}

// --- Instance generation ---

namespace {
// The density map's value (0..1) under a world position (1 m cells); 1 without a map.
f32 DensityAt(const DensityMap& density, const Vec3& pos) {
    if (density.width == 0 || density.height == 0 || density.cells.size() < static_cast<usize>(density.width) * density.height) return 1.0f;
    const i32 x = std::clamp(static_cast<i32>(std::floor(pos.x)), 0, static_cast<i32>(density.width) - 1);
    const i32 z = std::clamp(static_cast<i32>(std::floor(pos.z)), 0, static_cast<i32>(density.height) - 1);
    return static_cast<f32>(density.cells[static_cast<usize>(z) * density.width + static_cast<usize>(x)]) / 255.0f;
}

// The rotation turning +Y onto `normal`.
Quaternion TiltTo(const Vec3& normal) {
    const Vec3 up(0, 1, 0);
    const Vec3 n = normal.Length() > 1e-6f ? normal.Normalized() : up;
    const f32 d = std::clamp(up.Dot(n), -1.0f, 1.0f);
    if (d > 0.99999f) return Quaternion::Identity();
    if (d < -0.99999f) return Quaternion::FromAxisAngle({1, 0, 0}, 3.14159265f);
    return Quaternion::FromAxisAngle(up.Cross(n).Normalized(), std::acos(d));
}

// One instance of a type picked at random, with its variation.
FoliageInstance MakeInstance(const FoliageLayer& layer, const Vec3& position, const Vec3& normal, std::mt19937& rng) {
    FoliageInstance inst;
    inst.type_index = 0;
    if (layer.types.size() > 1) {
        std::uniform_int_distribution<u32> pick(0, static_cast<u32>(layer.types.size() - 1));
        inst.type_index = pick(rng);
    }
    inst.normal = normal;
    inst.position = position;
    if (!layer.types.empty()) {
        const FoliageType& type = layer.types[inst.type_index];
        inst.position.y += type.anchor_offset;
        inst.scale = RandomScale(rng, type);
        inst.rotation = RandomRotation(rng, type);
        if (type.align_to_normal) inst.rotation = TiltTo(normal) * inst.rotation;
    }
    inst.hidden = false;
    return inst;
}
} // namespace

void GenerateInstances(FoliageLayer& layer, const DensityMap& density,
                     span<const Vec3> sample_positions,
                     span<const Vec3> sample_normals,
                     std::mt19937& rng) {
    layer.instances.clear();
    if (sample_positions.size() != sample_normals.size() || layer.types.empty()) return;
    layer.instances.reserve(std::min<usize>(sample_positions.size(), layer.max_instances));
    std::uniform_real_distribution<f32> chance(0.0f, 1.0f);
    for (usize i = 0; i < sample_positions.size() && layer.instances.size() < layer.max_instances; ++i) {
        // Each sample is kept with the density map's probability there.
        if (chance(rng) >= DensityAt(density, sample_positions[i])) continue;
        layer.instances.push_back(MakeInstance(layer, sample_positions[i], sample_normals[i], rng));
    }
}

// --- Brush operations ---

void AddInstance(FoliageLayer& layer, const Vec3& position,
                const Vec3& normal, std::mt19937& rng) {
    if (layer.instances.size() >= layer.max_instances) return;
    layer.instances.push_back(MakeInstance(layer, position, normal, rng));
}

void RemoveInstancesNear(FoliageLayer& layer, const Vec3& position, f32 radius) {
    const f32 r2 = radius * radius;
    std::erase_if(layer.instances, [&](const FoliageInstance& inst) {
        const Vec3 diff = inst.position - position;
        return Dot(diff, diff) <= r2;
    });
}

void PaintInstances(FoliageLayer& layer, const DensityMap& density,
                   const Vec3& center, f32 brush_radius, std::mt19937& rng) {
    if (layer.types.empty() || brush_radius <= 0.0f) return;
    // layer.density instances per square metre over the brush's disc, thinned by the density map.
    const f32 area = 3.14159265f * brush_radius * brush_radius;
    const f32 expected = std::max(0.0f, layer.density) * area;
    std::uniform_real_distribution<f32> chance(0.0f, 1.0f);
    u32 count = static_cast<u32>(expected);
    if (chance(rng) < expected - static_cast<f32>(count)) ++count; // the fraction, on average
    for (u32 i = 0; i < count && layer.instances.size() < layer.max_instances; ++i) {
        // Uniform over the disc.
        const f32 r = brush_radius * std::sqrt(chance(rng));
        const f32 a = chance(rng) * 2.0f * 3.14159265f;
        const Vec3 p(center.x + r * std::cos(a), center.y, center.z + r * std::sin(a));
        if (chance(rng) >= DensityAt(density, p)) continue;
        layer.instances.push_back(MakeInstance(layer, p, {0, 1, 0}, rng));
    }
}

// --- Filtering ---

std::vector<FoliageInstance> FilterByChunk(span<const FoliageInstance> instances,
                                           u32 chunk_x, u32 chunk_z) {
    std::vector<FoliageInstance> result;
    for (const FoliageInstance& inst : instances) {
        if (inst.chunk_x == chunk_x && inst.chunk_z == chunk_z && !inst.hidden) {
            result.push_back(inst);
        }
    }
    return result;
}

// --- Sorting ---

std::vector<u32> SortInstancesByType(span<const FoliageInstance> instances) {
    std::vector<u32> order(instances.size());
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(), [&](u32 a, u32 b) {
        return instances[a].type_index < instances[b].type_index;
    });
    return order;
}

// --- Bounds ---

void ComputeBounds(span<const FoliageInstance> instances,
                   Vec3* out_min, Vec3* out_max) {
    Vec3 mn{1e9f, 1e9f, 1e9f};
    Vec3 mx{-1e9f, -1e9f, -1e9f};
    for (const auto& inst : instances) {
        mn = Min(mn, inst.position);
        mx = Max(mx, inst.position);
    }
    *out_min = mn;
    *out_max = mx;
}

} // namespace terrain
} // namespace aether
