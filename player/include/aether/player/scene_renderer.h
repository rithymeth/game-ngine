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
// With `graph_hlsl` (a drawable material graph's include, graph_material.h) the surface's base colour, metallic, roughness, occlusion and emissive come from
// the graph's EvaluateMaterial instead of the model's own factors and textures.
std::string SceneShaderSource(bool with_frame_data, bool hdr_target = false, bool ray_traced_shadows = false, const std::string& graph_hlsl = {});

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
    // Sun shadows: texels per cascade side and cascades (a quality preset's shadow_resolution and shadow_cascades; 0 turns them off).
    // On by default at 2048 and 4, from a DirectionalLight with cast_shadows, where the device can draw them (D3D12 today).
    void SetShadows(u32 resolution, u32 cascades);
    // Optional visual-only transform sampler. Never mutates Game's World or PhysicsScene.
    // Leave unset for the original offline/single-player rendering path.
    void SetRenderPoseSampler(std::function<bool(Entity, Transform&)> sampler);
    void Draw(Game& game, gfx::rhi::ICommandList& commands, u32 frame_index);

    // The sun's virtual shadow maps (pages of a fine shadow near the camera over the cascades) as of the last Draw, for tests and the profiler.
    struct VirtualShadowStats {
        bool enabled = false;   // the device and the settings allow them
        u32 requests = 0;       // pages the feedback of a few frames ago asked for
        u32 resident = 0;       // pages holding a physical page
        u32 pending = 0;        // of those, waiting to be drawn
        u32 drawn_pages = 0;    // pages drawn this frame
        u32 caster_draws = 0;   // caster draws over those pages
        u32 suppressed = 0;     // resident pages left out of the page table because a deforming caster is over them (the cascades shadow them)
        f64 cpu_ms = 0.0;       // the CPU's time in this frame's page work (feedback, allocation, the page table, recording the pages)
    };
    VirtualShadowStats VirtualShadows() const;
    // How long the GPU spent on the newest frame whose submission has finished (a few frames old when the caller does not wait), by stage, in
    // milliseconds: `shadows_ms` before the scene pass (cascades, virtual pages, ray tracing set-up), `scene_ms` the scene pass itself with its
    // depth copy, `post_ms` what follows (feedback, TAA, bloom, tone mapping, HUD), `frame_ms` their sum. Not valid where the device has no GPU
    // timing (IDevice::SupportsGpuTiming).
    struct GpuTimings {
        bool valid = false;
        f32 frame_ms = 0.0f;
        f32 shadows_ms = 0.0f;
        f32 scene_ms = 0.0f;
        f32 post_ms = 0.0f;
    };
    GpuTimings GpuFrameTimings() const;
    // Ray-traced sun shadows (AETHER_RENDER_RT_SHADOWS=1, needs ray tracing): whether they are on, how many rigid casters the last frame's
    // acceleration structure placed, and how many times that structure has been rebuilt (it is rebuilt only when a caster is new, gone or moved).
    struct RayTracedShadowStats {
        bool enabled = false;
        u32 instances = 0;
        u32 rebuilds = 0;
    };
    RayTracedShadowStats RayTracedShadows() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aether::player
