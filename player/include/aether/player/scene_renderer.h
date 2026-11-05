#pragma once

#include "aether/gfx/rhi/device.h"
#include "aether/player/game.h"

#include <functional>
#include <memory>
#include <string>

namespace aether::player {

// The HLSL the scene pass compiles (entry points VSMain and PSMain), as SceneRenderer builds it: the advanced PBR library ahead of the scene
// shader, and with the frame-constants block when `with_frame_data` (what devices that support frame constants get). Here so a test can
// compile the real source with glslang.
std::string SceneShaderSource(bool with_frame_data);

// Draws the current game's cooked ModelRenderer entities through the same
// cross-API RHI used by the renderer demos. The player owns this alongside
// its window/swap chain; headless runs do not construct it.
class SceneRenderer {
public:
    SceneRenderer(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& swap_chain, const GamePackage& package);
    ~SceneRenderer();
    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    // frame_index selects the fence-protected per-frame buffers for animated meshes.
    // A quality preset's view distance in metres: the camera's far plane is pulled in to it (0, the default, leaves cameras as authored).
    void SetViewDistance(f32 metres);
    // Whether the particle pass draws the game's particle effects (Game::Particles()); on by default.
    void SetDrawParticles(bool draw);
    // Optional visual-only transform sampler. Never mutates Game's World or PhysicsScene.
    // Leave unset for the original offline/single-player rendering path.
    void SetRenderPoseSampler(std::function<bool(Entity, Transform&)> sampler);
    void Draw(Game& game, gfx::rhi::ICommandList& commands, u32 frame_index);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aether::player
