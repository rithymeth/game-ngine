#pragma once

// MCP tools for the terrain module: a terrain workbench held in the server's memory. Create a heightmap terrain (flat or
// procedural), sculpt and paint it with the editor's own brushes (undoable), sample heights and slopes, see it as a shaded
// picture, plan its chunks and LODs for a camera, scatter foliage over it, and save or load it as files.
//
// Honest scope: the terrain module is a standalone library. The engine has no terrain component, no terrain asset type and
// no terrain in the player, so a terrain made here is not part of any scene or cooked game. The files these tools write
// (.r16 heights, .splat weights and a .terrain.json) are the MCP server's own interchange format, for keeping and sharing
// the workbench; nothing else in the engine reads them yet.

#include "mcp_server.h"

namespace aether::mcp {

void RegisterTerrainTools(McpServer& server);

} // namespace aether::mcp
