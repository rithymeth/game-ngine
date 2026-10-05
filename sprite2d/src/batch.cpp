#include "aether/sprite2d/batch.h"

#include <algorithm>
#include <cmath>

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

void PushQuad(std::vector<SpriteVertex>& out, const f32 px[4], const f32 py[4], f32 z, const f32 uv[4], const SpriteColor& c,
              bool flip_x, bool flip_y) {
    f32 u0 = uv[0], v0 = uv[1], u1 = uv[2], v1 = uv[3];
    if (flip_x) std::swap(u0, u1);
    if (flip_y) std::swap(v0, v1);
    // Corners: top-left, top-right, bottom-right, bottom-left.
    const f32 us[4] = {u0, u1, u1, u0};
    const f32 vs[4] = {v0, v0, v1, v1};
    for (int i = 0; i < 4; ++i) out.push_back({px[i], py[i], z, us[i], vs[i], c.r, c.g, c.b, c.a});
}

} // namespace

std::vector<SpriteBatch> BuildSpriteBatches(const World& world, const Resolvers2D& resolvers, const ViewRect* view) {
    std::vector<Item> items;
    usize sequence = 0;

    world.ForEach<Transform, Sprite>([&](Transform& t_ref, Sprite& sprite_ref) {
        const Transform* t = &t_ref;
        const Sprite* sprite = &sprite_ref;
        ++sequence;
        const SpriteAtlas* atlas = resolvers.atlases ? resolvers.atlases(sprite->atlas.guid) : nullptr;
        const SpriteFrame* frame = atlas ? atlas->FindFrame(sprite->frame) : nullptr;
        f32 uv[4];
        if (!frame || !atlas->Uv(*frame, uv) || sprite->pixels_per_unit <= 0.0f) return;
        const f32 w = static_cast<f32>(frame->w) / sprite->pixels_per_unit;
        const f32 h = static_cast<f32>(frame->h) / sprite->pixels_per_unit;
        // Local corners from the pivot (fractions from the frame's top left).
        const f32 left = -frame->pivot_x * w, right = (1.0f - frame->pivot_x) * w;
        const f32 top = frame->pivot_y * h, bottom = -(1.0f - frame->pivot_y) * h;
        const f32 lx[4] = {left, right, right, left};
        const f32 ly[4] = {top, top, bottom, bottom};
        const PlaneRotation rotation(t->rotation);
        f32 px[4], py[4];
        for (int i = 0; i < 4; ++i) {
            rotation.Apply(lx[i], ly[i], px[i], py[i]);
            px[i] += t->position.x;
            py[i] += t->position.y;
        }
        const Rect bounds{std::min({px[0], px[1], px[2], px[3]}), std::min({py[0], py[1], py[2], py[3]}),
                          std::max({px[0], px[1], px[2], px[3]}), std::max({py[0], py[1], py[2], py[3]})};
        if (!Visible(view, bounds)) return;
        Item item{sprite->sorting_layer, sprite->order, t->position.z, sequence, atlas->texture, {}};
        PushQuad(item.vertices, px, py, t->position.z, uv, sprite->color, sprite->flip_x, sprite->flip_y);
        items.push_back(std::move(item));
    });

    world.ForEach<Transform, TilemapRenderer>([&](Transform& t_ref, TilemapRenderer& renderer_ref) {
        const Transform* t = &t_ref;
        const TilemapRenderer* renderer = &renderer_ref;
        ++sequence;
        const TilemapData* map = resolvers.tilemaps ? resolvers.tilemaps(renderer->tilemap.guid) : nullptr;
        const Tileset* tileset = map && resolvers.tilesets ? resolvers.tilesets(map->tileset) : nullptr;
        if (!map || !tileset || renderer->pixels_per_unit <= 0.0f) return;
        const f32 tw = static_cast<f32>(tileset->tile_width) / renderer->pixels_per_unit;
        const f32 th = static_cast<f32>(tileset->tile_height) / renderer->pixels_per_unit;
        const PlaneRotation rotation(t->rotation);
        // Each layer is one item, so layers sort with the entity's order and keep their own order.
        for (usize layer = 0; layer < map->layers.size(); ++layer) {
            Item item{renderer->sorting_layer, renderer->order, t->position.z, sequence, tileset->texture, {}};
            // Cells in the view only (the rotation is rarely used on maps; the box handles it).
            i32 x0 = 0, y0 = 0, x1 = static_cast<i32>(map->width) - 1, y1 = static_cast<i32>(map->height) - 1;
            if (view && std::abs(rotation.s) < 1e-6f && rotation.c > 0.0f) {
                x0 = std::max(x0, static_cast<i32>(std::floor((view->min_x - t->position.x) / tw)));
                y0 = std::max(y0, static_cast<i32>(std::floor((view->min_y - t->position.y) / th)));
                x1 = std::min(x1, static_cast<i32>(std::floor((view->max_x - t->position.x) / tw)));
                y1 = std::min(y1, static_cast<i32>(std::floor((view->max_y - t->position.y) / th)));
            }
            for (i32 y = y0; y <= y1; ++y) {
                for (i32 x = x0; x <= x1; ++x) {
                    const i32 tile = ResolveTile(*map, *tileset, layer, x, y);
                    f32 uv[4];
                    if (tile < 0 || !tileset->Uv(tile, uv)) continue;
                    const f32 lx[4] = {static_cast<f32>(x) * tw, static_cast<f32>(x + 1) * tw, static_cast<f32>(x + 1) * tw,
                                       static_cast<f32>(x) * tw};
                    const f32 ly[4] = {static_cast<f32>(y + 1) * th, static_cast<f32>(y + 1) * th, static_cast<f32>(y) * th,
                                       static_cast<f32>(y) * th};
                    f32 px[4], py[4];
                    for (int i = 0; i < 4; ++i) {
                        rotation.Apply(lx[i], ly[i], px[i], py[i]);
                        px[i] += t->position.x;
                        py[i] += t->position.y;
                    }
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
    for (Item& item : items) {
        if (batches.empty() || batches.back().texture != item.texture) {
            batches.emplace_back();
            batches.back().texture = item.texture;
        }
        SpriteBatch& batch = batches.back();
        const u32 base = static_cast<u32>(batch.vertices.size());
        batch.vertices.insert(batch.vertices.end(), item.vertices.begin(), item.vertices.end());
        for (u32 q = 0; q < item.vertices.size() / 4; ++q) {
            const u32 v = base + q * 4;
            for (u32 i : {0u, 1u, 2u, 0u, 2u, 3u}) batch.indices.push_back(v + i);
        }
    }
    return batches;
}

} // namespace aether::sprite2d
