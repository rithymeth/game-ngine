#pragma once

// MCP tools for particle effects (.avfx): learn the format (a sample and every module's defaults), validate a system with
// the VFX module's own diagnostics, create, read and list effects under the open project's Content/, check that scenes'
// ParticleSystem components name effects that exist, and simulate an effect headless to see what it would do (particle
// counts over time, peak, bounds, a sample of live particles) with no GPU.
//
// Note: .avfx is not an asset type of the asset database (no importer, so no GUID and no cooking), and the player does not
// run ParticleSystem components. These tools therefore work on the files directly, and the effect plays only in the
// editor's VFX tools and anything that hosts a vfx::ParticleWorld. ParticleSystem components are edited with particle_set
// (editor_tools.cpp).

#include "asset_tools.h"
#include "mcp_server.h"

#include <memory>

namespace aether::mcp {

void RegisterVfxTools(McpServer& server, std::shared_ptr<detail::AssetHost> host);

} // namespace aether::mcp
