#pragma once

#include "aether/gfx/rhi/device.h"
#include "aether/player/game.h"

#include <functional>
#include <memory>
#include <string>

namespace aether::player {

// The HLSL the scene pass compiles (entry points VSMain and PSMain), as SceneRenderer builds it: the advanced PBR library ahead of the scene
// shader, and with the frame-constants block when `with_frame_data` (what devices that support frame constants get). Here so a test can
// compile the real source with glslang. With `hdr_target` the pixel shader writes linear light for the HDR scene colour target (the
// post pass tone maps it) instead of tone mapping and encoding for the back buffer itself.
std::string SceneShaderSource(bool with_frame_data, bool hdr_target = false);

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
    // Whether the post pass draws bloom (a quality preset's `bloom`); on by default. Only where there is an HDR scene colour target: the
    // fallback path that tone maps in the scene shader has no bloom.
    void SetBloom(bool enabled);
    // Optional visual-only transform sampler. Never mutates Game's World or PhysicsScene.
    // Leave unset for the original offline/single-player rendering path.
    void SetRenderPoseSampler(std::function<bool(Entity, Transform&)> sampler);
    void Draw(Game& game, gfx::rhi::ICommandList& commands, u32 frame_index);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aether::player
