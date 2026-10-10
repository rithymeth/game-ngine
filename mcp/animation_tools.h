#pragma once

// MCP tools for animation data: animation graphs (.aanim: variables, pose nodes, state machines), blend spaces (.ablend) and montages
// (.amontage). Check one against the animation module's own diagnostics, ask a blend space for the clip weights at a point, and run a graph
// headless: set variables, fire triggers and play montages over a list of steps and read back the state each machine is in, the state
// changes and the events (notifies, montage sections) that came out. These files aren't project assets with an importer, so the tools take
// the definition as JSON or a file under the open project's Content/.
//
// The simulation has no skeleton or real clips: every clip a graph names is a one-bone clip of a given length (1 second unless `clips`
// says otherwise), which is enough for state machines, transitions that wait for a clip to finish, blend weights and montage sections.

#include "asset_tools.h"
#include "mcp_server.h"

#include <memory>

namespace aether::mcp {

void RegisterAnimationTools(McpServer& server, std::shared_ptr<detail::AssetHost> host);

} // namespace aether::mcp
