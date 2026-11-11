#include "aether/terrain/generator.h"
#include "terrain_noise.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <queue>
#include <random>

namespace aether {
namespace terrain {

namespace {

constexpr i32 kDx[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
constexpr i32 kDz[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
constexpr f32 kDistance[8] = {1.41421356f, 1.0f, 1.41421356f, 1.0f, 1.0f, 1.41421356f, 1.0f, 1.41421356f};

f32 Smoothstep(f32 a, f32 b, f32 x) {
    if (b <= a) return x >= b ? 1.0f : 0.0f;
    const f32 t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

struct Grid {
    u32 w = 0, h = 0;
    bool In(i32 x, i32 z) const { return x >= 0 && z >= 0 && x < static_cast<i32>(w) && z < static_cast<i32>(h); }
    usize At(u32 x, u32 z) const { return static_cast<usize>(z) * w + x; }
};

// Ridged noise: 1 - |n|, squared so the crests are sharp, summed over octaves; 0..1.
f32 RidgedNoise(f32 x, f32 z, u32 octaves, f32 persistence, u32 seed) {
    f32 sum = 0.0f, amp = 1.0f, freq = 1.0f, norm = 0.0f, weight = 1.0f;
    for (u32 o = 0; o < std::max(octaves, 1u); ++o) {
        f32 n = 1.0f - std::fabs(ValueNoise(x * freq, z * freq, seed + o * 0x85EBCA6Bu));
        n *= n;
        n *= weight; // crests feed the next octave, so detail piles up on the ridges and not in the valleys
        weight = std::clamp(n * 2.0f, 0.0f, 1.0f);
        sum += n * amp;
        norm += amp;
        amp *= persistence;
        freq *= 2.0f;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

void Shape(const TerrainGenSettings& s, std::vector<f32>& out) {
    const Grid g{s.width, s.height};
    out.assign(static_cast<usize>(s.width) * s.height, 0.0f);
    for (u32 z = 0; z < s.height; ++z) {
        for (u32 x = 0; x < s.width; ++x) {
            const f32 u = static_cast<f32>(x) / static_cast<f32>(s.width - 1);
            const f32 v = static_cast<f32>(z) / static_cast<f32>(s.height - 1);
            f32 px = u * s.frequency, pz = v * s.frequency;
            if (s.warp > 0.0f) {
                const f32 wx = FractalNoise(px * 0.7f + 11.3f, pz * 0.7f + 4.7f, 3, 0.5f, s.seed ^ 0xA511E9B3u);
                const f32 wz = FractalNoise(px * 0.7f - 7.1f, pz * 0.7f + 19.9f, 3, 0.5f, s.seed ^ 0x63D83595u);
                px += wx * s.warp * s.frequency;
                pz += wz * s.warp * s.frequency;
            }
            const f32 rolling = FractalNoise(px, pz, s.octaves, s.persistence, s.seed) * 0.5f + 0.5f;
            const f32 ridged = RidgedNoise(px * 0.8f, pz * 0.8f, s.octaves, s.persistence, s.seed ^ 0x1B873593u);
            // Where the rolling base is high the ridges rise through it: the mountains stand on the uplands, not in the lowlands.
            const f32 mountain = ridged * Smoothstep(0.25f, 0.75f, rolling);
            out[g.At(x, z)] = rolling * (1.0f - s.ridge_amount) + (rolling * 0.35f + mountain * 0.65f) * s.ridge_amount;
        }
    }
    const auto [lo, hi] = std::minmax_element(out.begin(), out.end());
    const f32 low = *lo, range = std::max(*hi - *lo, 1e-6f);
    for (f32& v : out) v = std::pow((v - low) / range, std::max(s.lowland_power, 0.1f)) * s.height_scale;
}

void Thermal(const TerrainGenSettings& s, std::vector<f32>& h) {
    const Grid g{s.width, s.height};
    std::vector<f32> next;
    for (u32 it = 0; it < s.thermal_iterations; ++it) {
        next = h;
        for (u32 z = 0; z < s.height; ++z) {
            for (u32 x = 0; x < s.width; ++x) {
                const usize i = g.At(x, z);
                f32 excess[8];
                f32 total = 0.0f, biggest = 0.0f;
                for (int k = 0; k < 8; ++k) {
                    excess[k] = 0.0f;
                    const i32 nx = static_cast<i32>(x) + kDx[k], nz = static_cast<i32>(z) + kDz[k];
                    if (!g.In(nx, nz)) continue;
                    const f32 drop = h[i] - h[g.At(static_cast<u32>(nx), static_cast<u32>(nz))];
                    const f32 allowed = s.talus * s.cell_size * kDistance[k];
                    if (drop > allowed) {
                        excess[k] = drop - allowed;
                        total += excess[k];
                        biggest = std::max(biggest, excess[k]);
                    }
                }
                if (total <= 0.0f) continue;
                const f32 moved = 0.5f * biggest; // half of the worst overhang slides, split by how far each neighbour is below its limit
                for (int k = 0; k < 8; ++k) {
                    if (excess[k] <= 0.0f) continue;
                    const f32 share = moved * excess[k] / total;
                    next[i] -= share;
                    next[g.At(static_cast<u32>(static_cast<i32>(x) + kDx[k]), static_cast<u32>(static_cast<i32>(z) + kDz[k]))] += share;
                }
            }
        }
        h.swap(next);
    }
}

// Height and gradient (metres per metre) at a fractional cell position, bilinear.
struct Sample { f32 height, gx, gz; };
Sample SampleHeight(const Grid& g, const std::vector<f32>& h, f32 px, f32 pz, f32 cell) {
    const i32 x0 = static_cast<i32>(px), z0 = static_cast<i32>(pz);
    const f32 fx = px - static_cast<f32>(x0), fz = pz - static_cast<f32>(z0);
    const f32 h00 = h[g.At(static_cast<u32>(x0), static_cast<u32>(z0))], h10 = h[g.At(static_cast<u32>(x0) + 1, static_cast<u32>(z0))];
    const f32 h01 = h[g.At(static_cast<u32>(x0), static_cast<u32>(z0) + 1)], h11 = h[g.At(static_cast<u32>(x0) + 1, static_cast<u32>(z0) + 1)];
    return {h00 * (1 - fx) * (1 - fz) + h10 * fx * (1 - fz) + h01 * (1 - fx) * fz + h11 * fx * fz,
            ((h10 - h00) * (1 - fz) + (h11 - h01) * fz) / cell, ((h01 - h00) * (1 - fx) + (h11 - h10) * fx) / cell};
}

void AddAt(const Grid& g, std::vector<f32>& h, i32 x0, i32 z0, f32 fx, f32 fz, f32 amount) {
    h[g.At(static_cast<u32>(x0), static_cast<u32>(z0))] += amount * (1 - fx) * (1 - fz);
    h[g.At(static_cast<u32>(x0) + 1, static_cast<u32>(z0))] += amount * fx * (1 - fz);
    h[g.At(static_cast<u32>(x0), static_cast<u32>(z0) + 1)] += amount * (1 - fx) * fz;
    h[g.At(static_cast<u32>(x0) + 1, static_cast<u32>(z0) + 1)] += amount * fx * fz;
}

// Takes `amount` of earth from a disc round the cell nearest (px, pz), weighted toward its centre (a cone, radius 3 cells) and normalised over
// the cells that are on the map, so the total taken is exactly `amount`. Digging at one cell (or four) leaves a pit and a spike for every
// droplet; spread over a disc each droplet only leaves a shallow scoop.
void ErodeDisc(const Grid& g, std::vector<f32>& h, f32 px, f32 pz, f32 amount) {
    constexpr i32 radius = 3;
    const i32 cx = static_cast<i32>(px + 0.5f), cz = static_cast<i32>(pz + 0.5f);
    f32 weights[(2 * radius + 1) * (2 * radius + 1)];
    f32 total = 0.0f;
    i32 k = 0;
    for (i32 oz = -radius; oz <= radius; ++oz) {
        for (i32 ox = -radius; ox <= radius; ++ox, ++k) {
            const f32 w = g.In(cx + ox, cz + oz) ? std::max(0.0f, static_cast<f32>(radius) - std::sqrt(static_cast<f32>(ox * ox + oz * oz))) : 0.0f;
            weights[k] = w;
            total += w;
        }
    }
    if (total <= 0.0f) return;
    k = 0;
    for (i32 oz = -radius; oz <= radius; ++oz)
        for (i32 ox = -radius; ox <= radius; ++ox, ++k)
            if (weights[k] > 0.0f) h[g.At(static_cast<u32>(cx + ox), static_cast<u32>(cz + oz))] -= amount * weights[k] / total;
}

// Particle erosion: a droplet follows the slope (with a little inertia), carries sediment up to a capacity that grows with speed, water and
// slope, takes from the ground when under capacity and gives back when over it or when it climbs.
void Hydraulic(const TerrainGenSettings& s, std::vector<f32>& h) {
    const Grid g{s.width, s.height};
    std::mt19937 rng(s.seed ^ 0x9E3779B1u);
    std::uniform_real_distribution<f32> unit(0.0f, 1.0f);
    // Capacity is in metres of earth per metre of slope: 4 (the usual figure for a 0..1 height range) carries metres per droplet on a 60 m
    // terrain and leaves needles; 0.5 leaves the roughness near the shaped terrain's own and still cuts gullies.
    constexpr f32 inertia = 0.05f, capacity_factor = 0.5f, min_capacity = 0.01f, erode_rate = 0.3f, deposit_rate = 0.3f, evaporate = 0.015f,
                  gravity = 4.0f;
    constexpr u32 lifetime = 40;
    const f32 max_x = static_cast<f32>(s.width - 1) - 1e-3f, max_z = static_cast<f32>(s.height - 1) - 1e-3f;
    for (u32 d = 0; d < s.hydraulic_droplets; ++d) {
        f32 px = unit(rng) * max_x, pz = unit(rng) * max_z, dx = 0, dz = 0, speed = 1.0f, water = 1.0f, sediment = 0.0f;
        for (u32 step = 0; step < lifetime; ++step) {
            const i32 x0 = static_cast<i32>(px), z0 = static_cast<i32>(pz);
            const f32 fx = px - static_cast<f32>(x0), fz = pz - static_cast<f32>(z0);
            const Sample here = SampleHeight(g, h, px, pz, s.cell_size);
            dx = dx * inertia - here.gx * (1.0f - inertia);
            dz = dz * inertia - here.gz * (1.0f - inertia);
            const f32 len = std::sqrt(dx * dx + dz * dz);
            if (len < 1e-6f) break;
            dx /= len;
            dz /= len;
            const f32 nx = px + dx, nz = pz + dz;
            if (nx < 0.0f || nz < 0.0f || nx > max_x || nz > max_z) break;
            const f32 drop = SampleHeight(g, h, nx, nz, s.cell_size).height - here.height; // negative downhill
            const f32 capacity = std::max(-drop, min_capacity) * speed * water * capacity_factor;
            if (sediment > capacity || drop > 0.0f) {
                // Uphill, fill the step we just left up to the height of the new one; otherwise drop a share of the excess.
                const f32 amount = drop > 0.0f ? std::min(drop, sediment) : (sediment - capacity) * deposit_rate;
                sediment -= amount;
                AddAt(g, h, x0, z0, fx, fz, amount);
            } else {
                const f32 amount = std::min((capacity - sediment) * erode_rate, -drop);
                sediment += amount;
                ErodeDisc(g, h, px, pz, amount);
            }
            speed = std::sqrt(std::max(speed * speed - drop * gravity, 0.0f));
            water *= 1.0f - evaporate;
            px = nx;
            pz = nz;
        }
        // A droplet that stops (flat ground, the map's edge, the end of its life) sets down what it carries, so erosion moves earth and
        // does not make it vanish.
        if (sediment > 0.0f) {
            const i32 x0 = static_cast<i32>(px), z0 = static_cast<i32>(pz);
            AddAt(g, h, x0, z0, px - static_cast<f32>(x0), pz - static_cast<f32>(z0), sediment);
        }
    }
}

// Priority-flood (Barnes et al.): raises every depression to its spill point, plus a hair per step so each cell has a strictly lower
// neighbour to drain to. The edge of the map is the outlet.
void FillDepressions(const Grid& g, const std::vector<f32>& h, std::vector<f32>& filled) {
    filled = h;
    std::vector<u8> done(h.size(), 0);
    using Entry = std::pair<f32, usize>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
    for (u32 z = 0; z < g.h; ++z) {
        for (u32 x = 0; x < g.w; ++x) {
            if (x != 0 && z != 0 && x != g.w - 1 && z != g.h - 1) continue;
            open.push({filled[g.At(x, z)], g.At(x, z)});
            done[g.At(x, z)] = 1;
        }
    }
    constexpr f32 epsilon = 1e-4f;
    while (!open.empty()) {
        const auto [level, i] = open.top();
        open.pop();
        const i32 x = static_cast<i32>(i % g.w), z = static_cast<i32>(i / g.w);
        for (int k = 0; k < 8; ++k) {
            const i32 nx = x + kDx[k], nz = z + kDz[k];
            if (!g.In(nx, nz)) continue;
            const usize n = g.At(static_cast<u32>(nx), static_cast<u32>(nz));
            if (done[n]) continue;
            done[n] = 1;
            filled[n] = std::max(filled[n], level + epsilon);
            open.push({filled[n], n});
        }
    }
}

// Steepest descent on the filled surface; -1 for a cell on the map's edge (the outlet).
void FlowDirections(const Grid& g, const std::vector<f32>& filled, std::vector<i32>& next) {
    next.assign(filled.size(), -1);
    for (u32 z = 0; z < g.h; ++z) {
        for (u32 x = 0; x < g.w; ++x) {
            if (x == 0 || z == 0 || x == g.w - 1 || z == g.h - 1) continue;
            const usize i = g.At(x, z);
            f32 best = 0.0f;
            for (int k = 0; k < 8; ++k) {
                const usize n = g.At(static_cast<u32>(static_cast<i32>(x) + kDx[k]), static_cast<u32>(static_cast<i32>(z) + kDz[k]));
                const f32 slope = (filled[i] - filled[n]) / kDistance[k];
                if (slope > best) {
                    best = slope;
                    next[i] = static_cast<i32>(n);
                }
            }
        }
    }
}

void Accumulate(const std::vector<f32>& filled, const std::vector<i32>& next, std::vector<f32>& flow) {
    flow.assign(filled.size(), 1.0f);
    std::vector<usize> order(filled.size());
    std::iota(order.begin(), order.end(), usize{0});
    // Highest first, ties by index: the same order on every machine.
    std::sort(order.begin(), order.end(), [&](usize a, usize b) { return filled[a] != filled[b] ? filled[a] > filled[b] : a < b; });
    for (const usize i : order) {
        if (next[i] >= 0) flow[static_cast<usize>(next[i])] += flow[i];
    }
}

void Slopes(const TerrainGenSettings& s, const std::vector<f32>& h, std::vector<f32>& slope) {
    const Grid g{s.width, s.height};
    slope.assign(h.size(), 0.0f);
    for (u32 z = 0; z < s.height; ++z) {
        for (u32 x = 0; x < s.width; ++x) {
            const u32 xl = x > 0 ? x - 1 : x, xr = x + 1 < s.width ? x + 1 : x, zu = z > 0 ? z - 1 : z, zd = z + 1 < s.height ? z + 1 : z;
            const f32 gx = (h[g.At(xr, z)] - h[g.At(xl, z)]) / (static_cast<f32>(xr - xl) * s.cell_size);
            const f32 gz = (h[g.At(x, zd)] - h[g.At(x, zu)]) / (static_cast<f32>(zd - zu) * s.cell_size);
            slope[g.At(x, z)] = std::sqrt(gx * gx + gz * gz);
        }
    }
}

void BoxBlur(const Grid& g, std::vector<f32>& v, u32 passes) {
    std::vector<f32> t(v.size());
    for (u32 p = 0; p < passes; ++p) {
        for (u32 z = 0; z < g.h; ++z) {
            for (u32 x = 0; x < g.w; ++x) {
                f32 sum = 0.0f;
                u32 n = 0;
                for (i32 oz = -1; oz <= 1; ++oz) {
                    for (i32 ox = -1; ox <= 1; ++ox) {
                        const i32 nx = static_cast<i32>(x) + ox, nz = static_cast<i32>(z) + oz;
                        if (!g.In(nx, nz)) continue;
                        sum += v[g.At(static_cast<u32>(nx), static_cast<u32>(nz))];
                        ++n;
                    }
                }
                t[g.At(x, z)] = sum / static_cast<f32>(n);
            }
        }
        v.swap(t);
    }
}

} // namespace

const char* BiomeName(Biome biome) {
    switch (biome) {
    case Biome::Water: return "Water";
    case Biome::Beach: return "Beach";
    case Biome::Desert: return "Desert";
    case Biome::Grassland: return "Grassland";
    case Biome::Forest: return "Forest";
    case Biome::Rock: return "Rock";
    case Biome::Snow: return "Snow";
    }
    return "Unknown";
}

TerrainGenSettings SanitizeSettings(const TerrainGenSettings& in) {
    TerrainGenSettings s = in;
    auto finite = [](f32 v, f32 fallback) { return std::isfinite(v) ? v : fallback; };
    s.width = std::clamp(s.width, 8u, 4097u);
    s.height = std::clamp(s.height, 8u, 4097u);
    s.cell_size = std::clamp(finite(s.cell_size, 1.0f), 0.01f, 1000.0f);
    s.height_scale = std::clamp(finite(s.height_scale, 60.0f), 0.1f, 20000.0f);
    s.frequency = std::clamp(finite(s.frequency, 2.5f), 0.1f, 64.0f);
    s.octaves = std::clamp(s.octaves, 1u, 12u);
    s.persistence = std::clamp(finite(s.persistence, 0.5f), 0.1f, 0.95f);
    s.ridge_amount = std::clamp(finite(s.ridge_amount, 0.55f), 0.0f, 1.0f);
    s.warp = std::clamp(finite(s.warp, 0.25f), 0.0f, 1.0f);
    s.lowland_power = std::clamp(finite(s.lowland_power, 1.4f), 0.2f, 4.0f);
    s.smoothing = std::min(s.smoothing, 16u);
    s.thermal_iterations = std::min(s.thermal_iterations, 500u);
    s.talus = std::clamp(finite(s.talus, 0.8f), 0.05f, 10.0f);
    s.hydraulic_droplets = std::min(s.hydraulic_droplets, 5'000'000u);
    s.river_min_catchment = std::max(s.river_min_catchment, 2u);
    s.river_depth = std::clamp(finite(s.river_depth, 0.04f), 0.0f, 0.5f);
    s.lake_depth = std::clamp(finite(s.lake_depth, 0.25f), 0.0f, 1000.0f);
    s.sea_level = std::clamp(finite(s.sea_level, 0.18f), 0.0f, 0.95f);
    s.beach_band = std::clamp(finite(s.beach_band, 0.03f), 0.0f, 0.5f);
    s.tree_line = std::clamp(finite(s.tree_line, 0.62f), 0.0f, 1.0f);
    s.snow_line = std::clamp(finite(s.snow_line, 0.82f), 0.0f, 1.0f);
    s.rock_slope = std::clamp(finite(s.rock_slope, 0.9f), 0.1f, 20.0f);
    return s;
}

GeneratedTerrain GenerateTerrain(const TerrainGenSettings& requested) {
    const TerrainGenSettings s = SanitizeSettings(requested);
    const Grid g{s.width, s.height};
    GeneratedTerrain out;
    out.heightmap.width = s.width;
    out.heightmap.height = s.height;
    out.heightmap.cell_size = s.cell_size;

    std::vector<f32>& h = out.heightmap.heights;
    Shape(s, h);
    BoxBlur(g, h, s.smoothing);
    Thermal(s, h);
    Hydraulic(s, h);

    // Rivers: drain the filled surface, then cut channels where enough water collects.
    std::vector<f32> filled;
    std::vector<i32> next;
    FillDepressions(g, h, filled);
    FlowDirections(g, filled, next);
    Accumulate(filled, next, out.flow);
    out.river.assign(h.size(), 0);
    out.lake.assign(h.size(), 0);
    out.fill_height = filled; // the surface of a lake is where its basin spills; kept for drawing the water level
    for (usize i = 0; i < h.size(); ++i) {
        if (filled[i] - h[i] > s.lake_depth) {
            out.lake[i] = 1;
            ++out.lake_cells;
        }
    }
    const f32 catchment = static_cast<f32>(s.river_min_catchment);
    const f32 max_cut = s.river_depth * s.height_scale;
    std::vector<f32> cut(h.size(), 0.0f);
    for (usize i = 0; i < h.size(); ++i) {
        if (out.flow[i] < catchment || out.lake[i]) continue;
        out.river[i] = 1;
        ++out.river_cells;
        // Bigger rivers cut deeper, on a log scale (a river with ten times the water is not ten times as deep).
        cut[i] = max_cut * std::clamp(0.35f + 0.65f * std::log10(out.flow[i] / catchment), 0.0f, 1.0f);
    }
    // The channel's banks: a neighbour of a river cell is cut by half as much, so the channel is a valley and not a trench.
    std::vector<f32> banks = cut;
    for (u32 z = 0; z < s.height; ++z) {
        for (u32 x = 0; x < s.width; ++x) {
            const usize i = g.At(x, z);
            if (cut[i] <= 0.0f) continue;
            for (int k = 0; k < 8; ++k) {
                const i32 nx = static_cast<i32>(x) + kDx[k], nz = static_cast<i32>(z) + kDz[k];
                if (!g.In(nx, nz)) continue;
                const usize n = g.At(static_cast<u32>(nx), static_cast<u32>(nz));
                banks[n] = std::max(banks[n], cut[i] * 0.5f);
            }
        }
    }
    for (usize i = 0; i < h.size(); ++i) h[i] = std::max(h[i] - banks[i], 0.0f);

    const auto [lo, hi] = std::minmax_element(h.begin(), h.end());
    out.height_min = *lo;
    out.height_max = *hi;
    const f32 range = std::max(out.height_max - out.height_min, 1e-4f);
    out.sea_height = out.height_min + s.sea_level * range;
    Slopes(s, h, out.slope);

    // Moisture: noise, plus water nearby (a blurred river map), plus low ground.
    out.moisture.assign(h.size(), 0.0f);
    std::vector<f32> nearness(h.size(), 0.0f);
    for (usize i = 0; i < h.size(); ++i) nearness[i] = (out.river[i] || out.lake[i] || h[i] <= out.sea_height) ? 1.0f : 0.0f;
    BoxBlur(g, nearness, 8);
    out.temperature.assign(h.size(), 0.0f);
    out.biome.assign(h.size(), Biome::Grassland);
    for (u32 z = 0; z < s.height; ++z) {
        for (u32 x = 0; x < s.width; ++x) {
            const usize i = g.At(x, z);
            const f32 u = static_cast<f32>(x) / static_cast<f32>(s.width - 1), v = static_cast<f32>(z) / static_cast<f32>(s.height - 1);
            const f32 normalized = (h[i] - out.height_min) / range;
            const f32 wet = FractalNoise(u * 3.0f + 40.0f, v * 3.0f + 40.0f, 4, 0.5f, s.seed ^ 0x2545F491u) * 0.5f + 0.5f;
            const f32 warm = FractalNoise(u * 2.0f - 20.0f, v * 2.0f - 20.0f, 3, 0.5f, s.seed ^ 0x7F4A7C15u) * 0.5f + 0.5f;
            out.moisture[i] = std::clamp(wet * 0.6f + std::min(nearness[i] * 3.0f, 1.0f) * 0.3f + (1.0f - normalized) * 0.2f, 0.0f, 1.0f);
            out.temperature[i] = std::clamp(1.0f - normalized * 0.85f + (warm - 0.5f) * 0.3f, 0.0f, 1.0f);

            Biome b;
            const f32 sea_normalized = s.sea_level;
            if (out.river[i] || out.lake[i] || h[i] <= out.sea_height) b = Biome::Water;
            else if (out.slope[i] > s.rock_slope) b = Biome::Rock;
            else if (normalized > s.snow_line) b = Biome::Snow;
            else if (normalized < sea_normalized + s.beach_band) b = Biome::Beach;
            else if (out.moisture[i] < 0.34f && out.temperature[i] > 0.5f) b = Biome::Desert;
            else if (normalized > s.tree_line) b = out.moisture[i] > 0.55f ? Biome::Grassland : Biome::Rock;
            else b = out.moisture[i] > 0.45f ? Biome::Forest : Biome::Grassland;
            out.biome[i] = b;
        }
    }
    return out;
}

std::vector<f32> BuildMaterialWeights(const GeneratedTerrain& terrain, const TerrainGenSettings& requested, u32 resolution) {
    const TerrainGenSettings s = SanitizeSettings(requested);
    resolution = std::max(resolution, 1u);
    std::vector<f32> weights(static_cast<usize>(resolution) * resolution * 4, 0.0f);
    if (terrain.Width() == 0 || terrain.Height() == 0) return weights;
    const f32 range = std::max(terrain.height_max - terrain.height_min, 1e-4f);
    for (u32 pz = 0; pz < resolution; ++pz) {
        for (u32 px = 0; px < resolution; ++px) {
            const u32 x = std::min(static_cast<u32>((static_cast<f32>(px) + 0.5f) / static_cast<f32>(resolution) * static_cast<f32>(terrain.Width())), terrain.Width() - 1);
            const u32 z = std::min(static_cast<u32>((static_cast<f32>(pz) + 0.5f) / static_cast<f32>(resolution) * static_cast<f32>(terrain.Height())), terrain.Height() - 1);
            const usize i = terrain.Index(x, z);
            const f32 normalized = (terrain.heightmap.heights[i] - terrain.height_min) / range;
            const f32 rock = Smoothstep(s.rock_slope * 0.6f, s.rock_slope, terrain.slope[i]);
            const f32 snow = Smoothstep(s.snow_line - 0.04f, s.snow_line + 0.04f, normalized) * (1.0f - rock);
            const f32 beach = 1.0f - Smoothstep(s.sea_level + s.beach_band * 0.5f, s.sea_level + s.beach_band * 1.5f, normalized);
            const f32 dry = (1.0f - Smoothstep(0.28f, 0.4f, terrain.moisture[i])) * Smoothstep(0.45f, 0.55f, terrain.temperature[i]);
            const f32 sand = std::max(beach, dry) * (1.0f - rock) * (1.0f - snow);
            const f32 grass = std::max(1.0f - rock - snow - sand, 0.0f);
            f32 w[4] = {grass, rock, sand, snow};
            const f32 total = w[0] + w[1] + w[2] + w[3];
            for (int l = 0; l < 4; ++l) weights[(static_cast<usize>(pz) * resolution + px) * 4 + static_cast<usize>(l)] = total > 1e-6f ? w[l] / total : (l == 0 ? 1.0f : 0.0f);
        }
    }
    return weights;
}

SplatmapData BuildTerrainSplatmap(const GeneratedTerrain& terrain, const TerrainGenSettings& settings, std::span<const SplatmapLayer> layers,
                                  u32 resolution) {
    std::array<SplatmapLayer, 4> four{};
    const char* names[4] = {"Grass", "Rock", "Sand", "Snow"};
    for (usize l = 0; l < 4; ++l) {
        if (l < layers.size()) four[l] = layers[l];
        else four[l].name = names[l];
    }
    return BuildSplatmap(std::max(resolution, 1u), four, BuildMaterialWeights(terrain, settings, resolution));
}

std::vector<TerrainLayer> DefaultTerrainLayers() {
    std::vector<TerrainLayer> layers(4);
    layers[0] = {"Grass", Vec3(0.28f, 0.42f, 0.16f), 0.0f, 0.9f, 1.0f};
    layers[1] = {"Rock", Vec3(0.38f, 0.36f, 0.34f), 0.0f, 0.85f, 1.2f};
    layers[2] = {"Sand", Vec3(0.76f, 0.68f, 0.46f), 0.0f, 0.95f, 0.6f};
    layers[3] = {"Snow", Vec3(0.92f, 0.94f, 0.98f), 0.0f, 0.7f, 0.5f};
    return layers;
}

std::vector<u8> RenderTerrainPreview(const GeneratedTerrain& terrain) {
    const u32 w = terrain.Width(), h = terrain.Height();
    std::vector<u8> rgb(static_cast<usize>(w) * h * 3, 0);
    if (w == 0 || h == 0) return rgb;
    const Vec3 colors[kBiomeCount] = {Vec3(0.12f, 0.30f, 0.55f), Vec3(0.78f, 0.70f, 0.48f), Vec3(0.80f, 0.66f, 0.38f), Vec3(0.40f, 0.58f, 0.24f),
                                      Vec3(0.14f, 0.34f, 0.16f), Vec3(0.45f, 0.43f, 0.41f), Vec3(0.95f, 0.96f, 0.98f)};
    const std::vector<f32>& hm = terrain.heightmap.heights;
    const f32 cell = terrain.heightmap.cell_size;
    for (u32 z = 0; z < h; ++z) {
        for (u32 x = 0; x < w; ++x) {
            const usize i = terrain.Index(x, z);
            const u32 xl = x > 0 ? x - 1 : x, xr = x + 1 < w ? x + 1 : x, zu = z > 0 ? z - 1 : z, zd = z + 1 < h ? z + 1 : z;
            const f32 gx = (hm[terrain.Index(xr, z)] - hm[terrain.Index(xl, z)]) / (static_cast<f32>(xr - xl) * cell);
            const f32 gz = (hm[terrain.Index(x, zd)] - hm[terrain.Index(x, zu)]) / (static_cast<f32>(zd - zu) * cell);
            // The light comes from the north-west and above.
            const Vec3 normal = Vec3(-gx, 1.0f, -gz).Normalized();
            const Vec3 light = Vec3(-0.5f, 0.75f, -0.5f).Normalized();
            const f32 lit = 0.35f + 0.75f * std::max(normal.Dot(light), 0.0f);
            const Vec3 base = (terrain.river[i] || terrain.lake[i]) ? Vec3(0.18f, 0.42f, 0.78f) : colors[static_cast<usize>(terrain.biome[i])];
            for (int c = 0; c < 3; ++c) {
                const f32 value = (c == 0 ? base.x : c == 1 ? base.y : base.z) * lit;
                rgb[i * 3 + static_cast<usize>(c)] = static_cast<u8>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
        }
    }
    return rgb;
}

} // namespace terrain
} // namespace aether
