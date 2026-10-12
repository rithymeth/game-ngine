#pragma once

// MCP tools for the gameplay kits' data: the definitions a game is built from.
// Gameplay effects (.aeffect), abilities (.aability), items (.aitem), quests
// (.aquest) and input actions and mappings (.aaction, .amapping): learn each
// format from its template, validate a definition before writing it, create and
// read definitions in the open project, and check the whole set for broken
// cross-references (an ability's cost effect, an item's use effect, a quest's
// reward items and prerequisites).
//
// They use the project opened with project_open (asset_tools.h) and the kits' own
// parsers and validators, so what they accept is what the game loads.

#include "asset_tools.h"
#include "mcp_server.h"

#include <memory>

namespace aether::mcp {

void RegisterKitTools(McpServer& server, std::shared_ptr<detail::AssetHost> host);

} // namespace aether::mcp
