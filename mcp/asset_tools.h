#pragma once

// MCP tools for a project's content: open a project, browse and inspect its
// assets, add source files, import them, change import settings, move/rename/
// delete with references kept intact, read and write text content, and cook the
// project into an .apak archive. They use the engine's own asset database,
// importers and cooker, in process.
//
// The project is opened with project_open; the other tools act on it. The game
// tools (game_load) can then run what asset_cook produced.

#include "mcp_server.h"

#include <memory>

namespace aether::mcp {

namespace detail {
struct AssetHost;
}

// `host` is the open project, shared with the kit tools (see RegisterKitTools).
void RegisterAssetTools(McpServer& server, std::shared_ptr<detail::AssetHost> host);
// Makes the shared project holder.
std::shared_ptr<detail::AssetHost> MakeAssetHost();

} // namespace aether::mcp
