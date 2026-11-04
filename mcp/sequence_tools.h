#pragma once

// MCP tools for level sequences (.asequence), the engine's cutscenes: tracks bound to entities that animate transforms and
// component fields, fire events, play sounds and animations, spawn prefabs, cut between cameras, fade the screen and play other
// sequences. Validate one with the sequencer's own diagnostics and against the project (the component fields it animates, the
// cues, prefabs and sub-sequences it names, loops between sequences, the entities it is bound to), sample its tracks at times
// (what the camera, the door, the fade are doing at 2.5 seconds), and list everything it fires in time order. Creating, reading
// and listing sequences goes through the kit_* tools (type "sequence"); the running game's sequences are driven with the
// game_*sequence* tools (game_tools.cpp).

#include "asset_tools.h"
#include "mcp_server.h"

#include <memory>

namespace aether::mcp {

void RegisterSequenceTools(McpServer& server, std::shared_ptr<detail::AssetHost> host);

// A small working sequence (a door that rises, a sound event and a fade) as JSON, for kit_schema.
Json SequenceSampleJson();

} // namespace aether::mcp
