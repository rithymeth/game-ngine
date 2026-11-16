#include "forest_valley.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <queue>

namespace aether::forest_valley {

namespace {

using terrain::Biome;
using terrain::GeneratedTerrain;

constexpr u32 kSamples = 513; // 512 m at one sample per metre
constexpr f32 kEdgeMargin = 24.0f;
constexpr f32 kMinAreaSeparation = 90.0f;

bool CellWalkable(const GeneratedTerrain& t, u32 x, u32 z) {
    const usize i = t.Index(x, z);
    if (t.slope[i] > kMaxWalkSlope) return false;
    // Rivers are wadeable; the sea and standing lakes are not.
    if (t.lake[i]) return false;
    if (t.biome[i] == Biome::Water && !t.river[i]) return false;
    return true;
}

Vec3 Surface(const GeneratedTerrain& t, f32 x, f32 z) { return Vec3(x, t.heightmap.SampleLinear(x, z), z); }

f32 Dist2D(const Vec3& a, const Vec3& b) { return std::hypot(a.x - b.x, a.z - b.z); }

// True when every cell within `radius` metres of (cx, cz) is walkable with slope below `max_slope` and not water.
bool FlatDisc(const GeneratedTerrain& t, i32 cx, i32 cz, i32 radius, f32 max_slope) {
    const i32 w = static_cast<i32>(t.Width()), h = static_cast<i32>(t.Height());
    for (i32 dz = -radius; dz <= radius; dz += 2) {
        for (i32 dx = -radius; dx <= radius; dx += 2) {
            if (dx * dx + dz * dz > radius * radius) continue;
            const i32 x = cx + dx, z = cz + dz;
            if (x < 0 || z < 0 || x >= w || z >= h) return false;
            const usize i = t.Index(static_cast<u32>(x), static_cast<u32>(z));
            if (t.slope[i] > max_slope || t.lake[i] || t.river[i] || t.biome[i] == Biome::Water) return false;
        }
    }
    return true;
}

bool FarFromPicked(const std::vector<Vec3>& picked, const Vec3& p) {
    for (const Vec3& q : picked) {
        if (Dist2D(p, q) < kMinAreaSeparation) return false;
    }
    return true;
}

// 1 for every cell in the largest 4-connected piece of walkable ground. An area outside it could not be reached on foot, however good the
// spot looks, so no area is picked outside it.
std::vector<u8> MainLandmass(const GeneratedTerrain& t) {
    const i32 w = static_cast<i32>(t.Width()), h = static_cast<i32>(t.Height());
    std::vector<i32> label(static_cast<usize>(w) * static_cast<usize>(h), -1);
    std::vector<usize> stack;
    i32 best = -1;
    usize best_size = 0;
    i32 next = 0;
    for (usize seed = 0; seed < label.size(); ++seed) {
        if (label[seed] >= 0 || !CellWalkable(t, static_cast<u32>(seed % static_cast<usize>(w)), static_cast<u32>(seed / static_cast<usize>(w)))) continue;
        usize size = 0;
        label[seed] = next;
        stack.push_back(seed);
        while (!stack.empty()) {
            const usize k = stack.back();
            stack.pop_back();
            ++size;
            const i32 x = static_cast<i32>(k % static_cast<usize>(w)), z = static_cast<i32>(k / static_cast<usize>(w));
            for (i32 dz = -1; dz <= 1; ++dz) {
                for (i32 dx = -1; dx <= 1; ++dx) {
                    if (dx != 0 && dz != 0) continue; // 4-connected: the route search never cuts a corner, so neither may this
                    const i32 nx = x + dx, nz = z + dz;
                    if (nx < 0 || nz < 0 || nx >= w || nz >= h) continue;
                    const usize m = static_cast<usize>(nz) * static_cast<usize>(w) + static_cast<usize>(nx);
                    if (label[m] >= 0 || !CellWalkable(t, static_cast<u32>(nx), static_cast<u32>(nz))) continue;
                    label[m] = next;
                    stack.push_back(m);
                }
            }
        }
        if (size > best_size) {
            best_size = size;
            best = next;
        }
        ++next;
    }
    std::vector<u8> in_main(label.size(), 0);
    for (usize i = 0; i < label.size(); ++i) in_main[i] = label[i] == best ? 1 : 0;
    return in_main;
}

struct Picker {
    const GeneratedTerrain& t;
    const f32 cell;
    const std::vector<u8>& main_land;
    std::vector<Vec3> picked;

    // Best candidate (stride 4 cells) by `score` (higher wins) among those `accept` allows; false when none qualifies.
    template <class Accept, class Score>
    bool Pick(Accept accept, Score score, Vec3& out) {
        f32 best = -std::numeric_limits<f32>::max();
        bool found = false;
        const i32 margin = static_cast<i32>(kEdgeMargin / cell);
        for (i32 z = margin; z < static_cast<i32>(t.Height()) - margin; z += 4) {
            for (i32 x = margin; x < static_cast<i32>(t.Width()) - margin; x += 4) {
                const Vec3 p = Surface(t, static_cast<f32>(x) * cell, static_cast<f32>(z) * cell);
                if (!main_land[t.Index(static_cast<u32>(x), static_cast<u32>(z))] || !FarFromPicked(picked, p) || !accept(x, z, p)) continue;
                const f32 s = score(x, z, p);
                if (s > best) {
                    best = s;
                    out = p;
                    found = true;
                }
            }
        }
        if (found) picked.push_back(out);
        return found;
    }
};

// Grid A* over walkable cells at `step` cells, 8-connected. Returns the cell-centre path including both ends, empty when unreachable.
std::vector<Vec3> FindPath(const GeneratedTerrain& t, f32 cell, const Vec3& from, const Vec3& to, i32 step) {
    const i32 w = (static_cast<i32>(t.Width()) - 1) / step + 1, h = (static_cast<i32>(t.Height()) - 1) / step + 1;
    const auto node_of = [&](const Vec3& p) {
        const i32 x = std::clamp(static_cast<i32>(std::lround(p.x / cell / static_cast<f32>(step))), 0, w - 1);
        const i32 z = std::clamp(static_cast<i32>(std::lround(p.z / cell / static_cast<f32>(step))), 0, h - 1);
        return z * w + x;
    };
    const auto walkable = [&](i32 n) { return CellWalkable(t, static_cast<u32>(n % w * step), static_cast<u32>(n / w * step)); };
    const auto pos = [&](i32 n) { return Vec3(static_cast<f32>(n % w * step) * cell, 0, static_cast<f32>(n / w * step) * cell); };

    const i32 start = node_of(from), goal = node_of(to);
    std::vector<f32> g(static_cast<usize>(w) * static_cast<usize>(h), std::numeric_limits<f32>::max());
    std::vector<i32> parent(g.size(), -1);
    using Item = std::pair<f32, i32>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    g[static_cast<usize>(start)] = 0;
    open.push({0, start});
    while (!open.empty()) {
        const auto [f, n] = open.top();
        open.pop();
        (void)f;
        if (n == goal) break;
        const i32 nx = n % w, nz = n / w;
        for (i32 dz = -1; dz <= 1; ++dz) {
            for (i32 dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dz == 0) continue;
                const i32 mx = nx + dx, mz = nz + dz;
                if (mx < 0 || mz < 0 || mx >= w || mz >= h) continue;
                const i32 m = mz * w + mx;
                if (!walkable(m)) continue;
                // Corner cutting between two blocked cells would walk through a wall.
                if (dx != 0 && dz != 0 && (!walkable(nz * w + mx) || !walkable(mz * w + nx))) continue;
                const usize mi = static_cast<usize>(m);
                const f32 slope = t.slope[t.Index(static_cast<u32>(mx * step), static_cast<u32>(mz * step))];
                const f32 wade = t.river[t.Index(static_cast<u32>(mx * step), static_cast<u32>(mz * step))] ? 3.0f : 1.0f;
                const f32 cost = (dx != 0 && dz != 0 ? 1.4142f : 1.0f) * (1.0f + 2.0f * slope) * wade;
                const f32 cand = g[static_cast<usize>(n)] + cost;
                if (cand >= g[mi]) continue;
                g[mi] = cand;
                parent[mi] = n;
                const Vec3 a = pos(m), b = pos(goal);
                open.push({cand + std::hypot(a.x - b.x, a.z - b.z) / (cell * static_cast<f32>(step)), m});
            }
        }
    }
    if (parent[static_cast<usize>(goal)] < 0 && start != goal) return {};
    std::vector<Vec3> path;
    for (i32 n = goal; n >= 0; n = parent[static_cast<usize>(n)]) {
        Vec3 p = pos(n);
        p.y = t.heightmap.SampleLinear(p.x, p.z);
        path.push_back(p);
        if (n == start) break;
    }
    std::reverse(path.begin(), path.end());
    return path;
}

} // namespace

const char* AreaName(AreaId id) {
    switch (id) {
    case AreaId::Meadow: return "Starting Meadow";
    case AreaId::Woodland: return "Ancient Woodland";
    case AreaId::River: return "River and Waterfall";
    case AreaId::Ruins: return "Ancient Ruins";
    case AreaId::Overlook: return "Mountain Overlook";
    }
    return "?";
}

bool World::Walkable(f32 x, f32 z) const {
    const f32 gx = x / settings.cell_size, gz = z / settings.cell_size;
    if (gx < 0 || gz < 0 || gx > static_cast<f32>(terrain.Width() - 1) || gz > static_cast<f32>(terrain.Height() - 1)) return false;
    return CellWalkable(terrain, static_cast<u32>(std::lround(gx)), static_cast<u32>(std::lround(gz)));
}

World BuildWorld(u32 seed) {
    World world;
    world.settings.seed = seed;
    world.settings.width = kSamples;
    world.settings.height = kSamples;
    world.settings.cell_size = 1.0f;
    // Tuned by measurement, not taste: the generator's defaults give a map 46% water and 21% walkable in its largest piece, which no
    // route can cross. A low sea, gentler ridges and fewer, larger rivers give about 85% walkable ground in one piece.
    world.settings.height_scale = 50.0f;
    world.settings.ridge_amount = 0.3f;
    world.settings.frequency = 2.0f;
    world.settings.sea_level = 0.04f;
    world.settings.river_min_catchment = 1500;
    world.settings = terrain::SanitizeSettings(world.settings);
    world.terrain = terrain::GenerateTerrain(world.settings);
    const GeneratedTerrain& t = world.terrain;
    const f32 cell = world.settings.cell_size;
    const std::vector<u8> main_land = MainLandmass(t);
    Picker pick{t, cell, main_land, {}};

    Vec3 meadow, woodland, river, ruins, overlook;
    const f32 span = t.height_max - t.height_min;
    const auto set = [&](AreaId id, const Vec3& c, f32 radius) { world.areas[static_cast<usize>(id)] = Area{id, c, radius}; };

    // E. Overlook, picked first so nothing else takes the high ground: the highest flat standing place.
    pick.Pick([&](i32 x, i32 z, const Vec3&) { return FlatDisc(t, x, z, 5, 0.3f); }, [](i32, i32, const Vec3& p) { return p.y; }, overlook);
    // A. Meadow, the lowest wide flat ground (picked second, so it cannot take the overlook's plateau): the lowest wide flat grass or forest floor, so the spawn looks up at the valley.
    pick.Pick(
        [&](i32 x, i32 z, const Vec3& p) {
            // Grass or forest floor well clear of the beach band: the lowest flat ground is the shore, and a meadow painted as sand is not one.
            const Biome b = t.biome[t.Index(static_cast<u32>(x), static_cast<u32>(z))];
            return (b == Biome::Grassland || b == Biome::Forest) && p.y > t.sea_height + 5.0f && FlatDisc(t, x, z, 14, 0.2f);
        },
        [](i32, i32, const Vec3& p) { return -p.y; }, meadow);
    // C. River: the river cell carrying the most water, with walkable banks.
    pick.Pick([&](i32 x, i32 z, const Vec3&) { return t.river[t.Index(static_cast<u32>(x), static_cast<u32>(z))] != 0; },
              [&](i32 x, i32 z, const Vec3&) { return t.flow[t.Index(static_cast<u32>(x), static_cast<u32>(z))]; }, river);
    // B. Woodland: the 48 m window holding the most forest.
    pick.Pick([&](i32 x, i32 z, const Vec3&) { return CellWalkable(t, static_cast<u32>(x), static_cast<u32>(z)); },
              [&](i32 x, i32 z, const Vec3&) {
                  u32 n = 0;
                  for (i32 dz = -24; dz <= 24; dz += 4) {
                      for (i32 dx = -24; dx <= 24; dx += 4) {
                          const i32 px = x + dx, pz = z + dz;
                          if (px < 0 || pz < 0 || px >= static_cast<i32>(t.Width()) || pz >= static_cast<i32>(t.Height())) continue;
                          n += t.biome[t.Index(static_cast<u32>(px), static_cast<u32>(pz))] == Biome::Forest ? 1u : 0u;
                      }
                  }
                  return static_cast<f32>(n);
              },
              woodland);
    // D. Ruins: a wide flat site as far as possible from the other four.
    pick.Pick([&](i32 x, i32 z, const Vec3&) { return FlatDisc(t, x, z, 10, 0.2f); },
              [&](i32, i32, const Vec3& p) {
                  f32 nearest = 1.0e9f;
                  for (const Vec3& q : pick.picked) nearest = std::min(nearest, Dist2D(p, q));
                  return nearest;
              },
              ruins);
    (void)span;

    set(AreaId::Meadow, meadow, 40.0f);
    set(AreaId::Woodland, woodland, 48.0f);
    set(AreaId::River, river, 36.0f);
    set(AreaId::Ruins, ruins, 30.0f);
    set(AreaId::Overlook, overlook, 20.0f);

    // The route visits the areas in order and is proof that they are joined: any leg with no walkable path leaves `connected` false.
    world.connected = true;
    for (u32 i = 0; i + 1 < kAreaCount; ++i) {
        const std::vector<Vec3> leg = FindPath(t, cell, world.areas[i].center, world.areas[i + 1].center, 1);
        if (leg.empty()) {
            world.connected = false;
            break;
        }
        world.route.insert(world.route.end(), world.route.empty() ? leg.begin() : leg.begin() + 1, leg.end());
    }
    for (usize i = 1; i < world.route.size(); ++i) world.route_length += Dist2D(world.route[i - 1], world.route[i]);
    return world;
}

std::vector<terrain::ScatterRule> ShowcaseScatterRules(const ScatterBudget& b) {
    std::vector<terrain::ScatterRule> rules;
    for (terrain::ScatterRule rule : terrain::DefaultScatterRules()) {
        const auto share = [](u32 total, f32 f) { return static_cast<u32>(static_cast<f32>(total) * f); };
        if (rule.name == "Pine") rule.max_instances = share(b.trees_max, 0.7f);
        else if (rule.name == "Birch") rule.max_instances = share(b.trees_max, 0.3f);
        else if (rule.name == "Grass") rule.max_instances = share(b.plants_max, 0.8f);
        else if (rule.name == "Shrub") rule.max_instances = share(b.plants_max, 0.15f);
        else if (rule.name == "Reeds") rule.max_instances = share(b.plants_max, 0.05f);
        else if (rule.name == "Boulder") {
            rule.max_instances = b.rocks_max;
            rule.density = 2.0f; // the default 0.35 gives a few dozen boulders in this valley, a tenth of the budget
        }
        else continue; // the desert and snow rules do not belong in a temperate valley
        rules.push_back(std::move(rule));
    }
    return rules;
}

Walker::Walker(const World& world, Vec3 start, WalkerConfig config) : world_(&world), config_(config), pos_(start) {
    pos_.y = world.HeightAt(start.x, start.z);
    grounded_ = true;
}

void Walker::Step(const WalkerInput& in) {
    const f32 dt = kStep;
    const f32 speed = in.run ? config_.run_speed : config_.walk_speed;
    f32 mx = in.move_x, mz = in.move_z;
    const f32 len = std::hypot(mx, mz);
    if (len > 1.0f) {
        mx /= len;
        mz /= len;
    }
    // Approach the desired velocity; in the air the same rule, so a jump keeps its momentum rather than stopping dead.
    const f32 tx = mx * speed, tz = mz * speed;
    const f32 dx = tx - vel_.x, dz = tz - vel_.z;
    const f32 dl = std::hypot(dx, dz);
    const f32 step = config_.acceleration * dt;
    if (dl <= step || dl == 0.0f) {
        vel_.x = tx;
        vel_.z = tz;
    } else {
        vel_.x += dx / dl * step;
        vel_.z += dz / dl * step;
    }

    // Steep ground and water stop the walker (a cliff is a wall); the axes are tried apart so it slides along one.
    const Vec3 before = pos_;
    const f32 nx = pos_.x + vel_.x * dt, nz = pos_.z + vel_.z * dt;
    if (world_->Walkable(nx, pos_.z)) pos_.x = nx; else vel_.x = 0;
    if (world_->Walkable(pos_.x, nz)) pos_.z = nz; else vel_.z = 0;
    distance_ += std::hypot(pos_.x - before.x, pos_.z - before.z);

    const bool pressed = in.jump && !jump_was_down_;
    jump_was_down_ = in.jump;
    coyote_ = grounded_ ? config_.coyote_time : std::max(0.0f, coyote_ - dt);
    if (pressed && coyote_ > 0.0f) {
        vel_.y = config_.jump_speed;
        grounded_ = false;
        coyote_ = 0.0f;
    }

    vel_.y = std::max(vel_.y - config_.gravity * dt, -config_.max_fall);
    pos_.y += vel_.y * dt;
    const f32 ground = world_->HeightAt(pos_.x, pos_.z);
    if (pos_.y <= ground) {
        pos_.y = ground;
        vel_.y = 0.0f;
        grounded_ = true;
    } else if (pos_.y > ground + 0.05f) {
        grounded_ = false;
    }
}

std::string BenchmarkCameraPathJson(const World& world, f32 speed, f32 fps, f32 eye_height) {
    std::string json;
    char buf[256];
    std::snprintf(buf, sizeof buf, "{\"fps\": %.1f, \"keys\": [", static_cast<double>(fps));
    json += buf;
    // One key every ~12 m along the route, so the Catmull-Rom curve smooths the A* grid's corners without cutting across a hill.
    constexpr f32 kKeySpacing = 12.0f;
    f32 time = 0.0f, since_key = kKeySpacing;
    bool first = true;
    const usize n = world.route.size();
    for (usize i = 0; i < n; ++i) {
        if (i > 0) {
            const f32 d = Dist2D(world.route[i - 1], world.route[i]);
            time += d / speed;
            since_key += d;
        }
        if (since_key < kKeySpacing && i + 1 != n) continue;
        since_key = 0.0f;
        const Vec3& p = world.route[i];
        const Vec3& ahead = world.route[std::min(n - 1, i + 3)];
        const Vec3& behind = world.route[i >= 3 ? i - 3 : 0];
        const Vec3 look = (i + 3 < n || n < 4) ? ahead - p : p - behind;
        // yaw 0 faces -Z, positive turns left: forward = (-sin yaw, -cos yaw).
        const f32 yaw = std::atan2(-look.x, -look.z) * 180.0f / 3.14159265f;
        std::snprintf(buf, sizeof buf, "%s{\"t\": %.3f, \"position\": [%.3f, %.3f, %.3f], \"yaw\": %.2f, \"pitch\": -4}", first ? "" : ", ", static_cast<double>(time),
                      static_cast<double>(p.x), static_cast<double>(p.y + eye_height), static_cast<double>(p.z), static_cast<double>(yaw));
        json += buf;
        first = false;
    }
    json += "]}";
    return json;
}

f32 Percentile(std::vector<f32> values, f64 p) {
    if (values.empty()) return 0.0f;
    std::sort(values.begin(), values.end());
    const usize rank = static_cast<usize>(std::ceil(p * static_cast<f64>(values.size())));
    return values[std::min(values.size() - 1, rank == 0 ? usize{0} : rank - 1)];
}

GateResult EvaluateGates(const std::vector<f32>& cpu_ms, const std::vector<f32>& gpu_ms, const FrameGates& gates) {
    GateResult r;
    if (!cpu_ms.empty()) r.cpu_p95 = Percentile(cpu_ms, 0.95) <= gates.cpu_p95_ms;
    if (!gpu_ms.empty()) r.gpu_p95 = Percentile(gpu_ms, 0.95) <= gates.gpu_p95_ms;
    // p99 is of the slower of the two, frame by frame, when both were measured: a frame is as long as its longer side.
    std::vector<f32> frame;
    const usize n = std::min(cpu_ms.size(), gpu_ms.size());
    if (n > 0) {
        for (usize i = 0; i < n; ++i) frame.push_back(std::max(cpu_ms[i], gpu_ms[i]));
    } else {
        frame = cpu_ms.empty() ? gpu_ms : cpu_ms;
    }
    if (!frame.empty()) r.p99 = Percentile(frame, 0.99) <= gates.p99_ms;
    return r;
}

} // namespace aether::forest_valley
