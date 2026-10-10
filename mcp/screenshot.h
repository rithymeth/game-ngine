#pragma once

// Renders the hosted game's current frame to a PNG, off screen: a hidden window,
// a graphics device and swap chain, and the player's SceneRenderer, made on the
// first capture and kept for the next ones (a different size remakes them).
//
// Needs a graphics device (Direct3D 12 or Vulkan); where there is none,
// Capture reports why instead of throwing.

#include "aether/core/base.h"
#include "aether/player/game.h"

#include <memory>
#include <string>
#include <vector>

namespace aether::mcp {

class GameScreenshotter {
public:
    GameScreenshotter();
    ~GameScreenshotter();
    GameScreenshotter(const GameScreenshotter&) = delete;
    GameScreenshotter& operator=(const GameScreenshotter&) = delete;

    struct Result {
        bool ok = false;
        std::string error;
        u32 width = 0;
        u32 height = 0;
        std::vector<u8> png;
    };

    // `backend`: "d3d12" or "vulkan" (empty: d3d12 on Windows, vulkan elsewhere). A backend change remakes the device.
    Result Capture(player::Game& game, const player::GamePackage& package, u32 width, u32 height, const std::string& backend);
    // Releases the renderer and the device (the game it draws is going away).
    void Reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aether::mcp
