#pragma once

// MCP tools for save files: the slots in a save folder (`<slot>.asav`: JSON in a versioned, checksummed envelope with the previous save kept
// as `.bak`), what one claims to be and whether its checksum holds, deleting a slot, and the player's settings file
// (`settings.asettings`: quality, resolution, volumes, language, key rebinds) read, changed with the engine's own validation, and reset.
//
// A game's saves live in a user folder the game picks (the hosted game reports it as `saves` when loaded), so these tools take the folder
// as `directory`.

#include "asset_tools.h"
#include "mcp_server.h"

#include <memory>

namespace aether::mcp {

void RegisterSaveTools(McpServer& server);

} // namespace aether::mcp
