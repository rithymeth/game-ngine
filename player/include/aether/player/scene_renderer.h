#pragma once

#include "aether/gfx/rhi/device.h"
#include "aether/player/game.h"

#include <memory>

namespace aether::player {

// Draws the current game's cooked ModelRenderer entities through the same
// cross-API RHI used by the renderer demos. The player owns this alongside
// its window/swap chain; headless runs do not construct it.
class SceneRenderer {
public:
    SceneRenderer(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& swap_chain, const GamePackage& package);
    ~SceneRenderer();
    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    void Draw(Game& game, gfx::rhi::ICommandList& commands);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aether::player
