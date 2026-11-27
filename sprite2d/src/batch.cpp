#include "aether/sprite2d/batch.h"
#include "aether/scene/hierarchy.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace aether::sprite2d {

namespace {

struct Item {
    i32 layer, order;
    f32 z;
    usize sequence;
    assets::AssetGuid texture;
    std::vector<SpriteVertex> vertices; // quads of four
};

// Rotates (x, y) by the transform's rotation about z, from the quaternion.
struct PlaneRotation {
    f32 c = 1, s = 0;
    explicit PlaneRotation(const Quaternion& q) {
        const f32 angle = 2.0f * std::atan2(q.z, q.w);
        c = std::cos(angle);
        s = std::sin(angle);
    }
    void Apply(f32 x, f32 y, f32& ox, f32& oy) const {
        ox = x * c - y * s;
        oy = x * s + y * c;
    }
};

struct Rect {
    f32 min_x, min_y, max_x, max_y;
};

bool Visible(const ViewRect* view, const Rect& r) {
    return !view || (r.max_x >= view->min_x && r.min_x <= view->max_x && r.max_y >= view->min_y && r.min_y <= view->max_y);
}

f32 AxisTileEdge(i32 index, f32 size, f32 origin) {
    return static_cast<f32>(index) * size + origin;
}

struct CellRange {
    i32 x0, y0, x1, y1;
};

CellRange TileCellsInView(const TilemapData& map, const ViewRect* view, const Vec3& origin,
                          const PlaneRotation& rotation, f32 tw, f32 th) {
    const CellRange all{0, 0, static_cast<i32>(map.width) - 1, static_cast<i32>(map.height) - 1};
    if (!view || map.width == 0 || map.height == 0) return all;
    const f64 left = static_cast<f64>(view->min_x) - origin.x;
    const f64 right = static_cast<f64>(view->max_x) - origin.x;
    const f64 bottom = static_cast<f64>(view->min_y) - origin.y;
    const f64 top = static_cast<f64>(view->max_y) - origin.y;
    const auto index = [](f64 cell, i32 last) {
        return static_cast<i32>(std::clamp(std::floor(cell), 0.0, static_cast<f64>(last)));
    };
    if (rotation.s == 0.0f && rotation.c == 1.0f) {
        // Reject views wholly outside the map before clamping indices.
        // Strict comparisons preserve the renderer's closed edge tests.
        if (view->max_x < origin.x || view->max_y < origin.y ||
            view->min_x > AxisTileEdge(static_cast<i32>(map.width), tw, origin.x) ||
            view->min_y > AxisTileEdge(static_cast<i32>(map.height), th, origin.y)) return {0, 0, -1, -1};
        return {index(left / tw, all.x1), index(bottom / th, all.y1),
                index(right / tw, all.x1), index(top / th, all.y1)};
    }

    const f64 c = rotation.c, s = rotation.s;
    const f64 ac = std::abs(c), as = std::abs(s), det = c * c + s * s;
    // A tile's world AABB may overlap the view even when its rotated quad does not.
    // First expand the view by a tile's AABB half-extents so all such tile centers
    // are included, then bound the inverse-transformed rectangle in grid space.
    // The padding covers float rounding when corners are built in world space.
    const f64 rounding = 8.0 * (std::numeric_limits<f32>::epsilon() *
        (std::abs(static_cast<f64>(origin.x)) + std::abs(static_cast<f64>(origin.y)) +
         static_cast<f64>(map.width) * tw + static_cast<f64>(map.height) * th) +
        std::numeric_limits<f32>::min());
    const f64 cx = (left + right) * 0.5, cy = (bottom + top) * 0.5;
    const f64 rx = (right - left + ac * tw + as * th) * 0.5 + rounding;
    const f64 ry = (top - bottom + as * tw + ac * th) * 0.5 + rounding;
    const f64 tx = (c * cx + s * cy) / det, ty = (-s * cx + c * cy) / det;
    const f64 hx = (ac * rx + as * ry) / det, hy = (as * rx + ac * ry) / det;
    const f64 x0 = (tx - hx) / tw - 0.5, x1 = (tx + hx) / tw - 0.5;
    const f64 y0 = (ty - hy) / th - 0.5, y1 = (ty + hy) / th - 0.5;
    if (x1 < 0 || y1 < 0 || x0 > all.x1 || y0 > all.y1) return {0, 0, -1, -1};
    return {index(x0, all.x1), index(y0, all.y1), index(x1, all.x1), index(y1, all.y1)};
}

bool FiniteColor(const SpriteColor& color) {
    return std::isfinite(color.r) && std::isfinite(color.g) && std::isfinite(color.b) && std::isfinite(color.a);
}

bool FiniteTransform2D(const Transform& transform) {
    return std::isfinite(transform.position.x) && std::isfinite(transform.position.y) &&
           std::isfinite(transform.position.z) && std::isfinite(transform.rotation.z) &&
           std::isfinite(transform.rotation.w);
}

void PushQuad(std::vector<SpriteVertex>& out, const f32 px[4], const f32 py[4], f32 z, const f32 uv[4], const SpriteColor& c,
              bool flip_x, bool flip_y, const f32* depths = nullptr) {
    f32 u0 = uv[0], v0 = uv[1], u1 = uv[2], v1 = uv[3];
    if (flip_x) std::swap(u0, u1);
    if (flip_y) std::swap(v0, v1);
    // Corners: top-left, top-right, bottom-right, bottom-left.
    const f32 us[4] = {u0, u1, u1, u0};
    const f32 vs[4] = {v0, v0, v1, v1};
    for (int i = 0; i < 4; ++i) out.push_back({px[i], py[i], depths ? depths[i] : z, us[i], vs[i], c.r, c.g, c.b, c.a});
}

} // namespace

static std::vector<SpriteBatch> BuildSpriteBatchesImpl(const World& world, const Resolvers2D& resolvers,
                                                        const GuidIndex* guids, const ViewRect* view) {
    std::vector<Item> items;
    if (view && (!std::isfinite(view->min_x) || !std::isfinite(view->min_y) || !std::isfinite(view->max_x) ||
                 !std::isfinite(view->max_y) || view->min_x > view->max_x || view->min_y > view->max_y)) return {};
    usize sequence = 0;

    std::optional<GuidIndex> parent_guids;
    const auto add_sprite = [&](const Transform* t, const Sprite* sprite, const Parent* parent) {
        ++sequence;
        const SpriteAtlas* atlas = resolvers.atlases ? resolvers.atlases(sprite->atlas.guid) : nullptr;
        const SpriteFrame* frame = atlas ? atlas->FindFrame(sprite->frame) : nullptr;
        f32 uv[4];
        if (!frame || !atlas->Uv(*frame, uv) || !std::isfinite(sprite->pixels_per_unit) ||
            sprite->pixels_per_unit <= 0.0f || !FiniteTransform2D(*t) || !FiniteColor(sprite->color) ||
            !std::isfinite(frame->pivot_x) || !std::isfinite(frame->pivot_y)) return;
        const f32 w = (static_cast<f32>(frame->w) / sprite->pixels_per_unit) * t->scale.x;
        const f32 h = (static_cast<f32>(frame->h) / sprite->pixels_per_unit) * t->scale.y;
        if (!std::isfinite(w) || !std::isfinite(h) || w == 0.0f || h == 0.0f) return;
        // Scale around the frame pivot before rotation. Preserve the quad's
        // winding for a negative scale and mirror its UVs instead.
        f32 left = -frame->pivot_x * w, right = (1.0f - frame->pivot_x) * w;
        f32 top = frame->pivot_y * h, bottom = -(1.0f - frame->pivot_y) * h;
        if (w < 0.0f) std::swap(left, right);
        if (h < 0.0f) std::swap(top, bottom);
        const f32 lx[4] = {left, right, right, left};
        const f32 ly[4] = {top, top, bottom, bottom};
        const PlaneRotation rotation(t->rotation);
        f32 px[4], py[4];
        for (int i = 0; i < 4; ++i) {
            rotation.Apply(lx[i], ly[i], px[i], py[i]);
            px[i] += t->position.x;
            py[i] += t->position.y;
            if (!std::isfinite(px[i]) || !std::isfinite(py[i])) return;
        }
        f32 z = t->position.z;
        f32 depths[4] = {z, z, z, z};
        bool flip_x = sprite->flip_x != (w < 0.0f);
        if (parent && !parent->parent.IsNull()) {
            if (!guids) {
                // Build once only if a drawable sprite actually uses a parent.
                // Keep the lowest entity index if duplicate IDs escaped scene validation.
                parent_guids.emplace();
                world.ForEachArchetype([&](const Archetype& arch) {
                    const ComponentId id = GetComponentId<IdComponent>();
                    if (!arch.Has(id)) return;
                    for (usize chunk = 0; chunk < arch.ChunkCount(); ++chunk) {
                        const auto* ids = static_cast<const IdComponent*>(arch.ComponentArray(chunk, id));
                        const auto* entities = arch.EntityArray(chunk);
                        for (u32 row = 0; row < arch.ChunkEntityCount(chunk); ++row) {
                            const auto guid = ids[row].guid;
                            if (guid.IsNull()) continue;
                            const Entity existing = parent_guids->Find(world, guid);
                            if (existing.IsNull() || existing.index > entities[row].index)
                                parent_guids->Add(guid, entities[row]);
                        }
                    }
                });
                guids = &*parent_guids;
            }
            const Entity resolved = guids->Find(world, parent->parent);
            if (!resolved.IsNull()) {
                const Mat4 inherited = ComputeWorldTransform(world, *guids, resolved);
                z = (inherited * Vec4(t->position.x, t->position.y, t->position.z, 1)).z;
                if (!std::isfinite(z)) return;
                for (int i = 0; i < 4; ++i) {
                    const Vec4 point = inherited * Vec4(px[i], py[i], t->position.z, 1);
                    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) return;
                    px[i] = point.x;
                    py[i] = point.y;
                    depths[i] = point.z;
                }
                const f64 winding = (static_cast<f64>(px[1]) - px[0]) * (static_cast<f64>(py[3]) - py[0]) -
                                    (static_cast<f64>(py[1]) - py[0]) * (static_cast<f64>(px[3]) - px[0]);
                if (winding > 0.0) {
                    for (auto* axis : {px, py, depths}) {
                        std::swap(axis[0], axis[1]);
                        std::swap(axis[2], axis[3]);
                    }
                    flip_x = !flip_x;
                }
            }
        }
        const Rect bounds{std::min({px[0], px[1], px[2], px[3]}), std::min({py[0], py[1], py[2], py[3]}),
                          std::max({px[0], px[1], px[2], px[3]}), std::max({py[0], py[1], py[2], py[3]})};
        if (!Visible(view, bounds)) return;
        Item item{sprite->sorting_layer, sprite->order, z, sequence, atlas->texture, {}};
        PushQuad(item.vertices, px, py, z, uv, sprite->color, flip_x, sprite->flip_y != (h < 0.0f), depths);
        items.push_back(std::move(item));
    };
    world.ForEachArchetype([&](const Archetype& arch) {
        const ComponentId transform = GetComponentId<Transform>(), sprite = GetComponentId<Sprite>();
        const ComponentId parent = GetComponentId<Parent>();
        if (!arch.Has(transform) || !arch.Has(sprite)) return;
        for (usize chunk = 0; chunk < arch.ChunkCount(); ++chunk) {
            const auto* transforms = static_cast<const Transform*>(arch.ComponentArray(chunk, transform));
            const auto* sprites = static_cast<const Sprite*>(arch.ComponentArray(chunk, sprite));
            const auto* parents = arch.Has(parent) ? static_cast<const Parent*>(arch.ComponentArray(chunk, parent)) : nullptr;
            for (u32 row = 0; row < arch.ChunkEntityCount(chunk); ++row)
                add_sprite(&transforms[row], &sprites[row], parents ? &parents[row] : nullptr);
        }
    });

    world.ForEach<Transform, TilemapRenderer>([&](Transform& t_ref, TilemapRenderer& renderer_ref) {
        const Transform* t = &t_ref;
        const TilemapRenderer* renderer = &renderer_ref;
        ++sequence;
        const TilemapData* map = resolvers.tilemaps ? resolvers.tilemaps(renderer->tilemap.guid) : nullptr;
        const Tileset* tileset = map && resolvers.tilesets ? resolvers.tilesets(map->tileset) : nullptr;
        if (!map || !tileset || !std::isfinite(renderer->pixels_per_unit) || renderer->pixels_per_unit <= 0.0f ||
            !FiniteTransform2D(*t) || !FiniteColor(renderer->color) || map->width > static_cast<u32>(std::numeric_limits<i32>::max()) ||
            map->height > static_cast<u32>(std::numeric_limits<i32>::max()) || tileset->tile_width == 0 || tileset->tile_height == 0) return;
        const f32 tw = static_cast<f32>(tileset->tile_width) / renderer->pixels_per_unit;
        const f32 th = static_cast<f32>(tileset->tile_height) / renderer->pixels_per_unit;
        if (!std::isfinite(tw) || !std::isfinite(th) || tw <= 0.0f || th <= 0.0f) return;
        const PlaneRotation rotation(t->rotation);
        const auto cells = TileCellsInView(*map, view, t->position, rotation, tw, th);
        if (cells.x0 > cells.x1 || cells.y0 > cells.y1) return;
        // Each layer is one item, so layers sort with the entity's order and keep their own order.
        for (usize layer = 0; layer < map->layers.size(); ++layer) {
            Item item{renderer->sorting_layer, renderer->order, t->position.z, sequence, tileset->texture, {}};
            const auto& stored = map->layers[layer].cells;
            for (i32 y = cells.y0; y <= cells.y1; ++y) {
                const u64 start = static_cast<u64>(y) * map->width + static_cast<u32>(cells.x0);
                if (start >= stored.size()) break; // short rows act as empty cells
                const usize span = std::min<usize>(static_cast<usize>(cells.x1 - cells.x0) + 1,
                                                   stored.size() - static_cast<usize>(start));
                const i32* row = stored.data() + static_cast<usize>(start);
                for (usize i = 0; i < span; ++i) {
                    const i32 x = cells.x0 + static_cast<i32>(i);
                    const i32 raw = row[i];
                    const i32 tile = IsAutotileCell(raw) ? ResolveTile(*map, *tileset, layer, x, y) : raw;
                    f32 uv[4];
                    if (tile < 0 || !tileset->Uv(tile, uv)) continue;
                    const f32 lx[4] = {static_cast<f32>(x) * tw, static_cast<f32>(x + 1) * tw, static_cast<f32>(x + 1) * tw,
                                       static_cast<f32>(x) * tw};
                    const f32 ly[4] = {static_cast<f32>(y + 1) * th, static_cast<f32>(y + 1) * th, static_cast<f32>(y) * th,
                                       static_cast<f32>(y) * th};
                    f32 px[4], py[4];
                    bool finite = true;
                    for (int i = 0; i < 4; ++i) {
                        rotation.Apply(lx[i], ly[i], px[i], py[i]);
                        px[i] += t->position.x;
                        py[i] += t->position.y;
                        finite = finite && std::isfinite(px[i]) && std::isfinite(py[i]);
                    }
                    if (!finite) continue;
                    if (view) {
                        const Rect bounds{std::min({px[0], px[1], px[2], px[3]}), std::min({py[0], py[1], py[2], py[3]}),
                                          std::max({px[0], px[1], px[2], px[3]}), std::max({py[0], py[1], py[2], py[3]})};
                        if (!Visible(view, bounds)) continue;
                    }
                    PushQuad(item.vertices, px, py, t->position.z, uv, renderer->color, false, false);
                }
            }
            if (!item.vertices.empty()) items.push_back(std::move(item));
        }
    });

    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.layer != b.layer) return a.layer < b.layer;
        if (a.order != b.order) return a.order < b.order;
        if (a.z != b.z) return a.z < b.z;
        return a.sequence < b.sequence;
    });

    std::vector<SpriteBatch> batches;
    for (auto it = items.begin(); it != items.end();) {
        auto run_end = it + 1;
        usize vertex_count = it->vertices.size();
        while (run_end != items.end() && run_end->texture == it->texture) {
            vertex_count += run_end->vertices.size();
            ++run_end;
        }
        auto& batch = batches.emplace_back();
        batch.texture = it->texture;
        // Keep the first item's buffer when it already fits, but avoid
        // repeated growth and copying when several layers share this texture.
        batch.vertices = std::move(it->vertices);
        batch.vertices.reserve(vertex_count);
        batch.indices.reserve(vertex_count / 4 * 6);
        const auto append_indices = [&](u32 base, usize quads) {
            for (u32 q = 0; q < quads; ++q) {
                const u32 v = base + q * 4;
                for (u32 i : {0u, 1u, 2u, 0u, 2u, 3u}) batch.indices.push_back(v + i);
            }
        };
        append_indices(0, batch.vertices.size() / 4);
        for (++it; it != run_end; ++it) {
            const u32 base = static_cast<u32>(batch.vertices.size());
            batch.vertices.insert(batch.vertices.end(), it->vertices.begin(), it->vertices.end());
            append_indices(base, it->vertices.size() / 4);
        }
    }
    return batches;
}

std::vector<SpriteBatch> BuildSpriteBatches(const World& world, const Resolvers2D& resolvers, const ViewRect* view) {
    return BuildSpriteBatchesImpl(world, resolvers, nullptr, view);
}

std::vector<SpriteBatch> BuildSpriteBatches(const World& world, const Resolvers2D& resolvers,
                                           const GuidIndex& guids, const ViewRect* view) {
    return BuildSpriteBatchesImpl(world, resolvers, &guids, view);
}

} // namespace aether::sprite2d
