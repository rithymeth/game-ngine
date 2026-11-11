#include "aether/terrain/scatter.h"
#include "terrain_noise.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <unordered_map>

namespace aether {
namespace terrain {

namespace {

f32 Smoothstep(f32 a, f32 b, f32 x) {
    if (b <= a) return x >= b ? 1.0f : 0.0f;
    const f32 t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// A rotation that turns +Y onto `to` (unit).
Quaternion TiltFromUp(const Vec3& to) {
    const Vec3 up(0, 1, 0);
    const f32 c = std::clamp(up.Dot(to), -1.0f, 1.0f);
    if (c > 0.99999f) return Quaternion::Identity();
    Vec3 axis = up.Cross(to);
    if (axis.Length() < 1e-6f) axis = Vec3(1, 0, 0);
    return Quaternion::FromAxisAngle(axis.Normalized(), std::acos(c));
}

// A grid of placed footprints, for "is there room here".
class Footprints {
public:
    explicit Footprints(f32 cell) : cell_(std::max(cell, 0.25f)) {}
    void Add(f32 x, f32 z, f32 radius) {
        entries_.push_back({x, z, radius});
        grid_[Key(x, z)].push_back(static_cast<u32>(entries_.size() - 1));
        biggest_ = std::max(biggest_, radius);
    }
    // True when a circle of `radius` at (x, z) overlaps none by more than `overlap` (0..1) of the two radii together.
    bool Fits(f32 x, f32 z, f32 radius, f32 overlap) const {
        const f32 reach = radius + biggest_;
        const i32 span = static_cast<i32>(std::ceil(reach / cell_));
        const i32 cx = static_cast<i32>(std::floor(x / cell_)), cz = static_cast<i32>(std::floor(z / cell_));
        for (i32 oz = -span; oz <= span; ++oz) {
            for (i32 ox = -span; ox <= span; ++ox) {
                const auto it = grid_.find(Pack(cx + ox, cz + oz));
                if (it == grid_.end()) continue;
                for (const u32 i : it->second) {
                    const Entry& e = entries_[i];
                    const f32 dx = e.x - x, dz = e.z - z, min = (e.radius + radius) * (1.0f - overlap);
                    if (dx * dx + dz * dz < min * min) return false;
                }
            }
        }
        return true;
    }

private:
    struct Entry { f32 x, z, radius; };
    static u64 Pack(i32 x, i32 z) { return (static_cast<u64>(static_cast<u32>(x)) << 32) | static_cast<u32>(z); }
    u64 Key(f32 x, f32 z) const { return Pack(static_cast<i32>(std::floor(x / cell_)), static_cast<i32>(std::floor(z / cell_))); }
    f32 cell_;
    f32 biggest_ = 0.0f;
    std::vector<Entry> entries_;
    std::unordered_map<u64, std::vector<u32>> grid_;
};

u32 MixSeed(u32 seed, u32 rule) {
    u32 h = seed * 0x9E3779B1u ^ (rule + 1) * 0x85EBCA6Bu;
    h ^= h >> 15;
    h *= 0xC2B2AE35u;
    h ^= h >> 13;
    return h;
}

} // namespace

std::vector<f32> WaterDistanceField(const GeneratedTerrain& t) {
    const u32 w = t.Width(), h = t.Height();
    std::vector<f32> d(static_cast<usize>(w) * h, 1.0e9f);
    if (w == 0 || h == 0) return d;
    for (usize i = 0; i < d.size(); ++i) {
        const bool water = t.biome[i] == Biome::Water || (i < t.river.size() && t.river[i]) || (i < t.lake.size() && t.lake[i]);
        if (water) d[i] = 0.0f;
    }
    // Two-pass chamfer distance (1 and sqrt(2) per step), in cells, then metres.
    const f32 diagonal = 1.41421356f;
    auto relax = [&](u32 x, u32 z, i32 ox, i32 oz, f32 step) {
        const i32 nx = static_cast<i32>(x) + ox, nz = static_cast<i32>(z) + oz;
        if (nx < 0 || nz < 0 || nx >= static_cast<i32>(w) || nz >= static_cast<i32>(h)) return;
        const f32 candidate = d[static_cast<usize>(nz) * w + static_cast<usize>(nx)] + step;
        f32& here = d[static_cast<usize>(z) * w + x];
        here = std::min(here, candidate);
    };
    for (u32 z = 0; z < h; ++z)
        for (u32 x = 0; x < w; ++x) { relax(x, z, -1, 0, 1); relax(x, z, 0, -1, 1); relax(x, z, -1, -1, diagonal); relax(x, z, 1, -1, diagonal); }
    for (u32 z = h; z-- > 0;)
        for (u32 x = w; x-- > 0;) { relax(x, z, 1, 0, 1); relax(x, z, 0, 1, 1); relax(x, z, 1, 1, diagonal); relax(x, z, -1, 1, diagonal); }
    for (f32& v : d) v = v >= 1.0e8f ? v : v * t.heightmap.cell_size;
    return d;
}

Vec3 TerrainNormalAt(const Heightmap& hm, f32 x, f32 z) {
    const f32 e = std::max(hm.cell_size, 1e-3f);
    const f32 dx = hm.SampleLinear(x + e, z) - hm.SampleLinear(x - e, z);
    const f32 dz = hm.SampleLinear(x, z + e) - hm.SampleLinear(x, z - e);
    return Vec3(-dx / (2.0f * e), 1.0f, -dz / (2.0f * e)).Normalized();
}

std::vector<ScatterInstance> Scatter(const GeneratedTerrain& t, const TerrainGenSettings& requested, std::span<const ScatterRule> rules,
                                     const ScatterSettings& settings) {
    std::vector<ScatterInstance> out;
    const u32 w = t.Width(), h = t.Height();
    if (w < 2 || h < 2 || rules.empty() || t.biome.size() != static_cast<usize>(w) * h) return out;
    const TerrainGenSettings s = SanitizeSettings(requested);
    const Heightmap& hm = t.heightmap;
    const f32 cell = hm.cell_size;
    const f32 size_x = static_cast<f32>(w - 1) * cell, size_z = static_cast<f32>(h - 1) * cell;
    const f32 range = std::max(t.height_max - t.height_min, 1e-4f);
    const std::vector<f32> water = WaterDistanceField(t);

    f32 biggest = 0.0f;
    for (const ScatterRule& r : rules) biggest = std::max(biggest, r.footprint * std::max(r.scale_max, 1.0f));
    Footprints taken(std::max(biggest * 2.0f, 1.0f));

    for (u32 ri = 0; ri < rules.size() && out.size() < settings.max_total; ++ri) {
        const ScatterRule& rule = rules[ri];
        // Each rule draws from its own stream, so adding a rule after this one cannot move this one's instances.
        std::mt19937 rng(MixSeed(settings.seed, ri));
        std::uniform_real_distribution<f32> unit(0.0f, 1.0f);
        const f32 density = std::max(rule.density, 0.0f);
        const u32 clump = std::max(rule.clump_size, 1u);
        const f64 candidates_f = static_cast<f64>(density) * static_cast<f64>(size_x) * static_cast<f64>(size_z) / 100.0 / static_cast<f64>(clump);
        const u64 parents = static_cast<u64>(std::min(candidates_f, 5.0e7));
        const f32 scale_lo = std::min(rule.scale_min, rule.scale_max), scale_hi = std::max(rule.scale_min, rule.scale_max);
        u32 placed = 0;

        // Tries one position: false when the rules or the room refuse it.
        auto try_place = [&](f32 x, f32 z) {
            if (x < 0.0f || z < 0.0f || x > size_x || z > size_z) return false;
            const u32 cx = std::min(static_cast<u32>(x / cell + 0.5f), w - 1), cz = std::min(static_cast<u32>(z / cell + 0.5f), h - 1);
            const usize ci = static_cast<usize>(cz) * w + cx;
            const Biome biome = t.biome[ci];
            if (!(rule.biomes & BiomeBit(biome))) return false;
            if (biome == Biome::Water) return false; // nothing stands in water
            const f32 ground = hm.SampleLinear(x, z);
            const f32 normalized = (ground - t.height_min) / range;
            if (normalized < rule.height_min || normalized > rule.height_max) return false;
            const Vec3 normal = TerrainNormalAt(hm, x, z);
            const f32 slope = std::sqrt(std::max(1.0f - normal.y * normal.y, 0.0f)) / std::max(normal.y, 1e-4f);
            if (slope < rule.slope_min || slope > rule.slope_max) return false;
            if (water[ci] < rule.water_distance_min || water[ci] > rule.water_distance_max) return false;
            if (rule.patch_scale > 0.0f) {
                const f32 n = FractalNoise(x * rule.patch_scale / 100.0f, z * rule.patch_scale / 100.0f, 3, 0.5f, MixSeed(settings.seed ^ 0x51ED270Bu, ri)) * 0.5f + 0.5f;
                if (unit(rng) > Smoothstep(rule.patch_threshold - 0.08f, rule.patch_threshold + 0.08f, n)) return false;
            }
            f32 scale = scale_lo + (scale_hi - scale_lo) * unit(rng);
            if (rule.scale_steep_bias > 0.0f) scale *= 1.0f - std::clamp(rule.scale_steep_bias, 0.0f, 1.0f) * Smoothstep(0.0f, std::max(rule.slope_max, 1e-3f), slope) * 0.5f;
            const f32 radius = std::max(rule.footprint, 0.0f) * scale;
            if (!taken.Fits(x, z, radius, std::clamp(rule.overlap, 0.0f, 1.0f))) return false;
            taken.Add(x, z, radius);

            ScatterInstance inst;
            inst.position = Vec3(x, ground + rule.y_offset, z);
            inst.normal = normal;
            inst.scale = scale;
            inst.rule = ri;
            inst.biome = biome;
            const f32 yaw = rule.random_yaw ? (unit(rng) * 2.0f - 1.0f) * 3.14159265f : 0.0f;
            const f32 lean = std::clamp(rule.align_to_normal, 0.0f, 1.0f);
            const Vec3 up = (Vec3(0, 1, 0) * (1.0f - lean) + normal * lean).Normalized();
            inst.rotation = TiltFromUp(up) * Quaternion::FromAxisAngle(Vec3(0, 1, 0), yaw);
            out.push_back(inst);
            ++placed;
            return true;
        };

        for (u64 p = 0; p < parents && placed < rule.max_instances && out.size() < settings.max_total; ++p) {
            const f32 px = unit(rng) * size_x, pz = unit(rng) * size_z;
            const bool parent_ok = try_place(px, pz);
            if (clump <= 1 || !parent_ok) continue;
            for (u32 c = 1; c < clump && placed < rule.max_instances && out.size() < settings.max_total; ++c) {
                // Around the parent, denser toward it (the square root of a uniform radius would be even; this is not).
                const f32 angle = unit(rng) * 6.2831853f, radius = unit(rng) * unit(rng) * std::max(rule.clump_radius, 0.0f);
                try_place(px + std::cos(angle) * radius, pz + std::sin(angle) * radius);
            }
        }
    }
    (void)s;
    return out;
}

std::vector<ScatterRule> DefaultScatterRules() {
    std::vector<ScatterRule> r;
    ScatterRule pine;
    pine.name = "Pine";
    pine.biomes = BiomeBit(Biome::Forest);
    pine.slope_max = 0.65f;
    pine.height_max = 0.6f;
    pine.density = 4.0f;
    pine.patch_scale = 3.0f;
    pine.patch_threshold = 0.38f;
    pine.scale_min = 0.8f;
    pine.scale_max = 1.5f;
    pine.scale_steep_bias = 0.4f;
    pine.footprint = 1.6f;
    pine.overlap = 0.25f;
    r.push_back(pine);

    ScatterRule birch = pine;
    birch.name = "Birch";
    birch.biomes = BiomeBit(Biome::Forest) | BiomeBit(Biome::Grassland);
    birch.density = 0.5f;
    birch.clump_size = 4;
    birch.clump_radius = 5.0f;
    birch.patch_scale = 4.0f;
    birch.patch_threshold = 0.55f;
    birch.scale_min = 0.7f;
    birch.scale_max = 1.2f;
    birch.footprint = 1.2f;
    r.push_back(birch);

    ScatterRule shrub;
    shrub.name = "Shrub";
    shrub.biomes = BiomeBit(Biome::Forest) | BiomeBit(Biome::Grassland);
    shrub.slope_max = 0.8f;
    shrub.density = 4.0f;
    shrub.scale_min = 0.6f;
    shrub.scale_max = 1.3f;
    shrub.footprint = 0.7f;
    shrub.overlap = 0.4f;
    shrub.align_to_normal = 0.3f;
    r.push_back(shrub);

    ScatterRule grass;
    grass.name = "Grass";
    grass.biomes = BiomeBit(Biome::Grassland) | BiomeBit(Biome::Forest);
    grass.slope_max = 0.9f;
    grass.density = 40.0f;
    grass.scale_min = 0.7f;
    grass.scale_max = 1.3f;
    grass.footprint = 0.25f;
    grass.overlap = 0.8f;
    grass.align_to_normal = 0.6f;
    grass.max_instances = 400000;
    r.push_back(grass);

    ScatterRule boulder;
    boulder.name = "Boulder";
    boulder.biomes = BiomeBit(Biome::Rock) | BiomeBit(Biome::Grassland) | BiomeBit(Biome::Desert) | BiomeBit(Biome::Forest);
    boulder.slope_max = 1.6f;
    boulder.density = 0.35f;
    boulder.clump_size = 5;
    boulder.clump_radius = 4.0f;
    boulder.scale_min = 0.5f;
    boulder.scale_max = 2.2f;
    boulder.footprint = 0.9f;
    boulder.overlap = 0.3f;
    boulder.align_to_normal = 0.9f;
    boulder.y_offset = -0.15f;
    r.push_back(boulder);

    ScatterRule reeds;
    reeds.name = "Reeds";
    reeds.biomes = BiomeBit(Biome::Grassland) | BiomeBit(Biome::Beach) | BiomeBit(Biome::Forest);
    reeds.slope_max = 0.35f;
    reeds.water_distance_max = 6.0f;
    reeds.density = 12.0f;
    reeds.clump_size = 6;
    reeds.clump_radius = 1.5f;
    reeds.scale_min = 0.8f;
    reeds.scale_max = 1.4f;
    reeds.footprint = 0.2f;
    reeds.overlap = 0.8f;
    r.push_back(reeds);

    ScatterRule dead_tree;
    dead_tree.name = "DeadTree";
    dead_tree.biomes = BiomeBit(Biome::Desert);
    dead_tree.slope_max = 0.5f;
    dead_tree.density = 0.15f;
    dead_tree.scale_min = 0.8f;
    dead_tree.scale_max = 1.3f;
    dead_tree.footprint = 1.0f;
    r.push_back(dead_tree);

    ScatterRule cactus;
    cactus.name = "Cactus";
    cactus.biomes = BiomeBit(Biome::Desert);
    cactus.slope_max = 0.4f;
    cactus.density = 0.5f;
    cactus.scale_min = 0.6f;
    cactus.scale_max = 1.5f;
    cactus.footprint = 0.6f;
    cactus.patch_scale = 5.0f;
    cactus.patch_threshold = 0.5f;
    r.push_back(cactus);

    ScatterRule snow_rock;
    snow_rock.name = "SnowRock";
    snow_rock.biomes = BiomeBit(Biome::Snow) | BiomeBit(Biome::Rock);
    snow_rock.height_min = 0.7f;
    snow_rock.slope_max = 1.8f;
    snow_rock.density = 0.4f;
    snow_rock.clump_size = 3;
    snow_rock.clump_radius = 3.0f;
    snow_rock.scale_min = 0.6f;
    snow_rock.scale_max = 1.8f;
    snow_rock.footprint = 0.8f;
    snow_rock.align_to_normal = 0.9f;
    snow_rock.y_offset = -0.2f;
    r.push_back(snow_rock);
    return r;
}

std::vector<FoliageInstance> ToFoliageInstances(std::span<const ScatterInstance> instances, f32 chunk_world_size) {
    std::vector<FoliageInstance> out;
    out.reserve(instances.size());
    const f32 chunk = std::max(chunk_world_size, 1e-3f);
    for (const ScatterInstance& s : instances) {
        FoliageInstance f;
        f.position = s.position;
        f.normal = s.normal;
        f.scale = Vec3(s.scale, s.scale, s.scale);
        f.rotation = s.rotation;
        f.type_index = s.rule;
        f.chunk_x = static_cast<u16>(std::clamp(std::floor(s.position.x / chunk), 0.0f, 65535.0f));
        f.chunk_z = static_cast<u16>(std::clamp(std::floor(s.position.z / chunk), 0.0f, 65535.0f));
        out.push_back(f);
    }
    return out;
}

void OverlayScatterPreview(std::vector<u8>& rgb, const GeneratedTerrain& t, std::span<const ScatterInstance> instances) {
    const u32 w = t.Width(), h = t.Height();
    if (rgb.size() < static_cast<usize>(w) * h * 3 || w == 0 || h == 0) return;
    const Vec3 palette[] = {Vec3(0.02f, 0.18f, 0.06f), Vec3(0.75f, 0.9f, 0.5f), Vec3(0.2f, 0.5f, 0.12f), Vec3(0.5f, 0.75f, 0.2f), Vec3(0.2f, 0.2f, 0.22f),
                            Vec3(0.9f, 0.8f, 0.3f), Vec3(0.35f, 0.2f, 0.1f), Vec3(0.1f, 0.6f, 0.2f), Vec3(1.0f, 1.0f, 1.0f)};
    const f32 cell = t.heightmap.cell_size;
    // Latest rule first, so the earlier rules (the trees) end up on top of the grass between them.
    std::vector<const ScatterInstance*> order;
    order.reserve(instances.size());
    for (const ScatterInstance& s : instances) order.push_back(&s);
    std::stable_sort(order.begin(), order.end(), [](const ScatterInstance* a, const ScatterInstance* b) { return a->rule > b->rule; });
    for (const ScatterInstance* p : order) {
        const ScatterInstance& s = *p;
        const i32 x = static_cast<i32>(s.position.x / cell + 0.5f), z = static_cast<i32>(s.position.z / cell + 0.5f);
        if (x < 0 || z < 0 || x >= static_cast<i32>(w) || z >= static_cast<i32>(h)) continue;
        const Vec3 c = palette[s.rule % (sizeof(palette) / sizeof(palette[0]))];
        const usize i = (static_cast<usize>(z) * w + static_cast<usize>(x)) * 3;
        rgb[i] = static_cast<u8>(c.x * 255.0f);
        rgb[i + 1] = static_cast<u8>(c.y * 255.0f);
        rgb[i + 2] = static_cast<u8>(c.z * 255.0f);
    }
}

} // namespace terrain
} // namespace aether
