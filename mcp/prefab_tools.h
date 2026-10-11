#pragma once

// MCP tools for prefabs (.aprefab: a reusable group of entities placed into scenes as instances that follow later edits to the prefab, except
// for the fields an instance overrides). Look inside a prefab (its entities, nested prefabs, variant base, what it flattens to, which prefabs
// and scenes depend on it), capture an entity and its descendants from the editor scene as a new prefab asset, place an instance in the
// editor scene, list an instance's overrides, and revert them or apply them back to the prefab (re-resolving every instance of it).
//
// Prefabs are project assets, so these need a project open (project_open). They act on the editor scene's world directly: the entities they
// create or change are not on the undo stack.

#include "asset_tools.h"
#include "editor_tools.h"
#include "mcp_server.h"

#include <memory>

namespace aether::mcp {

void RegisterPrefabTools(McpServer& server, std::shared_ptr<detail::AssetHost> host, EditorSession& session);

} // namespace aether::mcp
