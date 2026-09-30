#include ""aether/terrain/foliage.h""

#include <algorithm>
#include <cmath>
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

// --- Noise-based density ---

namespace {
f32 SmoothNoise(i32 x, i32 z, i32 seed) {
    // Simple value noise.
    i32 n = x + z * 57 + seed * 31337;
    n = (n << 13) ^ n;
    n = n * (n * n * 15731 + 789221) + 1376312589;
    const i32 h = n & 0x0fffffff;
    return static_cast<f32>(h ^ 0x40000000) / 2147483648.0f; // -1..1
}

f32 ValueNoise(f32 x, f32 z, i32 seed) {
    const i32 ix = static_cast<i32>(std::floor(x));
    const i32 iz = static_cast<i32>(std::floor(z));
    const f32 fx = x - static_cast<f32>(ix);
    const f32 fz = z - static_cast<f32>(iz);
    const f32 sx = fx * fx * (3.0f - 2.0f * fx);
    const f32 sz = fz * fz * (3.0f - 2.0f * fz);

    const f32 n00 = SmoothNoise(ix,     iz,     seed);
    const f32 n10 = SmoothNoise(ix + 1, iz,     seed);
    const f32 n01 = SmoothNoise(ix,     iz + 1, seed);
    const f32 n11 = SmoothNoise(ix + 1, iz + 1, seed);

    const f32 nx0 = n00 + (n10 - n00) * sx;
    const f32 nx1 = n01 + (n11 - n01) * sx;
    return nx0 + (nx1 - nx0) * sz; // -1..1
}

f32 FractalNoise(f32 x, f32 z, u32 octaves, f32 persistence, i32 seed) {
    f32 val = 0.0f;
    f32 amp = 1.0f;
    f32 freq = 1.0f;
    f32 max_val = 0.0f;
    for (u32 i = 0; i < octaves; ++i) {
        val += amp * ValueNoise(x * freq, z * freq, seed + static_cast<i32>(i * 31337));
        max_val += amp;
        amp *= persistence;
        freq *= 2.0f;
    }
    return (val / max_val + 1.0f) * 0.5f; // 0..1
}
} // namespace

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
            const f32 n = FractalNoise(wx * 0.05f, wz * 0.05f, octaves, persistence,
                                       static_cast<i32>(seed * 17.0f + 3.0f));
            dm.cells[static_cast<usize>(z) * width + x] =
                static_cast<u8>(std::clamp(n, 0.0f, 1.0f) * 255.0f);
        }
    }
    return dm;
}

// --- Instance generation ---

void GenerateInstances(FoliageLayer& layer, const DensityMap& density,
                     span<const Vec3> sample_positions,
                     span<const Vec3> sample_normals,
                     std::mt19937& rng) {
    if (sample_positions.size() != sample_normals.size()) return;
    layer.instances.clear();
    layer.instances.reserve(sample_positions.size());

    std::uniform_real_distribution<f32> scale_dist(layer.types[0].scale_min,
                                                  layer.types[0].scale_max);

    for (usize i = 0; i < sample_positions.size(); ++i) {
        const Vec3& pos = sample_positions[i];
        const Vec3& normal = sample_normals[i];

        FoliageInstance inst;
        inst.position = pos;
        inst.normal = normal;
        inst.type_index = 0; // first type
        inst.scale = RandomScale(rng, layer.types[0]);
        inst.rotation = RandomRotation(rng, layer.types[0]);
        inst.hidden = false;
        layer.instances.push_back(inst);
    }
}

// --- Brush operations ---

void AddInstance(FoliageLayer& layer, const Vec3& position,
                const Vec3& normal, std::mt19937& rng) {
    if (layer.instances.size() >= layer.max_instances) return;
    FoliageInstance inst;
    inst.position = position;
    inst.normal = normal;
    inst.type_index = 0;
    if (!layer.types.empty()) {
        inst.scale = RandomScale(rng, layer.types[0]);
        inst.rotation = RandomRotation(rng, layer.types[0]);
    }
    inst.hidden = false;
    layer.instances.push_back(inst);
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
    // For now: just add instances at brush center (full density = add, no density = nothing).
    // The density map brush painting comes when we have per-cell data.
    AddInstance(layer, center, {0, 1, 0}, rng);
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
