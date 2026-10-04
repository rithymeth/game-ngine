#include "aether/sprite2d/lights2d.h"

#include <algorithm>
#include <cmath>

namespace aether::sprite2d {

namespace {

constexpr f32 kPi = 3.14159265358979f;

template <typename F>
void EachEntity(const World& world, F&& f) {
    world.ForEachArchetype([&](const Archetype& archetype) {
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            const u32 count = archetype.ChunkEntityCount(c);
            const Entity* entities = archetype.EntityArray(c);
            for (u32 i = 0; i < count; ++i) f(entities[i]);
        }
    });
}

f32 Cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }

// The distance along the ray `origin + t * dir` (t >= 0) to the segment, or a negative number for a miss.
f32 RaySegment(Vec2 origin, Vec2 dir, const Segment2D& s) {
    const Vec2 e = s.b - s.a;
    const f32 denom = Cross(dir, e);
    if (std::fabs(denom) < 1e-9f) return -1.0f; // parallel
    const Vec2 diff = s.a - origin;
    const f32 t = Cross(diff, e) / denom;
    const f32 u = Cross(diff, dir) / denom;
    if (t < 0.0f || u < -1e-5f || u > 1.0f + 1e-5f) return -1.0f;
    return t;
}

f32 SmoothStep(f32 a, f32 b, f32 x) {
    if (b <= a) return x >= b ? 1.0f : 0.0f;
    const f32 t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

f32 AngleOf(const Quaternion& q) { return 2.0f * std::atan2(q.z, q.w) * 180.0f / kPi; }

void AddBox(std::vector<Segment2D>& out, Vec2 lo, Vec2 hi) {
    const Vec2 a{lo.x, lo.y}, b{hi.x, lo.y}, c{hi.x, hi.y}, d{lo.x, hi.y};
    out.push_back({a, b});
    out.push_back({b, c});
    out.push_back({c, d});
    out.push_back({d, a});
}

} // namespace

LightColor LightMap::Sample(f32 x, f32 y) const {
    if (width == 0 || height == 0) return {};
    const f32 fx = (x - origin.x) / cell_size - 0.5f;
    const f32 fy = (y - origin.y) / cell_size - 0.5f;
    const i32 x0 = static_cast<i32>(std::floor(fx)), y0 = static_cast<i32>(std::floor(fy));
    const f32 tx = fx - static_cast<f32>(x0), ty = fy - static_cast<f32>(y0);
    const auto lerp = [](const LightColor& a, const LightColor& b, f32 t) {
        return LightColor{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
    };
    const LightColor bottom = lerp(At(x0, y0), At(x0 + 1, y0), tx);
    const LightColor top = lerp(At(x0, y0 + 1), At(x0 + 1, y0 + 1), tx);
    return lerp(bottom, top, ty);
}

const LightColor& LightMap::At(i32 x, i32 y) const {
    x = std::clamp(x, 0, static_cast<i32>(width) - 1);
    y = std::clamp(y, 0, static_cast<i32>(height) - 1);
    return cells[static_cast<usize>(y) * width + static_cast<usize>(x)];
}

std::vector<Segment2D> GatherOccluders(const World& world, const Resolvers2D& tiles, Vec2 region_min, Vec2 region_max) {
    std::vector<Segment2D> out;
    EachEntity(world, [&](Entity e) {
        const Transform* t = world.GetComponent<Transform>(e);
        if (!t) return;
        if (const ShadowCaster2D* c = world.GetComponent<ShadowCaster2D>(e)) {
            const Vec2 centre{t->position.x + c->offset.x, t->position.y + c->offset.y};
            const Vec2 lo{centre.x - c->half_extents.x, centre.y - c->half_extents.y};
            const Vec2 hi{centre.x + c->half_extents.x, centre.y + c->half_extents.y};
            if (hi.x >= region_min.x && lo.x <= region_max.x && hi.y >= region_min.y && lo.y <= region_max.y) AddBox(out, lo, hi);
        }
        const TilemapRenderer* r = world.GetComponent<TilemapRenderer>(e);
        if (!r || !tiles.tilemaps || !tiles.tilesets || r->pixels_per_unit <= 0.0f) return;
        const TilemapData* map = tiles.tilemaps(r->tilemap.guid);
        const Tileset* ts = map ? tiles.tilesets(map->tileset) : nullptr;
        if (!map || !ts) return;
        const f32 cw = static_cast<f32>(ts->tile_width) / r->pixels_per_unit, ch = static_cast<f32>(ts->tile_height) / r->pixels_per_unit;
        const i32 x0 = std::max(0, static_cast<i32>(std::floor((region_min.x - t->position.x) / cw)));
        const i32 y0 = std::max(0, static_cast<i32>(std::floor((region_min.y - t->position.y) / ch)));
        const i32 x1 = std::min(static_cast<i32>(map->width) - 1, static_cast<i32>(std::floor((region_max.x - t->position.x) / cw)));
        const i32 y1 = std::min(static_cast<i32>(map->height) - 1, static_cast<i32>(std::floor((region_max.y - t->position.y) / ch)));
        const auto solid = [&](i32 x, i32 y) { return map->InBounds(x, y) && IsSolidAt(*map, *ts, x, y); };
        for (i32 y = y0; y <= y1; ++y) {
            for (i32 x = x0; x <= x1; ++x) {
                if (!solid(x, y)) continue;
                const f32 lx = t->position.x + static_cast<f32>(x) * cw, ly = t->position.y + static_cast<f32>(y) * ch;
                const Vec2 bl{lx, ly}, br{lx + cw, ly}, tr{lx + cw, ly + ch}, tl{lx, ly + ch};
                if (!solid(x, y - 1)) out.push_back({bl, br});
                if (!solid(x + 1, y)) out.push_back({br, tr});
                if (!solid(x, y + 1)) out.push_back({tr, tl});
                if (!solid(x - 1, y)) out.push_back({tl, bl});
            }
        }
    });
    return out;
}

bool Occluded(const std::vector<Segment2D>& occluders, Vec2 from, Vec2 to) {
    const Vec2 d = to - from;
    const f32 len = d.Length();
    if (len < 1e-6f) return false;
    const Vec2 dir = d * (1.0f / len);
    for (const Segment2D& s : occluders) {
        const f32 t = RaySegment(from, dir, s);
        if (t >= 0.0f && t < len - 1e-4f) return true;
    }
    return false;
}

LightColor LightAt(const World& world, const std::vector<Segment2D>& occluders, Vec2 point) {
    LightColor sum;
    EachEntity(world, [&](Entity e) {
        const Light2D* light = world.GetComponent<Light2D>(e);
        const Transform* t = world.GetComponent<Transform>(e);
        if (!light || !t || !light->enabled) return;
        f32 amount = light->intensity;
        if (light->type != Light2DType::Global) {
            const Vec2 at{t->position.x, t->position.y};
            const Vec2 d = point - at;
            const f32 dist = d.Length();
            if (dist >= light->radius || light->radius <= 0.0f) return;
            amount *= std::pow(1.0f - dist / light->radius, light->falloff);
            if (light->type == Light2DType::Spot && dist > 1e-5f) {
                const f32 axis = (light->direction_degrees + AngleOf(t->rotation)) * kPi / 180.0f;
                const f32 cos_to = d.Dot(Vec2{std::cos(axis), std::sin(axis)}) * (1.0f / dist);
                const f32 angle = std::acos(std::clamp(cos_to, -1.0f, 1.0f)) * 180.0f / kPi;
                amount *= 1.0f - SmoothStep(light->inner_angle, light->outer_angle, angle);
            }
            if (amount <= 0.0f) return;
            if (light->cast_shadows && Occluded(occluders, at, point)) return;
        }
        sum.r += light->color.r * amount;
        sum.g += light->color.g * amount;
        sum.b += light->color.b * amount;
    });
    return sum;
}

LightMap BuildLightMap(const World& world, const Resolvers2D& tiles, Vec2 region_min, Vec2 region_max, f32 cell_size) {
    LightMap map;
    if (cell_size <= 0.0f || region_max.x <= region_min.x || region_max.y <= region_min.y) return map;
    map.origin = region_min;
    map.cell_size = cell_size;
    map.width = static_cast<u32>(std::ceil((region_max.x - region_min.x) / cell_size));
    map.height = static_cast<u32>(std::ceil((region_max.y - region_min.y) / cell_size));
    if (static_cast<u64>(map.width) * map.height > 4u * 1024u * 1024u) return LightMap{};
    // Occluders from a little beyond the region: a light outside still shadows it.
    f32 reach = 0.0f;
    EachEntity(world, [&](Entity e) {
        if (const Light2D* l = world.GetComponent<Light2D>(e); l && l->enabled && l->type != Light2DType::Global) reach = std::max(reach, l->radius);
    });
    const std::vector<Segment2D> occluders =
        GatherOccluders(world, tiles, {region_min.x - reach, region_min.y - reach}, {region_max.x + reach, region_max.y + reach});
    map.cells.assign(static_cast<usize>(map.width) * map.height, LightColor{});
    // Which cells are solid (a segment test would be wrong inside one): a cell is solid if a tilemap says so.
    std::vector<u8> solid(map.cells.size(), 0);
    EachEntity(world, [&](Entity e) {
        const Transform* t = world.GetComponent<Transform>(e);
        const TilemapRenderer* r = world.GetComponent<TilemapRenderer>(e);
        if (!t || !r || !tiles.tilemaps || !tiles.tilesets || r->pixels_per_unit <= 0.0f) return;
        const TilemapData* tm = tiles.tilemaps(r->tilemap.guid);
        const Tileset* ts = tm ? tiles.tilesets(tm->tileset) : nullptr;
        if (!tm || !ts) return;
        const f32 cw = static_cast<f32>(ts->tile_width) / r->pixels_per_unit, ch = static_cast<f32>(ts->tile_height) / r->pixels_per_unit;
        for (u32 y = 0; y < map.height; ++y) {
            for (u32 x = 0; x < map.width; ++x) {
                const f32 px = region_min.x + (static_cast<f32>(x) + 0.5f) * cell_size - t->position.x;
                const f32 py = region_min.y + (static_cast<f32>(y) + 0.5f) * cell_size - t->position.y;
                const i32 cx = static_cast<i32>(std::floor(px / cw)), cy = static_cast<i32>(std::floor(py / ch));
                if (tm->InBounds(cx, cy) && IsSolidAt(*tm, *ts, cx, cy)) solid[static_cast<usize>(y) * map.width + x] = 1;
            }
        }
    });
    for (u32 y = 0; y < map.height; ++y) {
        for (u32 x = 0; x < map.width; ++x) {
            if (solid[static_cast<usize>(y) * map.width + x]) continue;
            const Vec2 centre{region_min.x + (static_cast<f32>(x) + 0.5f) * cell_size, region_min.y + (static_cast<f32>(y) + 0.5f) * cell_size};
            map.cells[static_cast<usize>(y) * map.width + x] = LightAt(world, occluders, centre);
        }
    }
    for (u32 y = 0; y < map.height; ++y) {
        for (u32 x = 0; x < map.width; ++x) {
            if (!solid[static_cast<usize>(y) * map.width + x]) continue;
            LightColor best;
            for (const auto& [dx, dy] : {std::pair<i32, i32>{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
                const i32 nx = static_cast<i32>(x) + dx, ny = static_cast<i32>(y) + dy;
                if (nx < 0 || ny < 0 || nx >= static_cast<i32>(map.width) || ny >= static_cast<i32>(map.height)) continue;
                if (solid[static_cast<usize>(ny) * map.width + static_cast<usize>(nx)]) continue;
                const LightColor& c = map.cells[static_cast<usize>(ny) * map.width + static_cast<usize>(nx)];
                if (c.r + c.g + c.b > best.r + best.g + best.b) best = c;
            }
            map.cells[static_cast<usize>(y) * map.width + x] = best;
        }
    }
    return map;
}

std::vector<Vec2> VisibilityPolygon(Vec2 center, f32 radius, const std::vector<Segment2D>& occluders) {
    std::vector<Vec2> polygon;
    if (radius <= 0.0f) return polygon;
    // The bounds: a square round the light, so every ray ends somewhere.
    std::vector<Segment2D> segs = occluders;
    AddBox(segs, {center.x - radius, center.y - radius}, {center.x + radius, center.y + radius});
    // Rays at every endpoint, and a hair either side of it.
    std::vector<f32> angles;
    for (const Segment2D& s : segs) {
        for (Vec2 p : {s.a, s.b}) {
            const f32 a = std::atan2(p.y - center.y, p.x - center.x);
            angles.push_back(a - 1e-4f);
            angles.push_back(a);
            angles.push_back(a + 1e-4f);
        }
    }
    std::sort(angles.begin(), angles.end());
    for (f32 a : angles) {
        const Vec2 dir{std::cos(a), std::sin(a)};
        f32 nearest = 1e30f;
        for (const Segment2D& s : segs) {
            const f32 t = RaySegment(center, dir, s);
            if (t >= 0.0f && t < nearest) nearest = t;
        }
        if (nearest >= 1e29f) continue;
        polygon.push_back(center + dir * std::min(nearest, radius * 1.4143f));
    }
    return polygon;
}

void RegisterLight2DComponents() {
    (void)GetComponentId<Light2D>();
    (void)GetComponentId<ShadowCaster2D>();
}

} // namespace aether::sprite2d
