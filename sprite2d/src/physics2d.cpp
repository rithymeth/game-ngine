#include "aether/sprite2d/physics2d.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace aether::sprite2d {

namespace {

// Every live entity (ForEach hands out components, not entities).
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

u64 Key(Entity e) { return (static_cast<u64>(e.generation) << 32) | e.index; }
// The same for (a, b) and (b, a), so a pair keeps its identity whatever order the sweep finds it in.
u64 PairKey(u64 a, u64 b, bool trigger) {
    if (a > b) std::swap(a, b);
    u64 h = a * 0x9E3779B97F4A7C15ull;
    h ^= b + 0x7F4A7C15ull + (h << 6) + (h >> 2);
    h *= 0xC2B2AE3D27D4EB4Full;
    return (h & ~1ull) | (trigger ? 1ull : 0ull);
}

struct Body {
    Entity e;
    Transform* transform = nullptr;
    Rigidbody2D* rb = nullptr;
    Vec2 p, v;
    f32 inv_mass = 0.0f;
    Shape2D shape = Shape2D::Box;
    Vec2 half;
    f32 radius = 0.5f;
    Vec2 offset;
    f32 friction = 0.4f, restitution = 0.0f;
    bool trigger = false;
    u32 layer = 1, mask = ~0u;
    bool dynamic = false;
    bool tile = false; // a solid cell
    i32 cx = 0, cy = 0;
    const TilemapData* map = nullptr; // for tile bodies: the map (to cull internal faces)
    const Tileset* tileset = nullptr;
    Vec2 map_origin;
    Vec2 cell_size;

    Vec2 Min() const { return shape == Shape2D::Box ? Vec2{p.x - half.x, p.y - half.y} : Vec2{p.x - radius, p.y - radius}; }
    Vec2 Max() const { return shape == Shape2D::Box ? Vec2{p.x + half.x, p.y + half.y} : Vec2{p.x + radius, p.y + radius}; }
};

struct Manifold {
    Vec2 normal; // from a to b
    f32 depth = 0;
    bool hit = false;
};

Manifold BoxBox(const Body& a, const Body& b, f32 margin) {
    Manifold m;
    const f32 dx = b.p.x - a.p.x, dy = b.p.y - a.p.y;
    const f32 ox = a.half.x + b.half.x - std::fabs(dx) + margin;
    const f32 oy = a.half.y + b.half.y - std::fabs(dy) + margin;
    if (ox <= 0.0f || oy <= 0.0f) return m;
    m.hit = true;
    if (ox < oy) {
        m.normal = {dx < 0.0f ? -1.0f : 1.0f, 0.0f};
        m.depth = ox;
    } else {
        m.normal = {0.0f, dy < 0.0f ? -1.0f : 1.0f};
        m.depth = oy;
    }
    return m;
}

Manifold CircleCircle(const Body& a, const Body& b, f32 margin) {
    Manifold m;
    const Vec2 d = b.p - a.p;
    const f32 r = a.radius + b.radius + margin;
    const f32 dist2 = d.x * d.x + d.y * d.y;
    if (dist2 >= r * r) return m;
    const f32 dist = std::sqrt(dist2);
    m.hit = true;
    m.normal = dist > 1e-6f ? d * (1.0f / dist) : Vec2{0.0f, 1.0f};
    m.depth = r - dist;
    return m;
}

// `circle` is a body with shape Circle, `box` a Box; the normal is from the circle to the box.
Manifold CircleBox(const Body& circle, const Body& box, f32 margin) {
    Manifold m;
    const Vec2 d = circle.p - box.p;
    const Vec2 clamped{std::clamp(d.x, -box.half.x, box.half.x), std::clamp(d.y, -box.half.y, box.half.y)};
    const Vec2 closest = box.p + clamped;
    Vec2 to_box = closest - circle.p; // toward the box
    const f32 dist2 = to_box.x * to_box.x + to_box.y * to_box.y;
    const f32 r = circle.radius + margin;
    if (dist2 > 1e-12f) {
        if (dist2 >= r * r) return m;
        const f32 dist = std::sqrt(dist2);
        m.hit = true;
        m.normal = to_box * (1.0f / dist);
        m.depth = r - dist;
        return m;
    }
    // The centre is inside the box: leave by the nearest face.
    const f32 px = box.half.x - std::fabs(d.x), py = box.half.y - std::fabs(d.y);
    m.hit = true;
    if (px < py) {
        m.normal = {d.x < 0.0f ? 1.0f : -1.0f, 0.0f}; // from the circle, back toward the box's centre side
        m.depth = px + circle.radius;
    } else {
        m.normal = {0.0f, d.y < 0.0f ? 1.0f : -1.0f};
        m.depth = py + circle.radius;
    }
    return m;
}

Manifold Collide(const Body& a, const Body& b, f32 margin) {
    if (a.shape == Shape2D::Box && b.shape == Shape2D::Box) return BoxBox(a, b, margin);
    if (a.shape == Shape2D::Circle && b.shape == Shape2D::Circle) return CircleCircle(a, b, margin);
    if (a.shape == Shape2D::Circle) return CircleBox(a, b, margin);
    Manifold m = CircleBox(b, a, margin); // the normal is from b (circle) to a
    m.normal = m.normal * -1.0f;
    return m;
}

bool Allowed(const Body& a, const Body& b) { return (a.layer & b.mask) != 0 && (b.layer & a.mask) != 0; }

bool SolidCell(const Body& tile, i32 x, i32 y) {
    return tile.map && tile.map->InBounds(x, y) && IsSolidAt(*tile.map, *tile.tileset, x, y);
}

// A tile face whose neighbour is also solid can't be hit; dropping those
// contacts keeps a body from catching on the seams between tiles.
bool InternalFace(const Body& tile, const Vec2& normal_from_tile) {
    const i32 nx = tile.cx + static_cast<i32>(std::lround(normal_from_tile.x));
    const i32 ny = tile.cy + static_cast<i32>(std::lround(normal_from_tile.y));
    return SolidCell(tile, nx, ny);
}

bool SlabRay(Vec2 origin, Vec2 dir, Vec2 lo, Vec2 hi, f32 max_t, f32& t_out, Vec2& normal) {
    f32 t0 = 0.0f, t1 = max_t;
    Vec2 n{};
    const f32 o[2] = {origin.x, origin.y}, d[2] = {dir.x, dir.y}, l[2] = {lo.x, lo.y}, h[2] = {hi.x, hi.y};
    for (int axis = 0; axis < 2; ++axis) {
        if (std::fabs(d[axis]) < 1e-9f) {
            if (o[axis] < l[axis] || o[axis] > h[axis]) return false;
            continue;
        }
        f32 ta = (l[axis] - o[axis]) / d[axis], tb = (h[axis] - o[axis]) / d[axis];
        f32 sign = -1.0f; // entering through the low face: the normal points to -axis
        if (ta > tb) {
            std::swap(ta, tb);
            sign = 1.0f;
        }
        if (ta > t0) {
            t0 = ta;
            n = axis == 0 ? Vec2{sign, 0.0f} : Vec2{0.0f, sign};
        }
        t1 = std::min(t1, tb);
        if (t0 > t1) return false;
    }
    t_out = t0;
    normal = n;
    return true;
}

} // namespace

Physics2D::Physics2D(World& world, Resolvers2D tiles, Physics2DSettings s)
    : settings(s), world_(world), tiles_(std::move(tiles)) {
    RegisterPhysics2DComponents();
}

bool Physics2D::IsGrounded(Entity e) const { return std::binary_search(grounded_.begin(), grounded_.end(), Key(e)); }
int Physics2D::WallSide(Entity e) const {
    if (std::binary_search(left_wall_.begin(), left_wall_.end(), Key(e))) return -1;
    if (std::binary_search(right_wall_.begin(), right_wall_.end(), Key(e))) return 1;
    return 0;
}

void Physics2D::Step(f32 dt) {
    events_.clear();
    dt = std::min(dt, settings.max_step);
    if (dt <= 0.0f) return;
    bool any = false;
    world_.ForEachChunk<Collider2D>([&](u32, Collider2D*) { any = true; });
    if (!any) { // nothing 2D here: skip the walk, and end what was touching
        body_count_ = 0;
        pairs_.clear();
        last_info_.clear();
        grounded_.clear();
        left_wall_.clear();
        right_wall_.clear();
        return;
    }

    // Gather the bodies.
    std::vector<Body> bodies;
    std::vector<Entity> entities;
    EachEntity(world_, [&](Entity e) {
        if (world_.GetComponent<Transform>(e) && world_.GetComponent<Collider2D>(e)) entities.push_back(e);
    });
    for (Entity e : entities) {
        Body b;
        b.e = e;
        b.transform = world_.GetComponent<Transform>(e);
        const Collider2D* c = world_.GetComponent<Collider2D>(e);
        b.rb = world_.GetComponent<Rigidbody2D>(e);
        b.shape = c->shape;
        b.half = c->half_extents;
        b.radius = c->radius;
        b.offset = c->offset;
        b.friction = c->friction;
        b.restitution = c->restitution;
        b.trigger = c->trigger;
        b.layer = c->layer;
        b.mask = c->mask;
        b.p = {b.transform->position.x + c->offset.x, b.transform->position.y + c->offset.y};
        if (b.rb) {
            b.v = b.rb->velocity;
            b.dynamic = b.rb->type == BodyType2D::Dynamic && !c->trigger;
            b.inv_mass = b.dynamic ? 1.0f / std::max(b.rb->mass, 1e-3f) : 0.0f;
        }
        bodies.push_back(b);
    }
    body_count_ = bodies.size();

    // Integrate velocities and positions.
    for (Body& b : bodies) {
        if (!b.rb || b.rb->type == BodyType2D::Static) continue;
        if (b.dynamic) {
            b.v = b.v + settings.gravity * (b.rb->gravity_scale * dt);
            b.v = b.v * (1.0f / (1.0f + b.rb->linear_damping * dt));
        }
        b.p = b.p + b.v * dt;
    }

    // Tilemaps: their solid cells near each moving body, as static boxes.
    struct MapInfo {
        const TilemapData* map;
        const Tileset* tileset;
        Vec2 origin, cell;
    };
    std::vector<MapInfo> maps;
    if (tiles_.tilemaps && tiles_.tilesets) {
        for (Entity e : [&] {
                 std::vector<Entity> v;
                 EachEntity(world_, [&](Entity x) {
                     if (world_.GetComponent<TilemapRenderer>(x) && world_.GetComponent<Transform>(x)) v.push_back(x);
                 });
                 return v;
             }()) {
            const TilemapRenderer* r = world_.GetComponent<TilemapRenderer>(e);
            const Transform* t = world_.GetComponent<Transform>(e);
            const TilemapData* map = tiles_.tilemaps(r->tilemap.guid);
            const Tileset* ts = map ? tiles_.tilesets(map->tileset) : nullptr;
            if (!map || !ts || r->pixels_per_unit <= 0.0f) continue;
            maps.push_back({map, ts, {t->position.x, t->position.y},
                            {static_cast<f32>(ts->tile_width) / r->pixels_per_unit, static_cast<f32>(ts->tile_height) / r->pixels_per_unit}});
        }
    }
    std::vector<Body> statics; // tile boxes
    struct Candidate {
        int a, b; // b >= bodies.size(): an index into statics, offset
    };
    std::vector<Candidate> candidates;
    const int dynamic_count = static_cast<int>(bodies.size());
    const auto add_tiles_for = [&](int index) {
        const Body& body = bodies[static_cast<usize>(index)];
        const Vec2 lo = body.Min(), hi = body.Max();
        for (const MapInfo& m : maps) {
            const i32 x0 = std::max(0, static_cast<i32>(std::floor((lo.x - m.origin.x) / m.cell.x)));
            const i32 y0 = std::max(0, static_cast<i32>(std::floor((lo.y - m.origin.y) / m.cell.y)));
            const i32 x1 = std::min(static_cast<i32>(m.map->width) - 1, static_cast<i32>(std::floor((hi.x - m.origin.x) / m.cell.x)));
            const i32 y1 = std::min(static_cast<i32>(m.map->height) - 1, static_cast<i32>(std::floor((hi.y - m.origin.y) / m.cell.y)));
            for (i32 y = y0; y <= y1; ++y) {
                for (i32 x = x0; x <= x1; ++x) {
                    if (!IsSolidAt(*m.map, *m.tileset, x, y)) continue;
                    Body t;
                    t.tile = true;
                    t.shape = Shape2D::Box;
                    t.half = {m.cell.x * 0.5f, m.cell.y * 0.5f};
                    t.p = {m.origin.x + (static_cast<f32>(x) + 0.5f) * m.cell.x, m.origin.y + (static_cast<f32>(y) + 0.5f) * m.cell.y};
                    t.friction = 0.5f;
                    t.cx = x;
                    t.cy = y;
                    t.map = m.map;
                    t.tileset = m.tileset;
                    statics.push_back(t);
                    candidates.push_back({index, dynamic_count + static_cast<int>(statics.size()) - 1});
                }
            }
        }
    };
    // Body pairs by a sweep along x, and tile cells for moving bodies.
    std::vector<int> order(bodies.size());
    for (usize i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return bodies[static_cast<usize>(a)].Min().x < bodies[static_cast<usize>(b)].Min().x; });
    for (usize i = 0; i < order.size(); ++i) {
        const Body& a = bodies[static_cast<usize>(order[i])];
        for (usize j = i + 1; j < order.size(); ++j) {
            const Body& b = bodies[static_cast<usize>(order[j])];
            if (b.Min().x > a.Max().x) break;
            if (b.Min().y > a.Max().y || b.Max().y < a.Min().y) continue;
            const bool a_moves = a.rb && a.rb->type != BodyType2D::Static, b_moves = b.rb && b.rb->type != BodyType2D::Static;
            if (!a_moves && !b_moves) continue; // two static things never meet
            if (!Allowed(a, b)) continue;
            // A moving body is `a`, so an event names it first.
            if (!a_moves) candidates.push_back({order[j], order[i]});
            else candidates.push_back({order[i], order[j]});
        }
    }
    for (int i = 0; i < dynamic_count; ++i) {
        const Body& b = bodies[static_cast<usize>(i)];
        if (b.dynamic && !b.trigger) add_tiles_for(i);
        else if (b.trigger && b.rb && b.rb->type != BodyType2D::Static) add_tiles_for(i); // triggers see tiles as contacts too
    }
    const auto body_at = [&](int i) -> Body& {
        return i < dynamic_count ? bodies[static_cast<usize>(i)] : statics[static_cast<usize>(i - dynamic_count)];
    };

    // Resolve: sequential impulses and position correction, re-detecting each pass.
    for (u32 it = 0; it < settings.iterations; ++it) {
        for (const Candidate& c : candidates) {
            Body& a = body_at(c.a);
            Body& b = body_at(c.b);
            if (a.trigger || b.trigger) continue;
            if (a.inv_mass + b.inv_mass == 0.0f) continue;
            Manifold m = Collide(a, b, 0.0f);
            if (!m.hit) continue;
            if (b.tile && InternalFace(b, m.normal * -1.0f)) continue;
            if (a.tile && InternalFace(a, m.normal)) continue;
            const f32 inv_sum = a.inv_mass + b.inv_mass;
            // Position correction.
            const f32 push = std::max(m.depth - settings.slop, 0.0f) * settings.correction / inv_sum;
            a.p = a.p - m.normal * (push * a.inv_mass);
            b.p = b.p + m.normal * (push * b.inv_mass);
            // Normal impulse.
            const Vec2 rv = b.v - a.v;
            const f32 vn = rv.Dot(m.normal);
            if (vn < 0.0f) {
                const f32 e = std::max(a.restitution, b.restitution);
                const f32 bounce = (-vn > 1.0f) ? e : 0.0f; // resting contact doesn't jitter
                const f32 jn = -(1.0f + bounce) * vn / inv_sum;
                a.v = a.v - m.normal * (jn * a.inv_mass);
                b.v = b.v + m.normal * (jn * b.inv_mass);
                // Friction along the tangent, limited by the normal impulse.
                const Vec2 rv2 = b.v - a.v;
                const Vec2 tangent_raw = rv2 - m.normal * rv2.Dot(m.normal);
                const f32 tl = tangent_raw.Length();
                if (tl > 1e-6f) {
                    const Vec2 tangent = tangent_raw * (1.0f / tl);
                    const f32 mu = std::sqrt(a.friction * b.friction);
                    const f32 jt = std::clamp(-rv2.Dot(tangent) / inv_sum, -mu * jn, mu * jn);
                    a.v = a.v - tangent * (jt * a.inv_mass);
                    b.v = b.v + tangent * (jt * b.inv_mass);
                }
            }
        }
    }

    // Write back.
    for (Body& b : bodies) {
        if (!b.rb || b.rb->type == BodyType2D::Static) continue;
        b.transform->position.x = b.p.x - b.offset.x;
        b.transform->position.y = b.p.y - b.offset.y;
        b.rb->velocity = b.v;
    }

    // Contacts for events and grounding: a small margin counts resting touches.
    std::vector<u64> now_pairs, grounded, left_wall, right_wall;
    std::vector<std::pair<u64, Event2D>> info; // by pair key
    for (const Candidate& c : candidates) {
        Body& a = body_at(c.a);
        Body& b = body_at(c.b);
        const bool trigger_pair = a.trigger || b.trigger;
        const Manifold m = Collide(a, b, trigger_pair ? 0.0f : 0.02f);
        if (!m.hit) continue;
        if (!trigger_pair) {
            if (b.tile && InternalFace(b, m.normal * -1.0f)) continue;
            if (a.tile && InternalFace(a, m.normal)) continue;
        }
        const u64 ka = a.tile ? (1ull << 63) : Key(a.e); // all of the tiles are one thing to touch
        const u64 kb = b.tile ? (1ull << 63) : Key(b.e);
        // Grounding: the contact normal (a to b) points up onto a, or down onto b... a body is grounded when
        // something is below it: for a (the lower side of the normal pointing up toward b means b is above a).
        if (!trigger_pair) {
            if (!a.tile && a.dynamic && m.normal.y < -0.7f) grounded.push_back(Key(a.e));
            if (!b.tile && b.dynamic && m.normal.y > 0.7f) grounded.push_back(Key(b.e));
            if (!a.tile && a.dynamic) {
                if (m.normal.x > 0.7f) right_wall.push_back(Key(a.e));
                if (m.normal.x < -0.7f) left_wall.push_back(Key(a.e));
            }
            if (!b.tile && b.dynamic) {
                if (m.normal.x < -0.7f) right_wall.push_back(Key(b.e));
                if (m.normal.x > 0.7f) left_wall.push_back(Key(b.e));
            }
        }
        // A pair's identity; a tile contact is its body with the cell.
        const u64 pk = PairKey(ka, kb, trigger_pair);
        now_pairs.push_back(pk);
        Event2D ev;
        ev.kind = trigger_pair ? Event2D::Kind::TriggerEnter : Event2D::Kind::Begin;
        ev.a = a.tile ? b.e : a.e;
        ev.b = a.tile ? Entity{} : (b.tile ? Entity{} : b.e);
        ev.normal = a.tile ? m.normal * -1.0f : m.normal;
        info.emplace_back(pk, ev);
    }
    std::sort(now_pairs.begin(), now_pairs.end());
    now_pairs.erase(std::unique(now_pairs.begin(), now_pairs.end()), now_pairs.end());
    for (const auto& [pk, ev] : info) {
        if (std::binary_search(pairs_.begin(), pairs_.end(), pk)) continue; // already touching: no new event
        if (std::any_of(events_.begin(), events_.end(), [&](const Event2D& e) { return e.kind == ev.kind && e.a == ev.a && e.b == ev.b; })) continue;
        events_.push_back(ev);
    }
    // Pairs that stopped touching end, with the entities recorded when they began.
    for (const auto& [old, ev] : last_info_) {
        if (std::binary_search(now_pairs.begin(), now_pairs.end(), old)) continue;
        Event2D end = ev;
        end.kind = (old & 1) ? Event2D::Kind::TriggerExit : Event2D::Kind::End;
        events_.push_back(end);
    }
    std::stable_sort(info.begin(), info.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    info.erase(std::unique(info.begin(), info.end(), [](const auto& x, const auto& y) { return x.first == y.first; }), info.end());
    last_info_ = std::move(info);
    pairs_ = std::move(now_pairs);
    for (std::vector<u64>* v : {&grounded, &left_wall, &right_wall}) {
        std::sort(v->begin(), v->end());
        v->erase(std::unique(v->begin(), v->end()), v->end());
    }
    grounded_ = std::move(grounded);
    left_wall_ = std::move(left_wall);
    right_wall_ = std::move(right_wall);
}

bool Physics2D::Raycast(Vec2 origin, Vec2 dir, f32 max_distance, RayHit2D& out, u32 mask) const {
    const f32 len = dir.Length();
    if (len < 1e-9f || max_distance <= 0.0f) return false;
    dir = dir * (1.0f / len);
    f32 best = max_distance;
    bool found = false;
    EachEntity(world_, [&](Entity e) {
        const Transform* t = world_.GetComponent<Transform>(e);
        const Collider2D* c = world_.GetComponent<Collider2D>(e);
        if (!t || !c || c->trigger || (c->layer & mask) == 0) return;
        const Vec2 center{t->position.x + c->offset.x, t->position.y + c->offset.y};
        f32 hit_t = 0;
        Vec2 n{};
        bool hit = false;
        if (c->shape == Shape2D::Box) {
            hit = SlabRay(origin, dir, {center.x - c->half_extents.x, center.y - c->half_extents.y},
                          {center.x + c->half_extents.x, center.y + c->half_extents.y}, best, hit_t, n);
        } else {
            const Vec2 oc = origin - center;
            const f32 b = oc.Dot(dir), cc = oc.Dot(oc) - c->radius * c->radius;
            const f32 disc = b * b - cc;
            if (disc >= 0.0f) {
                const f32 s = std::sqrt(disc);
                f32 tt = -b - s;
                if (tt < 0.0f) tt = -b + s;
                if (tt >= 0.0f && tt <= best) {
                    hit = true;
                    hit_t = tt;
                    const Vec2 p = origin + dir * tt;
                    const Vec2 d = p - center;
                    const f32 dl = d.Length();
                    n = dl > 1e-9f ? d * (1.0f / dl) : Vec2{0, 1};
                }
            }
        }
        if (hit && hit_t <= best) {
            best = hit_t;
            found = true;
            out = {};
            out.entity = e;
            out.distance = hit_t;
            out.point = origin + dir * hit_t;
            out.normal = n;
        }
    });
    if (tiles_.tilemaps && tiles_.tilesets && (mask & 1u) != 0) { // solid tiles are on layer 1
        EachEntity(world_, [&](Entity e) {
            const TilemapRenderer* r = world_.GetComponent<TilemapRenderer>(e);
            const Transform* t = world_.GetComponent<Transform>(e);
            if (!r || !t || r->pixels_per_unit <= 0.0f) return;
            const TilemapData* map = tiles_.tilemaps(r->tilemap.guid);
            const Tileset* ts = map ? tiles_.tilesets(map->tileset) : nullptr;
            if (!map || !ts) return;
            const f32 cw = static_cast<f32>(ts->tile_width) / r->pixels_per_unit, ch = static_cast<f32>(ts->tile_height) / r->pixels_per_unit;
            const Vec2 end = origin + dir * best;
            const i32 x0 = std::max(0, static_cast<i32>(std::floor((std::min(origin.x, end.x) - t->position.x) / cw)));
            const i32 x1 = std::min(static_cast<i32>(map->width) - 1, static_cast<i32>(std::floor((std::max(origin.x, end.x) - t->position.x) / cw)));
            const i32 y0 = std::max(0, static_cast<i32>(std::floor((std::min(origin.y, end.y) - t->position.y) / ch)));
            const i32 y1 = std::min(static_cast<i32>(map->height) - 1, static_cast<i32>(std::floor((std::max(origin.y, end.y) - t->position.y) / ch)));
            for (i32 y = y0; y <= y1; ++y) {
                for (i32 x = x0; x <= x1; ++x) {
                    if (!IsSolidAt(*map, *ts, x, y)) continue;
                    const Vec2 lo{t->position.x + static_cast<f32>(x) * cw, t->position.y + static_cast<f32>(y) * ch};
                    f32 hit_t = 0;
                    Vec2 n{};
                    if (SlabRay(origin, dir, lo, {lo.x + cw, lo.y + ch}, best, hit_t, n) && hit_t <= best) {
                        best = hit_t;
                        found = true;
                        out = {};
                        out.tile = true;
                        out.tile_x = x;
                        out.tile_y = y;
                        out.distance = hit_t;
                        out.point = origin + dir * hit_t;
                        out.normal = n;
                    }
                }
            }
        });
    }
    return found;
}

std::vector<Entity> Physics2D::OverlapBox(Vec2 center, Vec2 half, u32 mask) const {
    std::vector<Entity> result;
    EachEntity(world_, [&](Entity e) {
        const Transform* t = world_.GetComponent<Transform>(e);
        const Collider2D* c = world_.GetComponent<Collider2D>(e);
        if (!t || !c || (c->layer & mask) == 0) return;
        const Vec2 p{t->position.x + c->offset.x, t->position.y + c->offset.y};
        const Vec2 h = c->shape == Shape2D::Box ? c->half_extents : Vec2{c->radius, c->radius};
        if (std::fabs(p.x - center.x) <= h.x + half.x && std::fabs(p.y - center.y) <= h.y + half.y) result.push_back(e);
    });
    return result;
}

void RegisterPhysics2DComponents() {
    (void)GetComponentId<Rigidbody2D>();
    (void)GetComponentId<Collider2D>();
}

} // namespace aether::sprite2d
