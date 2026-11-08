#pragma once

#include "environment.h"
#include "frame_lights.h"

#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"
#include "aether/renderer/view.h"

#include <chrono>
#include <string>

// The sky (render roadmap step 3): a SkyLight's environment as image-based lighting and as the background. SetEnvironment prefilters
// the image (player/src/environment.h) into an RGBA16F mip chain in the bindless table and keeps its irradiance SH; Fill puts both in
// the frame data, where the scene shader's SkyLighting reads them; Draw paints level 0 behind the scene, with the sun's disc.
// Needs frame constants and half-float textures (D3D12 today); elsewhere it is not Valid and the scene keeps its flat ambient term.

namespace aether::player {

// The sky pipeline's push constants: the camera's basis, scaled so that right * x + up * y + forward is the view ray through NDC (x, y).
struct SkyConstants {
    f32 right[4]{};   // xyz: the camera's right axis times tan(horizontal half-angle)
    f32 up[4]{};      // xyz: its up axis times tan(vertical half-angle)
    f32 forward[4]{}; // xyz: the way it looks
};

class SkyPass {
public:
    SkyPass(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& swap_chain, bool hdr_target);
    ~SkyPass();
    SkyPass(const SkyPass&) = delete;
    SkyPass& operator=(const SkyPass&) = delete;

    // Whether the device can hold an environment (half-float textures). The background additionally needs the HDR scene target.
    bool Valid() const { return supported_; }
    bool CanDrawBackground() const { return pipeline_.IsValid(); }

    // Replaces the environment with `image` (the procedural sky when it is empty), remembered under `key`: what the caller compares to
    // decide whether to rebuild (Key()). False when no texture could be made; the previous one is released either way.
    bool SetEnvironment(const std::string& key, const EnvironmentImage& image, const PrefilterSettings& settings = {});
    // The same from an environment already prefiltered (a cooked .aenv, assets/environment.h): only the upload is left to do.
    bool SetPrefilteredEnvironment(const std::string& key, PrefilteredEnvironment&& environment);
    const std::string& Key() const { return key_; }
    bool HasEnvironment() const { return texture_.IsValid(); }
    gfx::rhi::SampledTextureHandle Texture() const { return texture_; }
    const PrefilteredEnvironment& Environment() const { return environment_; }
    f64 LastBuildMilliseconds() const { return build_milliseconds_; }

    // The environment's part of the frame data (sky_sh, environment_info); leaves it "none" without an environment.
    void Fill(FrameData& data) const;
    // The background, inside the scene pass before the opaque draws: no depth test or write. The pipeline, frame constants and texture
    // table are left bound to the sky's; the caller binds its own next.
    void Draw(gfx::rhi::ICommandList& commands, const View& view, gfx::rhi::BufferHandle frame_constants);

    static SkyConstants MakeConstants(const View& view);
    static const char* ShaderSource();

private:
    void Release();
    bool Upload(PrefilteredEnvironment&& environment, std::chrono::steady_clock::time_point start);

    gfx::rhi::IDevice& device_;
    bool supported_ = false;
    gfx::rhi::PipelineHandle pipeline_;
    gfx::rhi::SampledTextureHandle texture_;
    PrefilteredEnvironment environment_;
    std::string key_;
    f64 build_milliseconds_ = 0.0;
};

} // namespace aether::player
