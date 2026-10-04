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
