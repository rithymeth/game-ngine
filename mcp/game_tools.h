#pragma once

// MCP tools that host a full cooked Game (player/ runtime): every system the
// shipped game runs -- physics, 2D physics, scripts, Blueprints, audio,
// sequences, the gameplay kits, saves, localization -- driven headless, frame
// by frame, by the agent.
//
// This is separate from the editor session (editor_tools.h): the editor edits
// the authored scene undoably; the game tools run the cooked build of it. The
// game's world is not the editor's, and edits made to it (game_set_component)
// are not undoable and vanish on game_unload / game_load.

#include "mcp_server.h"

namespace aether::mcp {

// Adds the game_* tools. The hosted game lives as long as the server's tools.
void RegisterGameTools(McpServer& server);

} // namespace aether::mcp
