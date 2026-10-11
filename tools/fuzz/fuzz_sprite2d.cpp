// Fuzz target: the 2D data loaders (Phase 47 step 1). The first input byte picks the sprite atlas, tileset or
// tilemap loader; the rest is the JSON text.
#include "fuzz_common.h"

#include "aether/sprite2d/atlas.h"
#include "aether/sprite2d/tilemap.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    FuzzQuietLogs();
    if (size == 0) return 0;
    const auto json = nlohmann::json::parse(data + 1, data + size, nullptr, /*allow_exceptions=*/false);
    std::string error;
    switch (data[0] % 3) {
    case 0: {
        aether::sprite2d::SpriteAtlas atlas;
        (void)aether::sprite2d::AtlasFromJson(json, atlas, &error);
        break;
    }
    case 1: {
        aether::sprite2d::Tileset tileset;
        (void)aether::sprite2d::TilesetFromJson(json, tileset, &error);
        break;
    }
    default: {
        aether::sprite2d::TilemapData map;
        (void)aether::sprite2d::TilemapFromJson(json, map, &error);
        break;
    }
    }
    return 0;
}
