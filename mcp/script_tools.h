#pragma once

// MCP tools for Luau scripts (.luau): check one compiles and see what it exposes (its variables with their ranges and tooltips, and the lifecycle
// callbacks it defines), get code completion for the engine API, run a chunk or call a function in the sandbox, and simulate a script on an entity
// for some ticks (OnCreate, OnStart, OnUpdate ... with timers) to see what it prints, which variables it ends with and what goes wrong. The whole
// project's scripts are checked at once, along with every ScriptComponent in the scenes: the script must exist and the variables it overrides must be
// ones the script exposes, with the right kind of value.
//
// Only built where the engine has the Luau scripting module (AETHER_BUILD_SCRIPTING).

#include "asset_tools.h"
#include "mcp_server.h"

#include <memory>

namespace aether::mcp {

void RegisterScriptTools(McpServer& server, std::shared_ptr<detail::AssetHost> host);

} // namespace aether::mcp
