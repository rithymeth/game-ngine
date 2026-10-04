#include "test_framework.h"

#include "aether/ecs/world.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/serialization.h"
#include "aether/sprite2d/atlas.h"
#include "aether/sprite2d/batch.h"
#include "aether/sprite2d/pixel_camera.h"
#include "aether/sprite2d/tilemap.h"

#include <cmath>
#include <filesystem>
#include <map>
#include <set>

// The 2D toolkit's data and batching (Phase 26 step 5, §26.6): sprite atlases
// and clips, packing, tilesets with autotiles, tilemaps, sorted batches and
// the pixel-perfect camera.

using namespace aether;
using namespace aether::sprite2d;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

bool Near(f32 a, f32 b, f32 eps = 1e-5f) { return std::fabs(a - b) <= eps; }

assets::AssetGuid Guid() { return assets::NewAssetGuid(); }

SpriteAtlas HeroAtlas(const assets::AssetGuid& texture) {
    SpriteAtlas a = AtlasFromGrid(texture, 64, 32, 16, 16, "hero"); // 4 x 2 cells
    SpriteClip run;
    run.name = "run";
    run.fps = 10.0f;
    run.frames = {"hero_0", "hero_1", "hero_2", "hero_3"};
    SpriteClip die;
    die.name = "die";
    die.fps = 10.0f;
    die.loop = false;
    die.frames = {"hero_4", "hero_5"};
    a.clips = {run, die};
    return a;
}

} // namespace

AETHER_TEST(Sprite2D_AtlasFramesUvAndJson) {
    const assets::AssetGuid tex = Guid();
    SpriteAtlas atlas = HeroAtlas(tex);
    CHECK(atlas.frames.size() == 8);
    const SpriteFrame* f = atlas.FindFrame("hero_5"); // second row, second cell
    CHECK(f && f->x == 16 && f->y == 16);
    f32 uv[4];
    CHECK(atlas.Uv(*f, uv));
    CHECK(Near(uv[0], 0.25f) && Near(uv[1], 0.5f) && Near(uv[2], 0.5f) && Near(uv[3], 1.0f));
    CHECK(atlas.FindFrame("nope") == nullptr && atlas.FindClip("run") != nullptr);
    atlas.frames[0].pivot_x = 0.0f;
    atlas.frames[0].pivot_y = 1.0f;

    SpriteAtlas loaded;
    std::string error;
    CHECK(AtlasFromJson(AtlasToJson(atlas), loaded, &error));
    CHECK(loaded.texture == tex && loaded.frames.size() == 8 && loaded.clips.size() == 2);
    CHECK(Near(loaded.frames[0].pivot_x, 0.0f) && Near(loaded.frames[0].pivot_y, 1.0f));
    CHECK(loaded.FindClip("die")->loop == false && loaded.FindClip("run")->fps == 10.0f);

    const std::filesystem::path file = std::filesystem::temp_directory_path() / "aether_sprite2d" / "hero.aatlas";
    CHECK(SaveAtlas(file, atlas, &error));
    SpriteAtlas again;
    CHECK(LoadAtlas(file, again, &error) && again.frames.size() == 8);
}

AETHER_TEST(Sprite2D_AtlasRefusesBadData) {
    SpriteAtlas out;
    std::string error;
    CHECK(!AtlasFromJson(nlohmann::json::parse(R"({"$type":"Other"})"), out, &error));
    CHECK(!AtlasFromJson(nlohmann::json::parse(R"({"$type":"SpriteAtlas","frames":[{"name":"a","w":0,"h":4}]})"), out, &error));
    CHECK(error.find("no size") != std::string::npos);
    CHECK(!AtlasFromJson(nlohmann::json::parse(
                             R"({"$type":"SpriteAtlas","frames":[{"name":"a","w":4,"h":4}],"clips":[{"name":"c","frames":["zzz"]}]})"),
                         out, &error));
    CHECK(error.find("zzz") != std::string::npos);
    CHECK(!AtlasFromJson(nlohmann::json::parse(
                             R"({"$type":"SpriteAtlas","frames":[{"name":"a","w":4,"h":4},{"name":"a","w":4,"h":4}]})"),
                         out, &error));
}

AETHER_TEST(Sprite2D_ClipFramesLoopAndHold) {
    const SpriteAtlas atlas = HeroAtlas(Guid());
    const SpriteClip& run = *atlas.FindClip("run");
    const SpriteClip& die = *atlas.FindClip("die");
    bool finished = false;
    CHECK(ClipFrameAt(run, 0.0f) == 0 && ClipFrameAt(run, 0.15f) == 1 && ClipFrameAt(run, 0.35f) == 3);
    CHECK(ClipFrameAt(run, 0.45f, &finished) == 0 && !finished); // wrapped
    CHECK(ClipFrameAt(die, 0.05f, &finished) == 0 && !finished);
    CHECK(ClipFrameAt(die, 5.0f, &finished) == 1 && finished); // held on the last frame
    CHECK(ClipFrameAt(SpriteClip{}, 1.0f) == 0);
}

AETHER_TEST(Sprite2D_PackRectsAndComposeImage) {
    std::vector<PackInput> in = {{"a", 10, 20}, {"b", 30, 10}, {"c", 16, 16}, {"d", 8, 8}, {"e", 30, 30}};
    PackResult packed;
    std::string error;
    CHECK(PackRects(in, 2, 256, packed, &error));
    CHECK(packed.rects.size() == 5 && packed.rects[0].name == "a"); // in the input order
    CHECK((packed.width & (packed.width - 1)) == 0 && (packed.height & (packed.height - 1)) == 0);
    // No overlaps, all inside, padding kept.
    for (usize i = 0; i < packed.rects.size(); ++i) {
        const PackedRect& a = packed.rects[i];
        CHECK(a.x >= 0 && a.y >= 0 && a.x + a.w <= packed.width && a.y + a.h <= packed.height);
        for (usize j = i + 1; j < packed.rects.size(); ++j) {
            const PackedRect& b = packed.rects[j];
            const bool apart = a.x + static_cast<i32>(a.w) + 2 <= b.x || b.x + static_cast<i32>(b.w) + 2 <= a.x ||
                               a.y + static_cast<i32>(a.h) + 2 <= b.y || b.y + static_cast<i32>(b.h) + 2 <= a.y;
            CHECK(apart);
        }
    }
    CHECK(!PackRects({{"big", 300, 10}}, 0, 256, packed, &error));
    CHECK(!PackRects({{"zero", 0, 10}}, 0, 256, packed, &error));

    // Compose: each image's pixels land at its rectangle.
    std::vector<assets::ImageData> images;
    for (usize i = 0; i < in.size(); ++i) {
        assets::ImageData img;
        img.width = in[i].w;
        img.height = in[i].h;
        img.pixels.assign(static_cast<usize>(img.width) * img.height * 4, static_cast<u8>(10 + i));
        images.push_back(std::move(img));
    }
    assets::ImageData atlas_image;
    CHECK(ComposeAtlasImage(images, packed, atlas_image, &error));
    CHECK(atlas_image.width == packed.width && atlas_image.pixels.size() == static_cast<usize>(packed.width) * packed.height * 4);
    for (usize i = 0; i < packed.rects.size(); ++i) {
        const PackedRect& r = packed.rects[i];
        CHECK(atlas_image.pixels[(static_cast<usize>(r.y) * packed.width + r.x) * 4] == 10 + i);
    }
    CHECK(atlas_image.pixels[3] == 0 || atlas_image.pixels[0] != 0); // the padding or a sprite; never garbage
    const SpriteAtlas a = AtlasFromPack(Guid(), packed);
    CHECK(a.frames.size() == 5 && a.FindFrame("e")->w == 30);
    images.pop_back();
    CHECK(!ComposeAtlasImage(images, packed, atlas_image, &error));
}

AETHER_TEST(Sprite2D_TilesetGeometryAndJson) {
    Tileset t;
    t.texture = Guid();
    t.texture_width = 100;
    t.texture_height = 50;
    t.tile_width = t.tile_height = 16;
    t.margin = 2;
    t.spacing = 2;
    CHECK(t.Columns() == 5 && t.Rows() == 2 && t.TileCount() == 10); // (100-4+2)/18, (50-4+2)/18
    f32 uv[4];
    CHECK(t.Uv(6, uv)); // column 1, row 1
    CHECK(Near(uv[0], 20.0f / 100.0f) && Near(uv[1], 20.0f / 50.0f) && Near(uv[2], 36.0f / 100.0f) && Near(uv[3], 36.0f / 50.0f));
    CHECK(!t.Uv(10, uv) && !t.Uv(-1, uv));
    t.SetSolid(3, true);
    CHECK(t.IsSolid(3) && !t.IsSolid(2) && !t.IsSolid(99) && !t.IsSolid(-1));
    std::array<i32, 16> tiles;
    tiles.fill(0);
    tiles[15] = 5;
    CHECK(t.AddAutotile("grass", tiles) == 0);

    Tileset back;
    std::string error;
    CHECK(TilesetFromJson(TilesetToJson(t), back, &error));
    CHECK(back.IsSolid(3) && back.autotiles.size() == 1 && back.autotiles[0].tiles[15] == 5 && back.margin == 2);
    CHECK(back.texture == t.texture && back.Columns() == 5);
    CHECK(!TilesetFromJson(nlohmann::json::parse(R"({"$type":"Tileset","tile_size":[0,0]})"), back, &error));
    CHECK(!TilesetFromJson(nlohmann::json::parse(R"({"$type":"Tileset","autotiles":[{"name":"x","tiles":[1]}]})"), back, &error));
}

AETHER_TEST(Sprite2D_TilemapEditingAndAutotiles) {
    TilemapData map;
    map.tileset = Guid();
    map.Resize(5, 4);
    CHECK(map.AddLayer("ground") == 0);
    CHECK(map.Get(0, 2, 2) == kEmpty && map.Get(0, -1, 0) == kEmpty && map.Get(7, 0, 0) == kEmpty);
    CHECK(map.Set(0, 2, 2, 7) && map.Get(0, 2, 2) == 7);
    CHECK(!map.Set(0, 5, 0, 1) && !map.Set(3, 0, 0, 1));
    CHECK(map.Fill(0, -3, 0, 1, 1, 4) == 4); // clamped to the map: x 0..1, y 0..1
    map.Resize(3, 3);                         // keeps what still fits
    CHECK(map.Get(0, 1, 1) == 4 && map.Get(0, 2, 2) == 7 && map.layers[0].cells.size() == 9);
    map.Resize(6, 6);
    CHECK(map.Get(0, 2, 2) == 7 && map.Get(0, 5, 5) == kEmpty);

    // A 3x3 autotile block: each cell's tile follows its same-neighbours.
    Tileset ts;
    ts.texture_width = ts.texture_height = 64;
    ts.tile_width = ts.tile_height = 16;
    std::array<i32, 16> tiles;
    for (int m = 0; m < 16; ++m) tiles[m] = 100 + m;
    const usize grass = ts.AddAutotile("grass", tiles);
    TilemapData m2;
    m2.Resize(5, 5);
    m2.AddLayer("L");
    m2.Fill(0, 1, 1, 3, 3, AutotileCell(grass));
    CHECK(IsAutotileCell(m2.Get(0, 1, 1)) && AutotileIndex(m2.Get(0, 1, 1)) == grass);
    CHECK(NeighbourMask(m2, 0, 2, 2) == 15);                       // the middle
    CHECK(NeighbourMask(m2, 0, 1, 1) == (kNorth | kEast));         // bottom-left corner
    CHECK(NeighbourMask(m2, 0, 3, 3) == (kSouth | kWest));         // top-right corner
    CHECK(NeighbourMask(m2, 0, 2, 1) == (kNorth | kEast | kWest)); // bottom edge
    CHECK(NeighbourMask(m2, 0, 0, 0) == 0);                        // empty
    CHECK(ResolveTile(m2, ts, 0, 2, 2) == 115 && ResolveTile(m2, ts, 0, 1, 1) == 103 && ResolveTile(m2, ts, 0, 0, 0) == -1);
    m2.Set(0, 2, 2, kEmpty); // a hole: its neighbours open toward it
    CHECK(ResolveTile(m2, ts, 0, 2, 1) == 100 + (kEast | kWest));
    m2.Set(0, 0, 0, 5); // a plain tile resolves to itself
    CHECK(ResolveTile(m2, ts, 0, 0, 0) == 5);
    m2.Set(0, 4, 4, AutotileCell(9)); // an autotile the set doesn't have
    CHECK(ResolveTile(m2, ts, 0, 4, 4) == kEmpty);
}

AETHER_TEST(Sprite2D_TilemapSolidityAndCells) {
    Tileset ts;
    ts.texture_width = ts.texture_height = 32;
    ts.tile_width = ts.tile_height = 16;
    ts.SetSolid(1, true);
    TilemapData m;
    m.Resize(4, 4);
    m.AddLayer("walls");
    const usize deco = m.AddLayer("deco");
    m.layers[deco].collides = false;
    m.Set(0, 1, 1, 1);
    m.Set(deco, 2, 2, 1);
    CHECK(IsSolidAt(m, ts, 1, 1) && !IsSolidAt(m, ts, 2, 2) && !IsSolidAt(m, ts, 0, 0) && !IsSolidAt(m, ts, -4, 9));
    i32 cx = 0, cy = 0;
    LocalToCell(1.6f, 0.9f, 0.5f, cx, cy);
    CHECK(cx == 3 && cy == 1);
    LocalToCell(-0.1f, -0.6f, 0.5f, cx, cy);
    CHECK(cx == -1 && cy == -2);
    f32 lx = 0, ly = 0;
    CellToLocal(3, 1, 0.5f, lx, ly);
    CHECK(Near(lx, 1.5f) && Near(ly, 0.5f));
}

AETHER_TEST(Sprite2D_TilemapJsonRoundTripAndErrors) {
    TilemapData m;
    m.tileset = Guid();
    m.Resize(3, 2);
    m.AddLayer("a");
    m.Set(0, 2, 1, 9);
    m.Set(0, 0, 0, AutotileCell(0));
    m.layers[0].collides = false;
    TilemapData back;
    std::string error;
    CHECK(TilemapFromJson(TilemapToJson(m), back, &error));
    CHECK(back.width == 3 && back.height == 2 && back.tileset == m.tileset && back.layers[0].collides == false);
    CHECK(back.Get(0, 2, 1) == 9 && back.Get(0, 0, 0) == AutotileCell(0));
    CHECK(!TilemapFromJson(nlohmann::json::parse(R"({"$type":"Tilemap","size":[2,2],"layers":[{"name":"x","cells":[1]}]})"), back, &error));
    CHECK(error.find("cells") != std::string::npos);
    CHECK(!TilemapFromJson(nlohmann::json::parse(R"({"$type":"Tilemap","size":[100000,100000]})"), back, &error));
    const auto file = std::filesystem::temp_directory_path() / "aether_sprite2d" / "level.atilemap";
    CHECK(SaveTilemap(file, m, &error));
    TilemapData again;
    CHECK(LoadTilemap(file, again, &error) && again.Get(0, 2, 1) == 9);
    CHECK(!LoadTilemap(file.parent_path() / "missing.atilemap", again, &error));
}

AETHER_TEST(Sprite2D_BatchesAreSortedAndMerged) {
    RegisterSprite2DComponents();
    World world;
    const assets::AssetGuid tex_a = Guid(), tex_b = Guid();
    const assets::AssetGuid atlas_a = Guid(), atlas_b = Guid();
    std::map<assets::AssetGuid, SpriteAtlas> atlases;
    atlases[atlas_a] = HeroAtlas(tex_a);
    atlases[atlas_b] = HeroAtlas(tex_b);
    Resolvers2D resolvers;
    resolvers.atlases = [&](const assets::AssetGuid& g) -> const SpriteAtlas* {
        auto it = atlases.find(g);
        return it == atlases.end() ? nullptr : &it->second;
    };
    const auto add = [&](const assets::AssetGuid& atlas, const char* frame, Vec3 at, i32 layer, i32 order) {
        const Entity e = world.CreateEntity();
        world.AddComponent<Transform>(e, Transform{at, Quaternion::Identity()});
        Sprite s;
        s.atlas.guid = atlas;
        SetSpriteFrame(s, frame);
        s.sorting_layer = layer;
        s.order = order;
        world.AddComponent<Sprite>(e, s);
        return e;
    };
    add(atlas_a, "hero_0", Vec3(0, 0, 0), 1, 0); // 3rd
    add(atlas_b, "hero_0", Vec3(1, 0, 0), 0, 5); // 2nd (layer 0, order 5)
    add(atlas_a, "hero_1", Vec3(2, 0, 0), 0, -1); // 1st
    add(atlas_a, "hero_2", Vec3(3, 0, 0), 1, 0);  // 4th: after the 3rd, same texture as it
    add(atlas_a, "missing", Vec3(4, 0, 0), 0, 0); // no such frame: skipped
    const std::vector<SpriteBatch> batches = BuildSpriteBatches(world, resolvers);
    // Order: A (x=2), B (x=1), then A, A merged (x=0, x=3).
    CHECK(batches.size() == 3);
    CHECK(batches[0].texture == tex_a && batches[0].QuadCount() == 1 && Near(batches[0].vertices[0].x, 2.0f - 0.5f));
    CHECK(batches[1].texture == tex_b && batches[1].QuadCount() == 1);
    CHECK(batches[2].texture == tex_a && batches[2].QuadCount() == 2);
    CHECK(batches[2].vertices.size() == 8 && batches[2].indices.size() == 12);
    CHECK(batches[2].indices[0] == 0 && batches[2].indices[5] == 3 && batches[2].indices[6] == 4);
}

AETHER_TEST(Sprite2D_QuadGeometryPivotFlipAndRotation) {
    World world;
    const assets::AssetGuid atlas_guid = Guid();
    SpriteAtlas atlas = AtlasFromGrid(Guid(), 32, 32, 16, 16, "t");
    atlas.frames[0].pivot_x = 0.0f; // the frame's top-left is the origin
    atlas.frames[0].pivot_y = 0.0f;
    Resolvers2D resolvers;
    resolvers.atlases = [&](const assets::AssetGuid&) { return &atlas; };
    const Entity e = world.CreateEntity();
    world.AddComponent<Transform>(e, Transform{Vec3(10, 20, 3), Quaternion::Identity()});
    Sprite s;
    s.atlas.guid = atlas_guid;
    SetSpriteFrame(s, "t_0");
    s.pixels_per_unit = 8.0f; // 16 px -> 2 units
    s.color = {0.5f, 0.25f, 1.0f, 0.5f};
    world.AddComponent<Sprite>(e, s);

    auto quad = [&] { return BuildSpriteBatches(world, resolvers).at(0).vertices; };
    std::vector<SpriteVertex> v = quad();
    // Pivot at the top left: the quad spans x 10..12 and y 18..20.
    CHECK(Near(v[0].x, 10) && Near(v[0].y, 20) && Near(v[2].x, 12) && Near(v[2].y, 18) && Near(v[0].z, 3));
    CHECK(Near(v[0].u, 0) && Near(v[0].v, 0) && Near(v[2].u, 0.5f) && Near(v[2].v, 0.5f));
    CHECK(Near(v[1].r, 0.5f) && Near(v[1].g, 0.25f) && Near(v[1].a, 0.5f));

    world.GetComponent<Sprite>(e)->flip_x = true;
    v = quad();
    CHECK(Near(v[0].u, 0.5f) && Near(v[1].u, 0.0f)); // u swapped, positions unchanged
    CHECK(Near(v[0].x, 10));
    world.GetComponent<Sprite>(e)->flip_x = false;

    // A quarter turn about z: the +x edge points up.
    const f32 half = std::sqrt(0.5f);
    world.GetComponent<Transform>(e)->rotation = Quaternion(0, 0, half, half);
    v = quad();
    CHECK(Near(v[1].x, 10, 1e-4f) && Near(v[1].y, 22, 1e-4f)); // the top-right corner (2, 0) -> (0, 2)
}

AETHER_TEST(Sprite2D_ViewCullingAndTilemapBatches) {
    RegisterSprite2DComponents();
    World world;
    Tileset ts;
    ts.texture = Guid();
    ts.texture_width = ts.texture_height = 32;
    ts.tile_width = ts.tile_height = 16;
    TilemapData map;
    map.tileset = Guid();
    map.Resize(10, 10);
    map.AddLayer("a");
    map.AddLayer("b");
    map.Fill(0, 0, 0, 9, 9, 0);
    map.Set(1, 5, 5, 3);
    const assets::AssetGuid map_guid = Guid();
    Resolvers2D resolvers;
    resolvers.tilemaps = [&](const assets::AssetGuid&) { return &map; };
    resolvers.tilesets = [&](const assets::AssetGuid&) { return &ts; };
    const Entity e = world.CreateEntity();
    world.AddComponent<Transform>(e, Transform{Vec3(0, 0, 0), Quaternion::Identity()});
    TilemapRenderer r;
    r.tilemap.guid = map_guid;
    r.pixels_per_unit = 16.0f; // one unit per tile
    world.AddComponent<TilemapRenderer>(e, r);

    std::vector<SpriteBatch> all = BuildSpriteBatches(world, resolvers);
    CHECK(all.size() == 1 && all[0].QuadCount() == 101); // 100 ground + 1 on layer b, merged (same texture)
    const ViewRect view{2.0f, 2.0f, 4.5f, 3.5f}; // tiles x 2..4, y 2..3
    std::vector<SpriteBatch> some = BuildSpriteBatches(world, resolvers, &view);
    CHECK(some.size() == 1 && some[0].QuadCount() == 6);
    const ViewRect away{100, 100, 110, 110};
    CHECK(BuildSpriteBatches(world, resolvers, &away).empty());
    // Culling cuts the quads the view can't see, not the visible ones: compare to the full set.
    usize inside = 0;
    for (usize q = 0; q < all[0].QuadCount(); ++q) {
        const SpriteVertex& a = all[0].vertices[q * 4];
        const SpriteVertex& c = all[0].vertices[q * 4 + 2];
        if (c.x > view.min_x && a.x <= view.max_x && a.y > view.min_y && c.y <= view.max_y) ++inside; // overlapping, not just touching
    }
    CHECK(inside == 6);
}

AETHER_TEST(Sprite2D_AnimatorPlaysClipsOnTheSprite) {
    RegisterSprite2DComponents();
    World world;
    const assets::AssetGuid atlas_guid = Guid();
    const SpriteAtlas atlas = HeroAtlas(Guid());
    const AtlasResolver resolver = [&](const assets::AssetGuid& g) { return g == atlas_guid ? &atlas : nullptr; };
    const Entity e = world.CreateEntity();
    Sprite s;
    s.atlas.guid = atlas_guid;
    world.AddComponent<Sprite>(e, s);
    SpriteAnimator a;
    PlayClip(a, "run");
    world.AddComponent<SpriteAnimator>(e, a);

    UpdateSpriteAnimations(world, resolver, 0.05f);
    CHECK(std::string(world.GetComponent<Sprite>(e)->frame) == "hero_0");
    UpdateSpriteAnimations(world, resolver, 0.1f); // t = 0.15
    CHECK(std::string(world.GetComponent<Sprite>(e)->frame) == "hero_1");
    UpdateSpriteAnimations(world, resolver, 0.3f); // t = 0.45: wrapped
    CHECK(std::string(world.GetComponent<Sprite>(e)->frame) == "hero_0");

    PlayClip(*world.GetComponent<SpriteAnimator>(e), "die");
    UpdateSpriteAnimations(world, resolver, 0.15f);
    CHECK(std::string(world.GetComponent<Sprite>(e)->frame) == "hero_5" || std::string(world.GetComponent<Sprite>(e)->frame) == "hero_4");
    UpdateSpriteAnimations(world, resolver, 5.0f);
    SpriteAnimator* anim = world.GetComponent<SpriteAnimator>(e);
    CHECK(std::string(world.GetComponent<Sprite>(e)->frame) == "hero_5" && anim->finished && !anim->playing);
    const f32 t = anim->time;
    UpdateSpriteAnimations(world, resolver, 1.0f); // stopped: nothing moves
    CHECK(anim->time == t);

    PlayClip(*anim, "no_such_clip");
    UpdateSpriteAnimations(world, resolver, 1.0f); // an unknown clip leaves the frame alone
    CHECK(std::string(world.GetComponent<Sprite>(e)->frame) == "hero_5");
    anim->speed = 0.0f;
    PlayClip(*anim, "run", false);
    CHECK(anim->playing);
}

AETHER_TEST(Sprite2D_ComponentsSaveAndLoadWithScenes) {
    RegisterSprite2DComponents();
    World world;
    const Entity e = world.CreateEntity();
    world.AddComponent<Transform>(e, Transform{Vec3(1, 2, 3), Quaternion::Identity()});
    Sprite s;
    s.atlas.guid = Guid();
    SetSpriteFrame(s, "hero_3");
    s.flip_x = true;
    s.order = 7;
    s.color = {1, 0.5f, 0.25f, 1};
    world.AddComponent<Sprite>(e, s);
    SpriteAnimator a;
    PlayClip(a, "run");
    world.AddComponent<SpriteAnimator>(e, a);

    const nlohmann::json sprite_json = reflect::ToJson(s);
    CHECK(sprite_json["frame"] == "hero_3" && sprite_json["flip_x"] == true && sprite_json["order"] == 7);
    Sprite back;
    CHECK(reflect::FromJson(back, sprite_json));
    CHECK(std::string(back.frame) == "hero_3" && back.flip_x && back.atlas == s.atlas && Near(back.color.g, 0.5f));
}

AETHER_TEST(Sprite2D_PixelPerfectCamera) {
    PixelPerfectCamera cam;
    cam.reference_width = 320;
    cam.reference_height = 180;
    cam.pixels_per_unit = 16.0f;
    PixelViewport v = ComputePixelViewport(cam, 1920, 1080); // exactly 6x
    CHECK(v.scale == 6 && v.width == 1920 && v.height == 1080 && v.x == 0 && v.y == 0);
    CHECK(Near(v.ortho_width, 20.0f) && Near(v.ortho_height, 11.25f));
    v = ComputePixelViewport(cam, 1366, 768); // min(4, 4) = 4: bars around the 1280x720 picture
    CHECK(v.scale == 4 && v.width == 1280 && v.height == 720 && v.x == 43 && v.y == 24);
    v = ComputePixelViewport(cam, 2560, 1080); // limited by the height: 6x, bars at the sides
    CHECK(v.scale == 6 && v.width == 1920 && v.x == 320);
    v = ComputePixelViewport(cam, 200, 100); // smaller than the reference: 1x, never 0
    CHECK(v.scale == 1);
    cam.fill_window = true;
    v = ComputePixelViewport(cam, 1366, 768);
    CHECK(v.scale == 4 && v.width == 1366 && v.height == 768 && v.x == 0);
    CHECK(Near(v.ortho_width, 1366.0f / 4.0f / 16.0f));

    const Vec3 p = SnapToPixelGrid(Vec3(1.03f, -2.49f, 5.5f), 16.0f);
    CHECK(Near(p.x, 1.0f) && Near(p.y, -2.5f) && Near(p.z, 5.5f));
    CHECK(Near(SnapToPixelGrid(Vec3(0.1f, 0.2f, 0), 0.0f).x, 0.1f)); // no grid: unchanged
}

// --- 2D physics ---------------------------------------------------------------

#include "aether/sprite2d/physics2d.h"

namespace {

Entity Box(World& w, Vec3 at, Vec2 half, BodyType2D type, bool with_body = true) {
    const Entity e = w.CreateEntity();
    w.AddComponent<Transform>(e, Transform{at, Quaternion::Identity()});
    Collider2D c;
    c.half_extents = half;
    w.AddComponent<Collider2D>(e, c);
    if (with_body) {
        Rigidbody2D rb;
        rb.type = type;
        w.AddComponent<Rigidbody2D>(e, rb);
    }
    return e;
}

void Run(Physics2D& p, int steps, f32 dt = 1.0f / 60.0f) {
    for (int i = 0; i < steps; ++i) p.Step(dt);
}

} // namespace

AETHER_TEST(Physics2D_FallsAndRestsOnAFloor) {
    World world;
    const Entity floor = Box(world, Vec3(0, -0.5f, 0), Vec2{10, 0.5f}, BodyType2D::Static, false);
    (void)floor;
    const Entity ball = Box(world, Vec3(0, 3, 0), Vec2{0.5f, 0.5f}, BodyType2D::Dynamic);
    Physics2D physics(world);
    Run(physics, 20);
    CHECK(world.GetComponent<Transform>(ball)->position.y < 3.0f); // it fell
    Run(physics, 120);
    const Transform* t = world.GetComponent<Transform>(ball);
    CHECK(Near(t->position.y, 0.5f, 0.03f)); // resting on the floor's top (y = 0), its half height up
    CHECK(std::fabs(world.GetComponent<Rigidbody2D>(ball)->velocity.y) < 0.2f);
    CHECK(physics.IsGrounded(ball));
    CHECK(physics.WallSide(ball) == 0);
    CHECK(Near(t->position.z, 0.0f)); // z untouched
}

AETHER_TEST(Physics2D_BoxesStackAndBodiesPush) {
    World world;
    Box(world, Vec3(0, -0.5f, 0), Vec2{10, 0.5f}, BodyType2D::Static, false);
    const Entity low = Box(world, Vec3(0, 0.6f, 0), Vec2{0.5f, 0.5f}, BodyType2D::Dynamic);
    const Entity high = Box(world, Vec3(0, 2.0f, 0), Vec2{0.5f, 0.5f}, BodyType2D::Dynamic);
    Physics2D physics(world);
    Run(physics, 240);
    CHECK(Near(world.GetComponent<Transform>(low)->position.y, 0.5f, 0.05f));
    CHECK(Near(world.GetComponent<Transform>(high)->position.y, 1.5f, 0.08f)); // on top of the low one
    CHECK(physics.IsGrounded(high) && physics.IsGrounded(low));
    CHECK(std::fabs(world.GetComponent<Transform>(high)->position.x) < 0.05f);

    // A heavier box shoves a light one along when moving into it.
    World w2;
    Rigidbody2D heavy;
    heavy.mass = 10.0f;
    heavy.gravity_scale = 0.0f;
    heavy.velocity = {4, 0};
    const Entity a = Box(w2, Vec3(0, 0, 0), Vec2{0.5f, 0.5f}, BodyType2D::Dynamic);
    *w2.GetComponent<Rigidbody2D>(a) = heavy;
    Rigidbody2D light;
    light.gravity_scale = 0.0f;
    const Entity b = Box(w2, Vec3(1.2f, 0, 0), Vec2{0.5f, 0.5f}, BodyType2D::Dynamic);
    *w2.GetComponent<Rigidbody2D>(b) = light;
    Physics2D p2(w2);
    p2.settings.gravity = {0, 0};
    Run(p2, 30);
    CHECK(w2.GetComponent<Transform>(b)->position.x > 2.0f); // moved on
    CHECK(w2.GetComponent<Rigidbody2D>(b)->velocity.x > 3.0f);
    CHECK(w2.GetComponent<Transform>(a)->position.x < w2.GetComponent<Transform>(b)->position.x - 0.9f); // never overlapping
}

AETHER_TEST(Physics2D_BounceFrictionAndKinematic) {
    World world;
    Box(world, Vec3(0, -0.5f, 0), Vec2{10, 0.5f}, BodyType2D::Static, false);
    const Entity ball = Box(world, Vec3(0, 4, 0), Vec2{0.5f, 0.5f}, BodyType2D::Dynamic);
    world.GetComponent<Collider2D>(ball)->shape = Shape2D::Circle;
    world.GetComponent<Collider2D>(ball)->radius = 0.5f;
    world.GetComponent<Collider2D>(ball)->restitution = 0.8f;
    Physics2D physics(world);
    f32 peak_after = 0.0f;
    bool hit = false;
    for (int i = 0; i < 240; ++i) {
        physics.Step(1.0f / 60.0f);
        const f32 y = world.GetComponent<Transform>(ball)->position.y;
        const f32 vy = world.GetComponent<Rigidbody2D>(ball)->velocity.y;
        if (!hit && vy > 1.0f) hit = true; // it bounced back up
        if (hit) peak_after = std::max(peak_after, y);
    }
    CHECK(hit && peak_after > 1.5f && peak_after < 4.0f); // a lively bounce that loses height

    // Friction slows a box sliding on the floor; zero friction doesn't.
    World w2;
    Box(w2, Vec3(0, -0.5f, 0), Vec2{50, 0.5f}, BodyType2D::Static, false);
    const Entity rough = Box(w2, Vec3(0, 0.5f, 0), Vec2{0.5f, 0.5f}, BodyType2D::Dynamic);
    const Entity ice = Box(w2, Vec3(0, 0.5f, 0), Vec2{0.5f, 0.5f}, BodyType2D::Dynamic);
    w2.GetComponent<Collider2D>(rough)->friction = 1.0f;
    w2.GetComponent<Collider2D>(ice)->friction = 0.0f;
    w2.GetComponent<Collider2D>(rough)->mask = 1u; // they must not hit each other: one layer each
    w2.GetComponent<Collider2D>(rough)->layer = 2u;
    w2.GetComponent<Collider2D>(rough)->mask = 1u;
    w2.GetComponent<Collider2D>(ice)->layer = 4u;
    w2.GetComponent<Collider2D>(ice)->mask = 1u;
    w2.GetComponent<Rigidbody2D>(rough)->velocity = {6, 0};
    w2.GetComponent<Rigidbody2D>(ice)->velocity = {6, 0};
    Physics2D p2(w2);
    Run(p2, 60);
    CHECK(w2.GetComponent<Rigidbody2D>(rough)->velocity.x < 3.0f);
    CHECK(w2.GetComponent<Rigidbody2D>(ice)->velocity.x > 5.5f);

    // A kinematic platform carries nothing by itself but pushes bodies it moves into, and ignores gravity.
    World w3;
    const Entity platform = Box(w3, Vec3(0, 0, 0), Vec2{1, 0.25f}, BodyType2D::Kinematic);
    w3.GetComponent<Rigidbody2D>(platform)->velocity = {0, 1};
    Physics2D p3(w3);
    Run(p3, 60);
    CHECK(Near(w3.GetComponent<Transform>(platform)->position.y, 1.0f, 0.05f));
    CHECK(Near(w3.GetComponent<Rigidbody2D>(platform)->velocity.y, 1.0f)); // gravity never touched it
}

AETHER_TEST(Physics2D_LayersAndTriggers) {
    World world;
    Box(world, Vec3(0, -0.5f, 0), Vec2{10, 0.5f}, BodyType2D::Static, false);
    const Entity ghost = Box(world, Vec3(0, 3, 0), Vec2{0.5f, 0.5f}, BodyType2D::Dynamic);
    world.GetComponent<Collider2D>(ghost)->layer = 2u;
    world.GetComponent<Collider2D>(ghost)->mask = 4u; // collides with nothing that is here
    Physics2D physics(world);
    Run(physics, 90);
    CHECK(world.GetComponent<Transform>(ghost)->position.y < -2.0f); // fell through the floor on layer 1

    // A trigger volume reports enter and exit without pushing.
    World w2;
    const Entity zone = Box(w2, Vec3(0, 0, 0), Vec2{1, 1}, BodyType2D::Static, false);
    w2.GetComponent<Collider2D>(zone)->trigger = true;
    const Entity body = Box(w2, Vec3(-3, 0, 0), Vec2{0.25f, 0.25f}, BodyType2D::Dynamic);
    w2.GetComponent<Rigidbody2D>(body)->gravity_scale = 0.0f;
    w2.GetComponent<Rigidbody2D>(body)->velocity = {2, 0};
    Physics2D p2(w2);
    int enters = 0, exits = 0;
    for (int i = 0; i < 240; ++i) {
        p2.Step(1.0f / 60.0f);
        for (const Event2D& e : p2.Events()) {
            if (e.kind == Event2D::Kind::TriggerEnter) {
                ++enters;
                CHECK((e.a == body && e.b == zone) || (e.a == zone && e.b == body));
            }
            if (e.kind == Event2D::Kind::TriggerExit) {
                ++exits;
                CHECK((e.a == body && e.b == zone) || (e.a == zone && e.b == body));
            }
        }
    }
    CHECK(enters == 1 && exits == 1);
    CHECK(Near(w2.GetComponent<Rigidbody2D>(body)->velocity.x, 2.0f)); // not slowed by passing through
    CHECK(w2.GetComponent<Transform>(body)->position.x > 4.0f);
}

AETHER_TEST(Physics2D_ContactEventsBeginAndEnd) {
    World world;
    Box(world, Vec3(0, -0.5f, 0), Vec2{10, 0.5f}, BodyType2D::Static, false);
    const Entity box = Box(world, Vec3(0, 1.5f, 0), Vec2{0.5f, 0.5f}, BodyType2D::Dynamic);
    Physics2D physics(world);
    int begins = 0, ends = 0;
    bool end_named = false;
    for (int i = 0; i < 120; ++i) {
        physics.Step(1.0f / 60.0f);
        for (const Event2D& e : physics.Events()) {
            if (e.kind == Event2D::Kind::Begin) {
                ++begins;
                CHECK(e.a == box || e.b == box);
                CHECK(e.normal.y < -0.7f); // from the box, down at the floor
            }
        }
    }
    CHECK(begins == 1); // it lands once and rests, which isn't a new contact every frame
    // Launch it upward: the contact ends, naming the box.
    world.GetComponent<Rigidbody2D>(box)->velocity = {0, 8};
    for (int i = 0; i < 20; ++i) {
        physics.Step(1.0f / 60.0f);
        for (const Event2D& e : physics.Events()) {
            if (e.kind == Event2D::Kind::End) {
                ++ends;
                end_named = end_named || e.a == box || e.b == box;
            }
        }
    }
    CHECK(ends == 1 && end_named);
    CHECK(!physics.IsGrounded(box));
}

AETHER_TEST(Physics2D_TilemapCollisionAndWalls) {
    World world;
    Tileset ts;
    ts.texture = Guid();
    ts.texture_width = ts.texture_height = 32;
    ts.tile_width = ts.tile_height = 16;
    ts.SetSolid(1, true);
    TilemapData map;
    map.tileset = Guid();
    map.Resize(12, 6);
    map.AddLayer("L");
    map.Fill(0, 0, 0, 11, 1, 1); // a floor two tiles thick (top at y = 2)
    map.Fill(0, 8, 2, 8, 4, 1);  // a wall at x = 8 (spans x 8..9)
    Resolvers2D resolvers;
    resolvers.tilemaps = [&](const assets::AssetGuid&) { return &map; };
    resolvers.tilesets = [&](const assets::AssetGuid&) { return &ts; };
    const Entity level = world.CreateEntity();
    world.AddComponent<Transform>(level, Transform{Vec3(0, 0, 0), Quaternion::Identity()});
    TilemapRenderer r;
    r.pixels_per_unit = 16.0f; // a tile is one unit
    world.AddComponent<TilemapRenderer>(level, r);

    const Entity hero = Box(world, Vec3(2, 5, 0), Vec2{0.4f, 0.5f}, BodyType2D::Dynamic);
    Physics2D physics(world, resolvers);
    Run(physics, 120);
    CHECK(Near(world.GetComponent<Transform>(hero)->position.y, 2.5f, 0.04f)); // standing on the floor's top
    CHECK(physics.IsGrounded(hero));

    // Run right along the floor: seams between tiles don't catch it, the wall stops it.
    world.GetComponent<Collider2D>(hero)->friction = 0.0f;
    world.GetComponent<Rigidbody2D>(hero)->velocity = {6, 0};
    f32 min_speed = 100.0f;
    for (int i = 0; i < 90; ++i) {
        physics.Step(1.0f / 60.0f);
        const f32 x = world.GetComponent<Transform>(hero)->position.x;
        if (x > 3.0f && x < 6.5f) min_speed = std::min(min_speed, world.GetComponent<Rigidbody2D>(hero)->velocity.x);
    }
    CHECK(min_speed > 5.9f); // no catching on the seams between tiles
    const f32 x = world.GetComponent<Transform>(hero)->position.x;
    CHECK(x < 8.0f - 0.4f + 0.05f && x > 7.0f); // against the wall's left face
    CHECK(physics.WallSide(hero) == 1);
    CHECK(physics.IsGrounded(hero));
    bool tile_begin = false;
    // Falling into the map's floor again from above registers a tile contact event.
    world.GetComponent<Transform>(hero)->position = {4, 6, 0};
    world.GetComponent<Rigidbody2D>(hero)->velocity = {0, 0};
    for (int i = 0; i < 90; ++i) {
        physics.Step(1.0f / 60.0f);
        for (const Event2D& e : physics.Events()) {
            if (e.kind == Event2D::Kind::Begin && e.a == hero && e.b.IsNull()) tile_begin = true;
        }
    }
    CHECK(tile_begin);
}

AETHER_TEST(Physics2D_RaycastsAndOverlaps) {
    World world;
    Tileset ts;
    ts.texture_width = ts.texture_height = 32;
    ts.tile_width = ts.tile_height = 16;
    ts.SetSolid(1, true);
    TilemapData map;
    map.Resize(6, 3);
    map.AddLayer("L");
    map.Fill(0, 4, 0, 4, 2, 1);
    Resolvers2D resolvers;
    resolvers.tilemaps = [&](const assets::AssetGuid&) { return &map; };
    resolvers.tilesets = [&](const assets::AssetGuid&) { return &ts; };
    const Entity level = world.CreateEntity();
    world.AddComponent<Transform>(level, Transform{Vec3(0, 0, 0), Quaternion::Identity()});
    TilemapRenderer r;
    r.pixels_per_unit = 16.0f;
    world.AddComponent<TilemapRenderer>(level, r);
    const Entity crate = Box(world, Vec3(2, 1, 0), Vec2{0.5f, 0.5f}, BodyType2D::Static, false);
    const Entity orb = Box(world, Vec3(2, 2.5f, 0), Vec2{0.5f, 0.5f}, BodyType2D::Static, false);
    world.GetComponent<Collider2D>(orb)->shape = Shape2D::Circle;
    world.GetComponent<Collider2D>(orb)->radius = 0.4f;
    Physics2D physics(world, resolvers);

    RayHit2D hit;
    CHECK(physics.Raycast({0, 1}, {1, 0}, 20, hit)); // along y = 1: the crate (x 1.5) comes before the wall (x 4)
    CHECK(hit.entity == crate && !hit.tile && Near(hit.distance, 1.5f) && Near(hit.normal.x, -1.0f) && Near(hit.point.x, 1.5f));
    CHECK(physics.Raycast({3, 1}, {1, 0}, 20, hit)); // past the crate: the wall tile
    CHECK(hit.tile && hit.tile_x == 4 && Near(hit.distance, 1.0f) && Near(hit.normal.x, -1.0f));
    CHECK(!physics.Raycast({3, 1}, {1, 0}, 0.5f, hit)); // out of range
    CHECK(!physics.Raycast({0, 5}, {1, 0}, 20, hit));   // above everything
    CHECK(physics.Raycast({2, 6}, {0, -1}, 20, hit) && hit.entity == orb && Near(hit.distance, 6 - 2.9f)); // the circle's top
    CHECK(!physics.Raycast({0, 1}, {1, 0}, 20, hit, 2u)); // a layer mask that matches nothing
    CHECK(!physics.Raycast({0, 1}, {0, 0}, 20, hit));

    const std::vector<Entity> near_crate = physics.OverlapBox({2, 1}, {0.2f, 0.2f});
    CHECK(near_crate.size() == 1 && near_crate[0] == crate);
    CHECK(physics.OverlapBox({2, 1.8f}, {1, 1}).size() == 2);
    CHECK(physics.OverlapBox({-5, -5}, {1, 1}).empty());
}

AETHER_TEST(Physics2D_ComponentsSaveThroughReflection) {
    RegisterPhysics2DComponents();
    Collider2D c;
    c.shape = Shape2D::Circle;
    c.radius = 0.25f;
    c.trigger = true;
    c.layer = 8;
    const nlohmann::json j = reflect::ToJson(c);
    Collider2D back;
    CHECK(reflect::FromJson(back, j));
    CHECK(back.shape == Shape2D::Circle && Near(back.radius, 0.25f) && back.trigger && back.layer == 8);
    Rigidbody2D rb;
    rb.type = BodyType2D::Kinematic;
    rb.velocity = {1, 2};
    Rigidbody2D rb_back;
    CHECK(reflect::FromJson(rb_back, reflect::ToJson(rb)));
    CHECK(rb_back.type == BodyType2D::Kinematic && Near(rb_back.velocity.y, 2.0f));
    CHECK(Near(Vec2{3, 4}.Length(), 5.0f));
}

// --- 2D lights ----------------------------------------------------------------

#include "aether/sprite2d/lights2d.h"

namespace {

Entity LightAtPos(World& w, Vec3 at, Light2D light) {
    const Entity e = w.CreateEntity();
    w.AddComponent<Transform>(e, Transform{at, Quaternion::Identity()});
    w.AddComponent<Light2D>(e, light);
    return e;
}

Entity Caster(World& w, Vec3 at, Vec2 half) {
    const Entity e = w.CreateEntity();
    w.AddComponent<Transform>(e, Transform{at, Quaternion::Identity()});
    ShadowCaster2D c;
    c.half_extents = half;
    w.AddComponent<ShadowCaster2D>(e, c);
    return e;
}

bool InsidePolygon(const std::vector<Vec2>& poly, Vec2 p) {
    bool inside = false;
    for (usize i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        if (((poly[i].y > p.y) != (poly[j].y > p.y)) &&
            (p.x < (poly[j].x - poly[i].x) * (p.y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x)) {
            inside = !inside;
        }
    }
    return inside;
}

Entity FindLamp(World& w) {
    Entity found;
    w.ForEachArchetype([&](const Archetype& a) {
        for (usize c = 0; c < a.ChunkCount(); ++c) {
            for (u32 i = 0; i < a.ChunkEntityCount(c); ++i) {
                if (w.GetComponent<Light2D>(a.EntityArray(c)[i])) found = a.EntityArray(c)[i];
            }
        }
    });
    return found;
}

struct TileWorld {
    World world;
    Tileset ts;
    TilemapData map;
    Resolvers2D resolvers;
    TileWorld() {
        RegisterSprite2DComponents();
        RegisterLight2DComponents();
        ts.texture_width = ts.texture_height = 32;
        ts.tile_width = ts.tile_height = 16;
        ts.SetSolid(1, true);
        map.Resize(20, 10);
        map.AddLayer("L");
        resolvers.tilemaps = [this](const assets::AssetGuid&) { return &map; };
        resolvers.tilesets = [this](const assets::AssetGuid&) { return &ts; };
        const Entity level = world.CreateEntity();
        world.AddComponent<Transform>(level, Transform{Vec3(0, 0, 0), Quaternion::Identity()});
        TilemapRenderer r;
        r.pixels_per_unit = 16.0f; // a tile is a unit
        world.AddComponent<TilemapRenderer>(level, r);
    }
};

} // namespace

AETHER_TEST(Lights2D_PointLightFalloffAndColor) {
    RegisterLight2DComponents();
    World world;
    Light2D lamp;
    lamp.radius = 4.0f;
    lamp.falloff = 1.0f;
    lamp.color = {1.0f, 0.5f, 0.25f, 1.0f};
    lamp.intensity = 2.0f;
    const Entity e = LightAtPos(world, Vec3(1, 1, 0), lamp);
    const std::vector<Segment2D> none;
    LightColor c = LightAt(world, none, {1, 1});
    CHECK(Near(c.r, 2.0f) && Near(c.g, 1.0f) && Near(c.b, 0.5f)); // at the lamp: full, tinted
    c = LightAt(world, none, {3, 1});                              // halfway out
    CHECK(Near(c.r, 1.0f) && Near(c.g, 0.5f) && Near(c.b, 0.25f));
    c = LightAt(world, none, {5, 1});                              // at the radius
    CHECK(Near(c.r, 0.0f) && Near(c.g, 0.0f));
    CHECK(Near(LightAt(world, none, {9, 9}).r, 0.0f));

    world.GetComponent<Light2D>(e)->falloff = 2.0f; // steeper: (1 - 0.5)^2 * 2
    CHECK(Near(LightAt(world, none, {3, 1}).r, 0.5f));
    world.GetComponent<Light2D>(e)->enabled = false;
    CHECK(Near(LightAt(world, none, {1, 1}).r, 0.0f));

    // A global light adds the same everywhere, and lights add up.
    world.GetComponent<Light2D>(e)->enabled = true;
    Light2D ambient;
    ambient.type = Light2DType::Global;
    ambient.color = {0.2f, 0.2f, 0.4f, 1.0f};
    ambient.intensity = 1.0f;
    LightAtPos(world, Vec3(100, 100, 0), ambient);
    c = LightAt(world, none, {1, 1});
    CHECK(Near(c.r, 2.2f) && Near(c.b, 0.4f + 0.5f));
    c = LightAt(world, none, {50, 50});
    CHECK(Near(c.r, 0.2f) && Near(c.g, 0.2f) && Near(c.b, 0.4f));
}

AETHER_TEST(Lights2D_SpotConeAndRotation) {
    RegisterLight2DComponents();
    World world;
    Light2D spot;
    spot.type = Light2DType::Spot;
    spot.radius = 10.0f;
    spot.falloff = 1.0f;
    spot.direction_degrees = 0.0f; // along +x
    spot.inner_angle = 10.0f;
    spot.outer_angle = 30.0f;
    const Entity e = LightAtPos(world, Vec3(0, 0, 0), spot);
    const std::vector<Segment2D> none;
    const f32 along = LightAt(world, none, {5, 0}).r;
    CHECK(Near(along, 0.5f));
    // 5 units out at 20 degrees (halfway across the soft edge: smoothstep(0.5) = 0.5).
    const f32 ang = 20.0f * 3.14159265f / 180.0f;
    const f32 half = LightAt(world, none, {5 * std::cos(ang), 5 * std::sin(ang)}).r;
    CHECK(half > 0.0f && half < along);
    CHECK(Near(LightAt(world, none, {5 * std::cos(0.1f), 5 * std::sin(0.1f)}).r, 0.5f * (1.0f - 0.0f), 0.01f)); // inside the inner cone
    CHECK(Near(LightAt(world, none, {0, 5}).r, 0.0f));   // 90 degrees off: dark
    CHECK(Near(LightAt(world, none, {-5, 0}).r, 0.0f));  // behind
    // Turn the entity a quarter turn about z: the cone now points up.
    const f32 s = std::sqrt(0.5f);
    world.GetComponent<Transform>(e)->rotation = Quaternion(0, 0, s, s);
    CHECK(Near(LightAt(world, none, {0, 5}).r, 0.5f, 0.01f) && Near(LightAt(world, none, {5, 0}).r, 0.0f));
    world.GetComponent<Transform>(e)->rotation = Quaternion::Identity();
    world.GetComponent<Light2D>(e)->direction_degrees = 180.0f; // and the light's own direction adds
    CHECK(Near(LightAt(world, none, {-5, 0}).r, 0.5f, 0.01f));
}

AETHER_TEST(Lights2D_ShadowCastersBlockLight) {
    RegisterLight2DComponents();
    World world;
    Light2D lamp;
    lamp.radius = 10.0f;
    lamp.falloff = 1.0f;
    const Entity e = LightAtPos(world, Vec3(0, 0, 0), lamp);
    Caster(world, Vec3(3, 0, 0), Vec2{0.5f, 0.5f});
    const std::vector<Segment2D> occluders = GatherOccluders(world, {}, {-20, -20}, {20, 20});
    CHECK(occluders.size() == 4); // one box
    CHECK(LightAt(world, occluders, {5, 0}).r == 0.0f);          // behind the box
    CHECK(LightAt(world, occluders, {5, 3}).r > 0.2f);           // beside it
    CHECK(LightAt(world, occluders, {2, 0}).r > 0.7f);           // in front
    world.GetComponent<Light2D>(e)->cast_shadows = false;
    CHECK(LightAt(world, occluders, {5, 0}).r > 0.3f);           // this lamp ignores casters
    CHECK(Occluded(occluders, {0, 0}, {5, 0}) && !Occluded(occluders, {0, 0}, {2, 0}) && !Occluded(occluders, {0, 0}, {0, 5}));
    CHECK(GatherOccluders(world, {}, {-20, 10}, {20, 20}).empty()); // out of the region
}

AETHER_TEST(Lights2D_TileWallsAreLinesNotGrids) {
    TileWorld tw;
    tw.map.Fill(0, 4, 2, 6, 4, 1); // a 3x3 block
    const std::vector<Segment2D> segs = GatherOccluders(tw.world, tw.resolvers, {0, 0}, {20, 10});
    CHECK(segs.size() == 12); // the perimeter: 3 edges a side
    const std::vector<Segment2D> culled = GatherOccluders(tw.world, tw.resolvers, {10, 0}, {20, 10});
    CHECK(culled.empty());
    tw.map.Set(0, 5, 3, kEmpty); // a hole in the middle: its four faces are exposed too
    CHECK(GatherOccluders(tw.world, tw.resolvers, {0, 0}, {20, 10}).size() == 16);
}

AETHER_TEST(Lights2D_LightMapShadowsAndWalls) {
    TileWorld tw;
    tw.map.Fill(0, 10, 0, 10, 9, 1); // a wall the full height at x = 10
    Light2D lamp;
    lamp.radius = 12.0f;
    lamp.falloff = 1.0f;
    LightAtPos(tw.world, Vec3(5.5f, 5.5f, 0), lamp);
    const LightMap lit = BuildLightMap(tw.world, tw.resolvers, {0, 0}, {20, 10}, 1.0f);
    CHECK(lit.width == 20 && lit.height == 10 && lit.cells.size() == 200);
    CHECK(lit.At(5, 5).r > 0.95f);        // at the lamp
    CHECK(lit.At(8, 5).r > 0.3f);         // in front of the wall
    CHECK(lit.At(12, 5).r == 0.0f);       // behind it: in shadow
    CHECK(lit.At(15, 5).r == 0.0f);
    // The wall's own cells take the light of the air in front of them.
    CHECK(lit.At(10, 5).r > 0.3f && Near(lit.At(10, 5).r, lit.At(9, 5).r, 0.01f));
    // Sampling blends cells and clamps at the edges.
    const LightColor mid = lit.Sample(6.0f, 5.5f);
    CHECK(mid.r < lit.At(5, 5).r && mid.r > lit.At(6, 5).r);
    CHECK(Near(lit.Sample(-5, 5.5f).r, lit.At(0, 5).r) && Near(lit.Sample(99, 99).r, lit.At(19, 9).r));
    // Without shadows the back of the wall is lit as well.
    tw.world.GetComponent<Light2D>(FindLamp(tw.world))->cast_shadows = false;
    CHECK(BuildLightMap(tw.world, tw.resolvers, {0, 0}, {20, 10}, 1.0f).At(12, 5).r > 0.2f);
    // Bad regions and sizes give an empty map.
    CHECK(BuildLightMap(tw.world, tw.resolvers, {0, 0}, {20, 10}, 0.0f).cells.empty());
    CHECK(BuildLightMap(tw.world, tw.resolvers, {5, 5}, {5, 9}, 1.0f).cells.empty());
}

AETHER_TEST(Lights2D_VisibilityPolygon) {
    // No occluders: the square round the light (rays stop at it).
    std::vector<Vec2> poly = VisibilityPolygon({0, 0}, 5.0f, {});
    CHECK(poly.size() >= 8);
    for (const Vec2& p : poly) CHECK(p.Length() <= 5.0f * 1.4143f + 1e-3f && p.Length() >= 5.0f - 1e-3f);
    CHECK(InsidePolygon(poly, {4, 4}) && InsidePolygon(poly, {-4, 3}) && !InsidePolygon(poly, {6, 0}));

    // A wall across the way casts a shadow behind it.
    std::vector<Segment2D> wall = {{{2, -1}, {2, 1}}};
    poly = VisibilityPolygon({0, 0}, 5.0f, wall);
    CHECK(InsidePolygon(poly, {1, 0}) && InsidePolygon(poly, {1.9f, 0.5f}));
    CHECK(!InsidePolygon(poly, {3, 0}) && !InsidePolygon(poly, {4, 0.5f}));
    CHECK(InsidePolygon(poly, {3, 3}) && InsidePolygon(poly, {-3, 0}));
    // Every ray's end is at the wall or the bounds: nothing reaches past the wall inside its shadow.
    for (const Vec2& p : poly) CHECK(!(p.x > 2.01f && std::fabs(p.y) < 0.4f));
    CHECK(VisibilityPolygon({0, 0}, 0.0f, wall).empty());
}

AETHER_TEST(Lights2D_ComponentsSaveThroughReflection) {
    Light2D spot;
    spot.type = Light2DType::Spot;
    spot.color = {0.1f, 0.2f, 0.3f, 1.0f};
    spot.radius = 7.0f;
    spot.outer_angle = 55.0f;
    spot.cast_shadows = false;
    Light2D back;
    CHECK(reflect::FromJson(back, reflect::ToJson(spot)));
    CHECK(back.type == Light2DType::Spot && Near(back.color.g, 0.2f) && Near(back.radius, 7.0f) && Near(back.outer_angle, 55.0f) && !back.cast_shadows);
    ShadowCaster2D box;
    box.half_extents = {2, 3};
    ShadowCaster2D box_back;
    CHECK(reflect::FromJson(box_back, reflect::ToJson(box)) && Near(box_back.half_extents.y, 3.0f));
}
