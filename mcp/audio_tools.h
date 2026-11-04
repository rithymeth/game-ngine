#pragma once

// MCP tools for the audio kit's sound cues (.acue): validate a cue with the kit's own diagnostics (checking that its
// sounds exist in the open project), and preview what a cue would play (which sounds, when, how loud, how many times
// over) without any audio output. Creating, reading and listing cues goes through the kit_* tools (kit_put, kit_get,
// kit_list, kit_schema with type "sound_cue"); AudioSource/AudioListener/ReverbZone components are edited with
// audio_set (editor_tools.cpp) and the running game's audio with the game_audio_* tools (game_tools.cpp).

#include "asset_tools.h"
#include "mcp_server.h"

#include <memory>

namespace aether::mcp {

void RegisterAudioTools(McpServer& server, std::shared_ptr<detail::AssetHost> host);

} // namespace aether::mcp
