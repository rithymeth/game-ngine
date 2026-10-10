#pragma once

// MCP tools for the navigation module: a navigation workbench held in the server's memory. Describe the ground (planes, boxes,
// triangles, the workbench terrain, and the NavObstacle / NavModifierVolume / NavLinkProxy components of the editor scene),
// bake a navigation mesh with Recast, then ask Detour about it: paths with area costs, nearest points, raycasts, random points,
// reachability. Look at the mesh and paths as a picture, and run a crowd of NavAgents over it headless to see where they go,
// when they arrive and whether they get in each other's way. Meshes can be saved and loaded as .anav files.
//
// Scope: the player does not run navigation (no NavWorld or NavCrowd in the cooked game), so these work on the workbench, not
// on the hosted game. Geometry from ModelRenderer entities is not available (it needs the model assets); describe the ground
// with shapes, triangles or the terrain workbench. NavAgent and the nav components are edited with nav_component_set
// (editor_tools.cpp).

#include "editor_tools.h"
#include "mcp_server.h"

namespace aether::mcp {

// `session` is the editor scene the nav components are read from; it must outlive the server.
void RegisterNavTools(McpServer& server, EditorSession& session);

} // namespace aether::mcp
