#pragma once

// MCP tools for 2D levels: tilesets (.atileset: a texture cut into equal tiles, with a collision flag per tile and autotile rules)
// and tilemaps (.atilemap: layers of cells over a tileset). Make a tileset from a texture, lay a map out in text (ASCII with a
// legend, in the way an agent thinks about a level), read it back as text, edit it with set / fill / line / replace / stamp
// operations, check it against its tileset, and see it as a PNG rendered from the real tile pictures with the autotiles resolved
// and the solid cells marked. Maps and tilesets also work through the kit_* tools (types "tilemap" and "tileset").

#include "asset_tools.h"
#include "mcp_server.h"

#include <memory>

namespace aether::mcp {

void RegisterTilemapTools(McpServer& server, std::shared_ptr<detail::AssetHost> host);

// Small working examples as JSON, for kit_schema.
Json TilesetSampleJson();
Json TilemapSampleJson();

} // namespace aether::mcp
