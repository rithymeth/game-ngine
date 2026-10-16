#include "aether/sprite2d/physics2d.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace aether::sprite2d {

namespace {

bool IsFinite(Vec2 value) {
    return std::isfinite(value.x) && std::isfinite(value.y);
}
bool IsValidCollider(const Collider2D& collider, const Transform& transform) {
    const Vec2 position{transform.position.x, transform.position.y};
    const Vec2 center = position + collider.offset;
    if (!IsFinite(position) || !IsFinite(collider.offset) || !IsFinite(center) ||
        !std::isfinite(collider.friction) || collider.friction < 0.0f || collider.friction > 2.0f ||
        !std::isfinite(collider.restitution) || collider.restitution < 0.0f || collider.restitution > 1.0f) return false;
    if (collider.shape == Shape2D::Box)
        return IsFinite(collider.half_extents) && collider.half_extents.x >= 0.0f && collider.half_extents.y >= 0.0f;
    if (collider.shape == Shape2D::Circle) return std::isfinite(collider.radius) && collider.radius >= 0.0f;
    return false;
}
bool IsValidRigidbody(const Rigidbody2D& body) {
    if (body.type == BodyType2D::Static) return true;
    if (body.type == BodyType2D::Kinematic) return IsFinite(body.velocity);
    return body.type == BodyType2D::Dynamic && std::isfinite(body.mass) && body.mass > 0.0f &&
           std::isfinite(body.gravity_scale) && std::isfinite(body.linear_damping) &&
           body.linear_damping >= 0.0f && IsFinite(body.velocity);
}
bool TilemapGeometry(const TilemapRenderer& renderer, const Transform& transform, const Tileset& tileset,
                     Vec2& origin, Vec2& cell_size) {
    origin = {transform.position.x, transform.position.y};
    if (!IsFinite(origin) || !std::isfinite(renderer.pixels_per_unit) || renderer.pixels_per_unit <= 0.0f ||
        tileset.tile_width == 0 || tileset.tile_height == 0) return false;
    cell_size = {static_cast<f32>(tileset.tile_width) / renderer.pixels_per_unit,
                 static_cast<f32>(tileset.tile_height) / renderer.pixels_per_unit};
    return IsFinite(cell_size) && cell_size.x > 0.0f && cell_size.y > 0.0f;
}
struct TileRange {
    i32 first = 0;
    i32 last = -1;
    bool Valid() const { return first <= last; }
};
TileRange GetTileRange(f32 world_min, f32 world_max, f32 origin, f32 cell_size, u32 extent) {
    if (extent == 0 || !std::isfinite(world_min) || !std::isfinite(world_max) || world_min > world_max ||
        !std::isfinite(origin) || !std::isfinite(cell_size) || cell_size <= 0.0f) return {};
    const f64 grid_min = (static_cast<f64>(world_min) - origin) / cell_size;
    const f64 grid_max = (static_cast<f64>(world_max) - origin) / cell_size;
    if (!std::isfinite(grid_min) || !std::isfinite(grid_max) || grid_max < 0.0 || grid_min > extent) return {};
    const f64 max_index = static_cast<f64>(std::min<u64>(static_cast<u64>(extent) - 1,
                                                         static_cast<u64>(std::numeric_limits<i32>::max() - 1)));
    const auto clamp_index = [max_index](f64 grid) {
        return static_cast<i32>(std::clamp(std::floor(grid), 0.0, max_index));
    };
    return {clamp_index(grid_min), clamp_index(grid_max)};
}

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
bool RayHitBefore(const RayHit2D& a, const RayHit2D& b) {
    if (a.distance != b.distance) return a.distance < b.distance;
    if (a.tile != b.tile) return !a.tile;
    if (a.tile) {
        if (a.tile_y != b.tile_y) return a.tile_y < b.tile_y;
        return a.tile_x < b.tile_x;
    }
    return Key(a.entity) < Key(b.entity);
}
void SortEntities(std::vector<Entity>& entities) {
    std::sort(entities.begin(), entities.end(), [](Entity a, Entity b) { return Key(a) < Key(b); });
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
        m.depth = px + r;
    } else {
        m.normal = {0.0f, d.y < 0.0f ? 1.0f : -1.0f};
        m.depth = py + r;
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
    if (t0 == 0.0f && n.Dot(n) == 0.0f) {
        const f32 left = origin.x - lo.x, right = hi.x - origin.x;
        const f32 bottom = origin.y - lo.y, top = hi.y - origin.y;
        const f32 nearest = std::min({left, right, bottom, top});
        if (nearest == left) n = {-1, 0};
        else if (nearest == right) n = {1, 0};
        else if (nearest == bottom) n = {0, -1};
        else n = {0, 1};
    }
    normal = n;
    return true;
}

bool CircleCastAabb(Vec2 origin, Vec2 dir, f32 radius, Vec2 lo, Vec2 hi, f32 max_t,
                    f32& t_out, Vec2& normal, Vec2& point_out) {
    f32 best = max_t;
    bool found = false;
    auto consider = [&](f32 t, Vec2 n, Vec2 point) {
        if (t < 0.0f || t > best) return;
        best = t;
        t_out = t;
        normal = n;
        point_out = point;
        found = true;
    };

    Vec2 closest{std::clamp(origin.x, lo.x, hi.x), std::clamp(origin.y, lo.y, hi.y)};
    const Vec2 initial_delta = origin - closest;
    if (initial_delta.Dot(initial_delta) <= radius * radius) {
        f32 normal_length = initial_delta.Length();
        if (normal_length > 1e-9f) {
            normal = initial_delta * (1.0f / normal_length);
        } else {
            const f32 left = origin.x - lo.x, right = hi.x - origin.x;
            const f32 bottom = origin.y - lo.y, top = hi.y - origin.y;
            const f32 nearest = std::min({left, right, bottom, top});
            if (nearest == left) { normal = {-1, 0}; closest.x = lo.x; }
            else if (nearest == right) { normal = {1, 0}; closest.x = hi.x; }
            else if (nearest == bottom) { normal = {0, -1}; closest.y = lo.y; }
            else { normal = {0, 1}; closest.y = hi.y; }
        }
        t_out = 0.0f;
        point_out = closest;
        return true;
    }

    auto side = [&](f32 plane, f32 start, f32 speed, f32 min_tangent, f32 max_tangent,
                    bool vertical, f32 sign) {
        if (std::fabs(speed) < 1e-9f) return;
        const f32 t = (plane - start) / speed;
        if (t < 0.0f || t > best) return;
        const Vec2 center = origin + dir * t;
        const f32 tangent = vertical ? center.y : center.x;
        if (tangent < min_tangent || tangent > max_tangent) return;
        const Vec2 n = vertical ? Vec2{sign, 0} : Vec2{0, sign};
        consider(t, n, center - n * radius);
    };
    if (dir.x > 0.0f) side(lo.x - radius, origin.x, dir.x, lo.y, hi.y, true, -1.0f);
    if (dir.x < 0.0f) side(hi.x + radius, origin.x, dir.x, lo.y, hi.y, true, 1.0f);
    if (dir.y > 0.0f) side(lo.y - radius, origin.y, dir.y, lo.x, hi.x, false, -1.0f);
    if (dir.y < 0.0f) side(hi.y + radius, origin.y, dir.y, lo.x, hi.x, false, 1.0f);

    const Vec2 corners[] = {lo, {lo.x, hi.y}, {hi.x, lo.y}, hi};
    for (usize i = 0; i < 4; ++i) {
        const Vec2 offset = origin - corners[i];
        const f32 b = offset.Dot(dir);
        const f32 c = offset.Dot(offset) - radius * radius;
        const f32 discriminant = b * b - dir.Dot(dir) * c;
        if (discriminant < 0.0f || dir.Dot(dir) <= 1e-18f) continue;
        const f32 t = (-b - std::sqrt(discriminant)) / dir.Dot(dir);
        if (t < 0.0f || t > best) continue;
        const Vec2 center = origin + dir * t;
        const bool in_corner_region = (i == 0 && center.x <= lo.x && center.y <= lo.y) ||
                                      (i == 1 && center.x <= lo.x && center.y >= hi.y) ||
                                      (i == 2 && center.x >= hi.x && center.y <= lo.y) ||
                                      (i == 3 && center.x >= hi.x && center.y >= hi.y);
        if (!in_corner_region) continue;
        const Vec2 delta = center - corners[i];
        const f32 length = delta.Length();
        if (length > 1e-9f) consider(t, delta * (1.0f / length), corners[i]);
    }
    return found;
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
    if (!std::isfinite(dt) || dt <= 0.0f || !std::isfinite(settings.max_step) || settings.max_step <= 0.0f ||
        !IsFinite(settings.gravity) || !std::isfinite(settings.slop) || settings.slop < 0.0f ||
        !std::isfinite(settings.correction) || settings.correction < 0.0f || settings.correction > 1.0f) return;
    dt = std::min(dt, settings.max_step);
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
        if (!IsValidCollider(*c, *b.transform) || (b.rb && !IsValidRigidbody(*b.rb))) continue;
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
            b.dynamic = b.rb->type == BodyType2D::Dynamic;
            b.inv_mass = b.dynamic && !c->trigger ? 1.0f / std::max(b.rb->mass, 1e-3f) : 0.0f;
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
            Vec2 map_origin{}, cell_size{};
            if (!map || !ts || !TilemapGeometry(*r, *t, *ts, map_origin, cell_size)) continue;
            maps.push_back({map, ts, map_origin, cell_size});
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
        if (body.layer == 0 || (body.mask & 1u) == 0) return; // tilemaps use layer 1 and accept every nonzero layer
        const Vec2 lo = body.Min(), hi = body.Max();
        for (const MapInfo& m : maps) {
            const TileRange x_range = GetTileRange(lo.x, hi.x, m.origin.x, m.cell.x, m.map->width);
            const TileRange y_range = GetTileRange(lo.y, hi.y, m.origin.y, m.cell.y, m.map->height);
            if (!x_range.Valid() || !y_range.Valid()) continue;
            for (i32 y = y_range.first; y <= y_range.last; ++y) {
                for (i32 x = x_range.first; x <= x_range.last; ++x) {
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
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        const Body& body_a = bodies[static_cast<usize>(a)];
        const Body& body_b = bodies[static_cast<usize>(b)];
        const f32 min_a = body_a.Min().x, min_b = body_b.Min().x;
        return min_a == min_b ? Key(body_a.e) < Key(body_b.e) : min_a < min_b;
    });
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
        if (b.rb && b.rb->type != BodyType2D::Static) add_tiles_for(i); // moving and trigger bodies detect solid tile contacts
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
    std::vector<PairIdentity> now_pairs;
    std::vector<u64> grounded, left_wall, right_wall;
    std::vector<std::pair<PairIdentity, Event2D>> info;
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
        // Store both exact entity keys and whether either side represents the aggregate tile contact.
        // Canonical ordering keeps the pair stable when candidate order changes between frames.
        u64 first = a.tile ? 0 : Key(a.e), second = b.tile ? 0 : Key(b.e);
        bool first_tile = a.tile, second_tile = b.tile;
        if (first_tile != second_tile ? first_tile : first > second) {
            std::swap(first, second);
            std::swap(first_tile, second_tile);
        }
        const PairIdentity identity{first, second, first_tile, second_tile, trigger_pair};
        now_pairs.push_back(identity);
        Event2D ev;
        ev.kind = trigger_pair ? Event2D::Kind::TriggerEnter : Event2D::Kind::Begin;
        ev.a = a.tile ? b.e : a.e;
        ev.b = a.tile ? Entity{} : (b.tile ? Entity{} : b.e);
        ev.normal = a.tile ? m.normal * -1.0f : m.normal;
        info.emplace_back(identity, ev);
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
        end.kind = old.trigger ? Event2D::Kind::TriggerExit : Event2D::Kind::End;
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
    if (!IsFinite(origin) || !IsFinite(dir) || !std::isfinite(len) || !std::isfinite(max_distance) ||
        len < 1e-9f || max_distance <= 0.0f) return false;
    dir = dir * (1.0f / len);
    f32 best = max_distance;
    bool found = false;
    EachEntity(world_, [&](Entity e) {
        const Transform* t = world_.GetComponent<Transform>(e);
        const Collider2D* c = world_.GetComponent<Collider2D>(e);
        if (!t || !c || !IsValidCollider(*c, *t) || c->trigger || (c->layer & mask) == 0) return;
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
            if (cc <= 0.0f) {
                hit = true;
                hit_t = 0.0f;
                const f32 distance = oc.Length();
                n = distance > 1e-9f ? oc * (1.0f / distance) : Vec2{0.0f, 1.0f};
            } else if (disc >= 0.0f) {
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
            RayHit2D candidate;
            candidate.entity = e;
            candidate.distance = hit_t;
            candidate.point = origin + dir * hit_t;
            candidate.normal = n;
            if (!found || RayHitBefore(candidate, out)) {
                best = hit_t;
                found = true;
                out = candidate;
            }
        }
    });
    if (tiles_.tilemaps && tiles_.tilesets && (mask & 1u) != 0) { // solid tiles are on layer 1
        EachEntity(world_, [&](Entity e) {
            const TilemapRenderer* r = world_.GetComponent<TilemapRenderer>(e);
            const Transform* t = world_.GetComponent<Transform>(e);
            if (!r || !t) return;
            const TilemapData* map = tiles_.tilemaps(r->tilemap.guid);
            const Tileset* ts = map ? tiles_.tilesets(map->tileset) : nullptr;
            if (!map || !ts) return;
            Vec2 map_origin{}, cell_size{};
            if (!TilemapGeometry(*r, *t, *ts, map_origin, cell_size)) return;
            const f32 cw = cell_size.x, ch = cell_size.y;
            const Vec2 end = origin + dir * best;
            const TileRange x_range = GetTileRange(std::min(origin.x, end.x), std::max(origin.x, end.x),
                                                   map_origin.x, cw, map->width);
            const TileRange y_range = GetTileRange(std::min(origin.y, end.y), std::max(origin.y, end.y),
                                                   map_origin.y, ch, map->height);
            if (!x_range.Valid() || !y_range.Valid()) return;
            for (i32 y = y_range.first; y <= y_range.last; ++y) {
                for (i32 x = x_range.first; x <= x_range.last; ++x) {
                    if (!IsSolidAt(*map, *ts, x, y)) continue;
                    const Vec2 lo{map_origin.x + static_cast<f32>(x) * cw, map_origin.y + static_cast<f32>(y) * ch};
                    f32 hit_t = 0;
                    Vec2 n{};
                    if (SlabRay(origin, dir, lo, {lo.x + cw, lo.y + ch}, best, hit_t, n) && hit_t <= best) {
                        RayHit2D candidate;
                        candidate.tile = true;
                        candidate.tile_x = x;
                        candidate.tile_y = y;
                        candidate.distance = hit_t;
                        candidate.point = origin + dir * hit_t;
                        candidate.normal = n;
                        if (!found || RayHitBefore(candidate, out)) {
                            best = hit_t;
                            found = true;
                            out = candidate;
                        }
                    }
                }
            }
        });
    }
    return found;
}

std::vector<RayHit2D> Physics2D::RaycastAll(Vec2 origin, Vec2 dir, f32 max_distance, u32 mask, usize max_hits) const {
    std::vector<RayHit2D> hits;
    const f32 direction_length = dir.Length();
    if (max_hits == 0 || !IsFinite(origin) || !IsFinite(dir) || !std::isfinite(direction_length) ||
        !std::isfinite(max_distance) || direction_length < 1e-9f || max_distance < 0.0f) return hits;
    dir = dir * (1.0f / direction_length);
    EachEntity(world_, [&](Entity e) {
        const Transform* transform = world_.GetComponent<Transform>(e);
        const Collider2D* collider = world_.GetComponent<Collider2D>(e);
        if (!transform || !collider || !IsValidCollider(*collider, *transform) || collider->trigger ||
            (collider->layer & mask) == 0) return;
        const Vec2 center{transform->position.x + collider->offset.x,
                          transform->position.y + collider->offset.y};
        f32 t = 0.0f;
        Vec2 normal{};
        bool did_hit = false;
        if (collider->shape == Shape2D::Box) {
            did_hit = SlabRay(origin, dir,
                              {center.x - collider->half_extents.x, center.y - collider->half_extents.y},
                              {center.x + collider->half_extents.x, center.y + collider->half_extents.y},
                              max_distance, t, normal);
        } else {
            const Vec2 offset = origin - center;
            const f32 a = dir.Dot(dir), b = offset.Dot(dir), c = offset.Dot(offset) - collider->radius * collider->radius;
            const f32 discriminant = b * b - a * c;
            if (c <= 0.0f) {
                did_hit = true;
                t = 0.0f;
            } else if (discriminant >= 0.0f) {
                t = (-b - std::sqrt(discriminant)) / a;
                did_hit = t >= 0.0f && t <= max_distance;
            }
            if (did_hit) {
                Vec2 delta = origin + dir * t - center;
                const f32 length = delta.Length();
                normal = length > 1e-9f ? delta * (1.0f / length) : Vec2{0.0f, 1.0f};
            }
        }
        if (!did_hit || t < 0.0f || t > max_distance) return;
        RayHit2D hit;
        hit.entity = e;
        hit.distance = t;
        hit.point = origin + dir * t;
        hit.normal = normal;
        hits.push_back(hit);
    });

    if (tiles_.tilemaps && tiles_.tilesets && (mask & 1u) != 0) {
        const Vec2 end = origin + dir * max_distance;
        EachEntity(world_, [&](Entity e) {
            const TilemapRenderer* renderer = world_.GetComponent<TilemapRenderer>(e);
            const Transform* transform = world_.GetComponent<Transform>(e);
            if (!renderer || !transform) return;
            const TilemapData* map = tiles_.tilemaps(renderer->tilemap.guid);
            const Tileset* tileset = map ? tiles_.tilesets(map->tileset) : nullptr;
            if (!map || !tileset) return;
            Vec2 map_origin{}, cell_size{};
            if (!TilemapGeometry(*renderer, *transform, *tileset, map_origin, cell_size)) return;
            const f32 cell_width = cell_size.x, cell_height = cell_size.y;
            const TileRange x_range = GetTileRange(std::min(origin.x, end.x), std::max(origin.x, end.x),
                                                   map_origin.x, cell_width, map->width);
            const TileRange y_range = GetTileRange(std::min(origin.y, end.y), std::max(origin.y, end.y),
                                                   map_origin.y, cell_height, map->height);
            if (!x_range.Valid() || !y_range.Valid()) return;
            for (i32 y = y_range.first; y <= y_range.last; ++y) {
                for (i32 x = x_range.first; x <= x_range.last; ++x) {
                    if (!IsSolidAt(*map, *tileset, x, y)) continue;
                    const Vec2 lo{map_origin.x + static_cast<f32>(x) * cell_width,
                                  map_origin.y + static_cast<f32>(y) * cell_height};
                    f32 t = 0.0f;
                    Vec2 normal{};
                    if (!SlabRay(origin, dir, lo, {lo.x + cell_width, lo.y + cell_height},
                                 max_distance, t, normal)) continue;
                    RayHit2D hit;
                    hit.tile = true;
                    hit.tile_x = x;
                    hit.tile_y = y;
                    hit.distance = t;
                    hit.point = origin + dir * t;
                    hit.normal = normal;
                    hits.push_back(hit);
                }
            }
        });
    }
    std::sort(hits.begin(), hits.end(), RayHitBefore);
    if (hits.size() > max_hits) hits.resize(max_hits);
    return hits;
}

bool Physics2D::CircleCast(Vec2 origin, Vec2 dir, f32 radius, f32 max_distance, RayHit2D& out, u32 mask) const {
    const f32 direction_length = dir.Length();
    if (!IsFinite(origin) || !IsFinite(dir) || !std::isfinite(radius) || !std::isfinite(max_distance) ||
        radius <= 0.0f || max_distance < 0.0f || !std::isfinite(direction_length) || direction_length < 1e-9f) return false;
    dir = dir * (1.0f / direction_length);
    f32 best = max_distance;
    bool found = false;
    auto set_hit = [&](f32 t, Vec2 point, Vec2 normal, Entity entity, bool tile, i32 tile_x, i32 tile_y) {
        if (t < 0.0f || t > best) return;
        RayHit2D candidate;
        candidate.entity = entity;
        candidate.tile = tile;
        candidate.tile_x = tile_x;
        candidate.tile_y = tile_y;
        candidate.distance = t;
        candidate.point = point;
        candidate.normal = normal;
        if (!found && t <= best) {
            best = t;
            found = true;
            out = candidate;
        } else if (found && RayHitBefore(candidate, out)) {
            best = t;
            out = candidate;
        }
    };

    EachEntity(world_, [&](Entity e) {
        const Transform* transform = world_.GetComponent<Transform>(e);
        const Collider2D* collider = world_.GetComponent<Collider2D>(e);
        if (!transform || !collider || !IsValidCollider(*collider, *transform) || collider->trigger ||
            (collider->layer & mask) == 0) return;
        const Vec2 center{transform->position.x + collider->offset.x,
                          transform->position.y + collider->offset.y};
        if (collider->shape == Shape2D::Box) {
            f32 t = 0.0f;
            Vec2 normal{}, point{};
            if (CircleCastAabb(origin, dir, radius,
                               {center.x - collider->half_extents.x, center.y - collider->half_extents.y},
                               {center.x + collider->half_extents.x, center.y + collider->half_extents.y},
                               best, t, normal, point)) {
                set_hit(t, point, normal, e, false, 0, 0);
            }
            return;
        }

        const f32 combined_radius = radius + collider->radius;
        const Vec2 offset = origin - center;
        const f32 a = dir.Dot(dir), b = offset.Dot(dir), c = offset.Dot(offset) - combined_radius * combined_radius;
        const f32 discriminant = b * b - a * c;
        f32 t = 0.0f;
        if (c <= 0.0f) {
            t = 0.0f;
        } else if (discriminant >= 0.0f) {
            t = (-b - std::sqrt(discriminant)) / a;
        } else {
            return;
        }
        if (t < 0.0f || t > best) return;
        const Vec2 cast_center = origin + dir * t;
        Vec2 normal = cast_center - center;
        const f32 normal_length = normal.Length();
        normal = normal_length > 1e-9f ? normal * (1.0f / normal_length) : Vec2{0.0f, 1.0f};
        set_hit(t, center + normal * collider->radius, normal, e, false, 0, 0);
    });

    if (tiles_.tilemaps && tiles_.tilesets && (mask & 1u) != 0) {
        const Vec2 end = origin + dir * best;
        EachEntity(world_, [&](Entity e) {
            const TilemapRenderer* renderer = world_.GetComponent<TilemapRenderer>(e);
            const Transform* transform = world_.GetComponent<Transform>(e);
            if (!renderer || !transform) return;
            const TilemapData* map = tiles_.tilemaps(renderer->tilemap.guid);
            const Tileset* tileset = map ? tiles_.tilesets(map->tileset) : nullptr;
            if (!map || !tileset) return;
            Vec2 map_origin{}, cell_size{};
            if (!TilemapGeometry(*renderer, *transform, *tileset, map_origin, cell_size)) return;
            const f32 cell_width = cell_size.x, cell_height = cell_size.y;
            const TileRange x_range = GetTileRange(std::min(origin.x, end.x) - radius,
                                                   std::max(origin.x, end.x) + radius,
                                                   map_origin.x, cell_width, map->width);
            const TileRange y_range = GetTileRange(std::min(origin.y, end.y) - radius,
                                                   std::max(origin.y, end.y) + radius,
                                                   map_origin.y, cell_height, map->height);
            if (!x_range.Valid() || !y_range.Valid()) return;
            for (i32 y = y_range.first; y <= y_range.last; ++y) {
                for (i32 x = x_range.first; x <= x_range.last; ++x) {
                    if (!IsSolidAt(*map, *tileset, x, y)) continue;
                    const Vec2 lo{map_origin.x + static_cast<f32>(x) * cell_width,
                                  map_origin.y + static_cast<f32>(y) * cell_height};
                    f32 t = 0.0f;
                    Vec2 normal{}, point{};
                    if (CircleCastAabb(origin, dir, radius, lo, {lo.x + cell_width, lo.y + cell_height},
                                       best, t, normal, point)) {
                        set_hit(t, point, normal, {}, true, x, y);
                    }
                }
            }
        });
    }
    return found;
}

bool Physics2D::BoxCast(Vec2 origin, Vec2 half, Vec2 dir, f32 max_distance, RayHit2D& out, u32 mask) const {
    const f32 direction_length = dir.Length();
    if (!IsFinite(origin) || !IsFinite(half) || !IsFinite(dir) || !std::isfinite(max_distance) ||
        half.x <= 0.0f || half.y <= 0.0f || max_distance < 0.0f ||
        !std::isfinite(direction_length) || direction_length < 1e-9f) return false;
    dir = dir * (1.0f / direction_length);
    f32 best = max_distance;
    bool found = false;
    auto set_hit = [&](f32 t, Vec2 point, Vec2 normal, Entity entity, bool tile, i32 tile_x, i32 tile_y) {
        if (t < 0.0f || t > best) return;
        RayHit2D candidate;
        candidate.entity = entity;
        candidate.tile = tile;
        candidate.tile_x = tile_x;
        candidate.tile_y = tile_y;
        candidate.distance = t;
        candidate.point = point;
        candidate.normal = normal;
        if (!found) {
            best = t;
            found = true;
            out = candidate;
        } else if (RayHitBefore(candidate, out)) {
            best = t;
            out = candidate;
        }
    };
    auto box_hit_point = [&](Vec2 cast_center, Vec2 target_lo, Vec2 target_hi, Vec2 normal) {
        if (normal.x < -0.5f) return Vec2{target_lo.x, std::clamp(cast_center.y, target_lo.y, target_hi.y)};
        if (normal.x > 0.5f) return Vec2{target_hi.x, std::clamp(cast_center.y, target_lo.y, target_hi.y)};
        if (normal.y < -0.5f) return Vec2{std::clamp(cast_center.x, target_lo.x, target_hi.x), target_lo.y};
        if (normal.y > 0.5f) return Vec2{std::clamp(cast_center.x, target_lo.x, target_hi.x), target_hi.y};
        return Vec2{std::clamp(cast_center.x, target_lo.x, target_hi.x),
                    std::clamp(cast_center.y, target_lo.y, target_hi.y)};
    };
    auto sweep_box_target = [&](Vec2 target_lo, Vec2 target_hi, f32 t_limit, f32& t, Vec2& normal, Vec2& point) {
        const Vec2 expanded_lo{target_lo.x - half.x, target_lo.y - half.y};
        const Vec2 expanded_hi{target_hi.x + half.x, target_hi.y + half.y};
        if (!SlabRay(origin, dir, expanded_lo, expanded_hi, t_limit, t, normal)) return false;
        const Vec2 cast_center = origin + dir * t;
        point = box_hit_point(cast_center, target_lo, target_hi, normal);
        return true;
    };

    EachEntity(world_, [&](Entity e) {
        const Transform* transform = world_.GetComponent<Transform>(e);
        const Collider2D* collider = world_.GetComponent<Collider2D>(e);
        if (!transform || !collider || !IsValidCollider(*collider, *transform) || collider->trigger ||
            (collider->layer & mask) == 0) return;
        const Vec2 center{transform->position.x + collider->offset.x,
                          transform->position.y + collider->offset.y};
        f32 t = 0.0f;
        Vec2 normal{}, point{};
        if (collider->shape == Shape2D::Box) {
            if (!sweep_box_target({center.x - collider->half_extents.x, center.y - collider->half_extents.y},
                                  {center.x + collider->half_extents.x, center.y + collider->half_extents.y},
                                  best, t, normal, point)) return;
        } else {
            if (!CircleCastAabb(origin, dir, collider->radius,
                                {center.x - half.x, center.y - half.y},
                                {center.x + half.x, center.y + half.y}, best, t, normal, point)) return;
            point = center + normal * collider->radius;
        }
        set_hit(t, point, normal, e, false, 0, 0);
    });

    if (tiles_.tilemaps && tiles_.tilesets && (mask & 1u) != 0) {
        const Vec2 end = origin + dir * best;
        EachEntity(world_, [&](Entity e) {
            const TilemapRenderer* renderer = world_.GetComponent<TilemapRenderer>(e);
            const Transform* transform = world_.GetComponent<Transform>(e);
            if (!renderer || !transform) return;
            const TilemapData* map = tiles_.tilemaps(renderer->tilemap.guid);
            const Tileset* tileset = map ? tiles_.tilesets(map->tileset) : nullptr;
            if (!map || !tileset) return;
            Vec2 map_origin{}, cell_size{};
            if (!TilemapGeometry(*renderer, *transform, *tileset, map_origin, cell_size)) return;
            const f32 cell_width = cell_size.x, cell_height = cell_size.y;
            const TileRange x_range = GetTileRange(std::min(origin.x, end.x) - half.x,
                                                   std::max(origin.x, end.x) + half.x,
                                                   map_origin.x, cell_width, map->width);
            const TileRange y_range = GetTileRange(std::min(origin.y, end.y) - half.y,
                                                   std::max(origin.y, end.y) + half.y,
                                                   map_origin.y, cell_height, map->height);
            if (!x_range.Valid() || !y_range.Valid()) return;
            for (i32 y = y_range.first; y <= y_range.last; ++y) {
                for (i32 x = x_range.first; x <= x_range.last; ++x) {
                    if (!IsSolidAt(*map, *tileset, x, y)) continue;
                    const Vec2 lo{map_origin.x + static_cast<f32>(x) * cell_width,
                                  map_origin.y + static_cast<f32>(y) * cell_height};
                    const Vec2 hi{lo.x + cell_width, lo.y + cell_height};
                    f32 t = 0.0f;
                    Vec2 normal{}, point{};
                    if (sweep_box_target(lo, hi, best, t, normal, point)) set_hit(t, point, normal, {}, true, x, y);
                }
            }
        });
    }
    return found;
}

std::vector<Entity> Physics2D::OverlapBox(Vec2 center, Vec2 half, u32 mask) const {
    std::vector<Entity> result;
    if (!IsFinite(center) || !IsFinite(half) || half.x < 0.0f || half.y < 0.0f) return result;
    EachEntity(world_, [&](Entity e) {
        const Transform* t = world_.GetComponent<Transform>(e);
        const Collider2D* c = world_.GetComponent<Collider2D>(e);
        if (!t || !c || !IsValidCollider(*c, *t) || (c->layer & mask) == 0) return;
        const Vec2 p{t->position.x + c->offset.x, t->position.y + c->offset.y};
        if (c->shape == Shape2D::Circle) {
            const Vec2 closest{std::clamp(p.x, center.x - half.x, center.x + half.x),
                               std::clamp(p.y, center.y - half.y, center.y + half.y)};
            const Vec2 delta = p - closest;
            if (delta.Dot(delta) <= c->radius * c->radius) result.push_back(e);
            return;
        }
        if (std::fabs(p.x - center.x) <= c->half_extents.x + half.x &&
            std::fabs(p.y - center.y) <= c->half_extents.y + half.y) result.push_back(e);
    });
    SortEntities(result);
    return result;
}

std::vector<Entity> Physics2D::OverlapCircle(Vec2 center, f32 radius, u32 mask) const {
    std::vector<Entity> result;
    if (!IsFinite(center) || !std::isfinite(radius) || radius < 0.0f) return result;
    const f32 radius_sq = radius * radius;
    EachEntity(world_, [&](Entity e) {
        const Transform* t = world_.GetComponent<Transform>(e);
        const Collider2D* c = world_.GetComponent<Collider2D>(e);
        if (!t || !c || !IsValidCollider(*c, *t) || (c->layer & mask) == 0) return;
        const Vec2 p{t->position.x + c->offset.x, t->position.y + c->offset.y};
        if (c->shape == Shape2D::Circle) {
            const Vec2 delta = p - center;
            const f32 combined = radius + c->radius;
            if (delta.Dot(delta) <= combined * combined) result.push_back(e);
        } else {
            const Vec2 closest{std::clamp(center.x, p.x - c->half_extents.x, p.x + c->half_extents.x),
                               std::clamp(center.y, p.y - c->half_extents.y, p.y + c->half_extents.y)};
            const Vec2 delta = closest - center;
            if (delta.Dot(delta) <= radius_sq) result.push_back(e);
        }
    });
    SortEntities(result);
    return result;
}

std::vector<Entity> Physics2D::OverlapOrientedBox(Vec2 center, Vec2 half, f32 radians, u32 mask) const {
    std::vector<Entity> result;
    if (!IsFinite(center) || !IsFinite(half) || !std::isfinite(radians) || half.x < 0.0f || half.y < 0.0f) return result;
    const f32 c = std::cos(radians), s = std::sin(radians);
    const Vec2 axis_x{c, s}, axis_y{-s, c};
    EachEntity(world_, [&](Entity e) {
        const Transform* t = world_.GetComponent<Transform>(e);
        const Collider2D* collider = world_.GetComponent<Collider2D>(e);
        if (!t || !collider || !IsValidCollider(*collider, *t) || (collider->layer & mask) == 0) return;
        const Vec2 p{t->position.x + collider->offset.x, t->position.y + collider->offset.y};
        const Vec2 delta = p - center;
        if (collider->shape == Shape2D::Circle) {
            const f32 local_x = delta.Dot(axis_x);
            const f32 local_y = delta.Dot(axis_y);
            const f32 closest_x = std::clamp(local_x, -half.x, half.x);
            const f32 closest_y = std::clamp(local_y, -half.y, half.y);
            const f32 dx = local_x - closest_x, dy = local_y - closest_y;
            if (dx * dx + dy * dy <= collider->radius * collider->radius) result.push_back(e);
            return;
        }

        const Vec2 box_half = collider->half_extents;
        if (std::fabs(delta.x) > half.x * std::fabs(axis_x.x) + half.y * std::fabs(axis_y.x) + box_half.x ||
            std::fabs(delta.y) > half.x * std::fabs(axis_x.y) + half.y * std::fabs(axis_y.y) + box_half.y ||
            std::fabs(delta.Dot(axis_x)) > half.x + box_half.x * std::fabs(axis_x.x) + box_half.y * std::fabs(axis_x.y) ||
            std::fabs(delta.Dot(axis_y)) > half.y + box_half.x * std::fabs(axis_y.x) + box_half.y * std::fabs(axis_y.y)) return;
        result.push_back(e);
    });
    SortEntities(result);
    return result;
}

std::vector<Entity> Physics2D::OverlapCapsule(Vec2 point_a, Vec2 point_b, f32 radius, u32 mask) const {
    std::vector<Entity> result;
    if (!IsFinite(point_a) || !IsFinite(point_b) || !std::isfinite(radius) || radius < 0.0f) return result;
    const Vec2 segment = point_b - point_a;
    const f32 segment_length_sq = segment.Dot(segment);
    auto distance_sq_to_segment = [&](Vec2 point) {
        const f32 t = segment_length_sq > 0.0f
                          ? std::clamp((point - point_a).Dot(segment) / segment_length_sq, 0.0f, 1.0f)
                          : 0.0f;
        const Vec2 delta = point - (point_a + segment * t);
        return delta.Dot(delta);
    };
    auto point_box_distance_sq = [](Vec2 point, Vec2 box_center, Vec2 half) {
        const f32 dx = std::max(std::fabs(point.x - box_center.x) - half.x, 0.0f);
        const f32 dy = std::max(std::fabs(point.y - box_center.y) - half.y, 0.0f);
        return dx * dx + dy * dy;
    };
    const f32 radius_sq = radius * radius;
    EachEntity(world_, [&](Entity e) {
        const Transform* transform = world_.GetComponent<Transform>(e);
        const Collider2D* collider = world_.GetComponent<Collider2D>(e);
        if (!transform || !collider || !IsValidCollider(*collider, *transform) || (collider->layer & mask) == 0) return;
        const Vec2 center{transform->position.x + collider->offset.x,
                          transform->position.y + collider->offset.y};
        if (collider->shape == Shape2D::Circle) {
            const f32 combined_radius = radius + collider->radius;
            if (distance_sq_to_segment(center) <= combined_radius * combined_radius) result.push_back(e);
            return;
        }

        const Vec2 half = collider->half_extents;
        const Vec2 lo{center.x - half.x, center.y - half.y};
        const Vec2 hi{center.x + half.x, center.y + half.y};
        const f32 min_x = std::min(point_a.x, point_b.x), max_x = std::max(point_a.x, point_b.x);
        const f32 min_y = std::min(point_a.y, point_b.y), max_y = std::max(point_a.y, point_b.y);
        bool intersects_box = max_x >= lo.x && min_x <= hi.x && max_y >= lo.y && min_y <= hi.y;
        if (intersects_box) {
            f32 t_min = 0.0f, t_max = 1.0f;
            auto clip_axis = [&](f32 start, f32 delta, f32 min_value, f32 max_value) {
                if (std::fabs(delta) < 1e-9f) return start >= min_value && start <= max_value;
                f32 t0 = (min_value - start) / delta, t1 = (max_value - start) / delta;
                if (t0 > t1) std::swap(t0, t1);
                t_min = std::max(t_min, t0);
                t_max = std::min(t_max, t1);
                return t_min <= t_max;
            };
            intersects_box = clip_axis(point_a.x, segment.x, lo.x, hi.x) &&
                             clip_axis(point_a.y, segment.y, lo.y, hi.y);
        }
        if (intersects_box || point_box_distance_sq(point_a, center, half) <= radius_sq ||
            point_box_distance_sq(point_b, center, half) <= radius_sq ||
            distance_sq_to_segment(lo) <= radius_sq || distance_sq_to_segment({lo.x, hi.y}) <= radius_sq ||
            distance_sq_to_segment({hi.x, lo.y}) <= radius_sq || distance_sq_to_segment(hi) <= radius_sq) {
            result.push_back(e);
        }
    });
    SortEntities(result);
    return result;
}

std::vector<Entity> Physics2D::OverlapPoint(Vec2 point, u32 mask) const {
    std::vector<Entity> result;
    if (!IsFinite(point)) return result;
    EachEntity(world_, [&](Entity e) {
        const Transform* transform = world_.GetComponent<Transform>(e);
        const Collider2D* collider = world_.GetComponent<Collider2D>(e);
        if (!transform || !collider || !IsValidCollider(*collider, *transform) || (collider->layer & mask) == 0) return;
        const Vec2 center{transform->position.x + collider->offset.x,
                          transform->position.y + collider->offset.y};
        if (collider->shape == Shape2D::Circle) {
            const Vec2 delta = point - center;
            if (delta.Dot(delta) <= collider->radius * collider->radius) result.push_back(e);
        } else if (std::fabs(point.x - center.x) <= collider->half_extents.x &&
                   std::fabs(point.y - center.y) <= collider->half_extents.y) {
            result.push_back(e);
        }
    });
    SortEntities(result);
    return result;
}

void RegisterPhysics2DComponents() {
    (void)GetComponentId<Rigidbody2D>();
    (void)GetComponentId<Collider2D>();
}

} // namespace aether::sprite2d
