#pragma once

#include "aether/assets/asset_guid.h"
#include "aether/ecs/world.h"
#include "aether/sprite2d/components.h"
#include "aether/sprite2d/tilemap.h"

#include <functional>
#include <vector>

// Turns a world's sprites and tilemaps into draw batches (Phase 26 step 5,
// docs/design/PHASE_SPECS.md §26.6): quads sorted back to front, merged into
// runs that share a texture, ready for any renderer to upload and draw.

namespace aether::sprite2d {

struct SpriteVertex {
    f32 x, y, z;
    f32 u, v;
    f32 r, g, b, a;
};

// One draw: `vertices` are quads of four (two triangles each by `indices`).
struct SpriteBatch {
    assets::AssetGuid texture;
    std::vector<SpriteVertex> vertices;
    std::vector<u32> indices;
    usize QuadCount() const { return indices.size() / 6; }
};

struct Resolvers2D {
    AtlasResolver atlases;
    std::function<const Tileset*(const assets::AssetGuid&)> tilesets;
    std::function<const TilemapData*(const assets::AssetGuid&)> tilemaps;
};

// What the camera sees, in world x and y; quads outside are left out.
struct ViewRect {
    f32 min_x = 0, min_y = 0, max_x = 0, max_y = 0;
};

// Every Sprite and TilemapRenderer (on an entity with a Transform) as
// batches in draw order: by sorting layer, order, then z; ties keep the
// order sprites come in. `view` (optional) culls.
std::vector<SpriteBatch> BuildSpriteBatches(const World& world, const Resolvers2D& resolvers,
                                            const ViewRect* view = nullptr);

} // namespace aether::sprite2d
