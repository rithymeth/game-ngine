#pragma once

// MCP tools for materials (node graphs with typed pins that compile to HLSL): the node library with each node's pins, validating a graph
// (the renderer's MT001.. diagnostics and each connected pin's inferred type), compiling it to the HLSL include the renderer's passes call (with
// the parameter buffer layout, the textures it samples, the features it needs and a rough cost), and resolving a material instance chain
// to the parameter values a draw would use.
//
// Materials (.amat), functions (.amf) and instances (.amati) have no importer, so the tools take them as JSON or a file under the open
// project's Content/. The graph is compiled and checked on the CPU only: no shader is built and nothing is drawn.

#include "asset_tools.h"
#include "mcp_server.h"

#include <memory>

namespace aether::mcp {

void RegisterMaterialTools(McpServer& server, std::shared_ptr<detail::AssetHost> host);

} // namespace aether::mcp
