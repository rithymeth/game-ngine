#pragma once

// MCP tools for localization: the project's string tables (.astrings, CSV with a column per language), the strings the content
// uses (gathered from scenes, prefabs, UI layouts and Blueprints), and the loc module's own message format, plural rules and
// pseudo-localization. Check translations for missing and mismatched placeholders, keep the table in step with the content,
// round-trip .po files for translators, and preview how text formats, pluralizes and expands in any language.

#include "asset_tools.h"
#include "mcp_server.h"

#include <memory>

namespace aether::mcp {

void RegisterLocTools(McpServer& server, std::shared_ptr<detail::AssetHost> host);

} // namespace aether::mcp
