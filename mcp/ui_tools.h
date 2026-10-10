#pragma once

// MCP tools for UI layouts (.aui: a widget tree as JSON with data bindings): list the widget types and what each looks like by default,
// check a layout (it loads, names are unique, bindings point at widgets and properties that exist, nothing sits off screen or has no size)
// and read back where every widget lands at a screen size, and render it to a PNG. Layouts have no importer, so the tools take the layout
// as JSON or a file under the open project's Content/.
//
// The preview uses the built-in fixed-width font (text shows as blocks of the right size and place) and paints images as flat grey
// tinted rectangles: it shows layout, anchoring, sizing, colours and clipping, not final art.

#include "asset_tools.h"
#include "mcp_server.h"

#include <memory>

namespace aether::mcp {

void RegisterUiTools(McpServer& server, std::shared_ptr<detail::AssetHost> host);

} // namespace aether::mcp
