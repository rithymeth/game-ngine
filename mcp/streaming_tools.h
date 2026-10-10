#pragma once

// MCP tools for world partition (the streaming module): split the editor scene into grid cells on the ground plane, each saved as its own
// scene beside a persistent one and a `world.aworld` index; read a partitioned world back and check its files; and walk a streaming source
// along a path to see which cells load and unload, in what order and how many entities that puts in the world.
//
// Scope: the cooked game does not stream a partitioned world (no WorldStreamer in the player), so the simulation runs the streamer on a
// scratch world, not the hosted game.

#include "editor_tools.h"
#include "mcp_server.h"

namespace aether::mcp {

// `session` is the editor scene that world_partition splits; it must outlive the server.
void RegisterStreamingTools(McpServer& server, EditorSession& session);

} // namespace aether::mcp
