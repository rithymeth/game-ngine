#pragma once

// MCP tools for Blueprints (.abp), the engine's visual scripting: find the node types and their pins, validate a graph with
// the Blueprint module's own diagnostics (BP001...), compile it to bytecode, and run it headless on a test entity to see what it
// prints, which variables it changes and what goes wrong, so an agent can write a graph, run it and check the result. Creating,
// reading and listing Blueprints goes through the kit_* tools (type "blueprint").

#include "asset_tools.h"
#include "mcp_server.h"

#include <memory>

namespace aether::mcp {

void RegisterBlueprintTools(McpServer& server, std::shared_ptr<detail::AssetHost> host);

// A small working Blueprint (a variable, a custom event and a BeginPlay) as JSON, for kit_schema.
Json BlueprintSampleJson();

} // namespace aether::mcp
