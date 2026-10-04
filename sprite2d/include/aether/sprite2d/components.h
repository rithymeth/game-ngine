#pragma once

#include "aether/assets/asset_ref.h"
#include "aether/core/base.h"
#include "aether/ecs/world.h"
#include "aether/reflection/reflection.h"
#include "aether/scene/components.h"
#include "aether/sprite2d/atlas.h"

#include <cstring>
#include <functional>
#include <string>

// The 2D scene components (Phase 26 step 5, docs/design/PHASE_SPECS.md
// §26.6). Sprites and tilemaps sit on the entity's Transform: x and y are
// the 2D position, z orders depth, and the rotation turns them (in the
// plane, about z).

namespace aether::sprite2d {

struct SpriteAtlasAsset {
    static constexpr const char* kImporter = "SpriteAtlas";
};
struct TilesetAsset {
    static constexpr const char* kImporter = "Tileset";
};
struct TilemapAsset {
    static constexpr const char* kImporter = "Tilemap";
};

struct SpriteColor {
    f32 r = 1, g = 1, b = 1, a = 1;
};

// One frame of an atlas, drawn at the entity.
struct Sprite {
    assets::AssetRef<SpriteAtlasAsset> atlas;
    char frame[32] = {};
    SpriteColor color;
    bool flip_x = false, flip_y = false;
    // Drawn back to front by layer, then order, then z.
    i32 sorting_layer = 0;
    i32 order = 0;
    f32 pixels_per_unit = 16.0f;
};

// Plays one of the atlas's clips on the entity's Sprite.
struct SpriteAnimator {
    char clip[32] = {};
    f32 time = 0.0f;
    f32 speed = 1.0f;
    bool playing = true;
    bool finished = false; // a clip that doesn't loop reached its end
};

// Draws a tilemap asset at the entity (its origin is the map's bottom left).
struct TilemapRenderer {
    assets::AssetRef<TilemapAsset> tilemap;
    i32 sorting_layer = 0;
    i32 order = 0;
    f32 pixels_per_unit = 16.0f;
    SpriteColor color;
};

template <usize N>
inline void SetName(char (&buffer)[N], const std::string& text) {
    std::memset(buffer, 0, N);
    std::strncpy(buffer, text.c_str(), N - 1);
}
inline void SetSpriteFrame(Sprite& sprite, const std::string& name) { SetName(sprite.frame, name); }
inline void PlayClip(SpriteAnimator& animator, const std::string& clip, bool restart = true) {
    if (!restart && clip == animator.clip) return;
    SetName(animator.clip, clip);
    animator.time = 0.0f;
    animator.playing = true;
    animator.finished = false;
}

using AtlasResolver = std::function<const SpriteAtlas*(const assets::AssetGuid&)>;

// Advances each SpriteAnimator by `dt` and sets its Sprite's frame to the
// clip's frame at that time; a clip that doesn't loop stops at its end.
void UpdateSpriteAnimations(World& world, const AtlasResolver& atlases, f32 dt);

// Makes the 2D component types known to scene loading.
void RegisterSprite2DComponents();

} // namespace aether::sprite2d

AETHER_REFLECT(aether::sprite2d::SpriteColor, 1,
    AETHER_FIELD(r, Field_EditAnywhere, {.range_min = 0.0, .range_max = 1.0}),
    AETHER_FIELD(g, Field_EditAnywhere, {.range_min = 0.0, .range_max = 1.0}),
    AETHER_FIELD(b, Field_EditAnywhere, {.range_min = 0.0, .range_max = 1.0}),
    AETHER_FIELD(a, Field_EditAnywhere, {.range_min = 0.0, .range_max = 1.0})
)

AETHER_REFLECT(aether::sprite2d::Sprite, 1,
    AETHER_FIELD(atlas, Field_EditAnywhere, {.tooltip = "The sprite atlas asset"}),
    AETHER_FIELD(frame, Field_EditAnywhere, {.tooltip = "The frame's name in the atlas"}),
    AETHER_FIELD(color, Field_EditAnywhere, {.tooltip = "Tint and opacity"}),
    AETHER_FIELD(flip_x, Field_EditAnywhere),
    AETHER_FIELD(flip_y, Field_EditAnywhere),
    AETHER_FIELD(sorting_layer, Field_EditAnywhere, {.tooltip = "Drawn back to front by layer first"}),
    AETHER_FIELD(order, Field_EditAnywhere, {.tooltip = "Within a layer, higher draws over lower"}),
    AETHER_FIELD(pixels_per_unit, Field_EditAnywhere, {.tooltip = "Art pixels per world unit", .range_min = 1.0, .range_max = 4096.0})
)

AETHER_REFLECT(aether::sprite2d::SpriteAnimator, 1,
    AETHER_FIELD(clip, Field_EditAnywhere, {.tooltip = "The clip's name in the atlas"}),
    AETHER_FIELD(time, Field_EditAnywhere, {.units = "s"}),
    AETHER_FIELD(speed, Field_EditAnywhere, {.range_min = 0.0, .range_max = 16.0}),
    AETHER_FIELD(playing, Field_EditAnywhere),
    AETHER_FIELD(finished, Field_ReadOnly)
)

AETHER_REFLECT(aether::sprite2d::TilemapRenderer, 1,
    AETHER_FIELD(tilemap, Field_EditAnywhere, {.tooltip = "The tilemap asset"}),
    AETHER_FIELD(sorting_layer, Field_EditAnywhere),
    AETHER_FIELD(order, Field_EditAnywhere),
    AETHER_FIELD(pixels_per_unit, Field_EditAnywhere, {.tooltip = "Art pixels per world unit", .range_min = 1.0, .range_max = 4096.0}),
    AETHER_FIELD(color, Field_EditAnywhere)
)
