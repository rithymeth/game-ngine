#include "sky_pass.h"

#include "aether/core/log.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

namespace aether::player {

namespace {

constexpr char kSkyShaderHead[] = R"(
struct SkyConstants {
    float4 right;
    float4 up;
    float4 forward;
};
#ifdef __spirv__
[[vk::push_constant]]
#endif
ConstantBuffer<SkyConstants> g_PC : register(b0);
Texture2D g_Textures[128] : register(t0, space0);
SamplerState g_Sampler : register(s0, space1);
)";

constexpr char kSkyShaderBody[] = R"(
struct PSInput {
    float4 position : SV_POSITION;
    float2 ndc : TEXCOORD0;
};

PSInput VSMain(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    PSInput output;
    output.ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    output.position = float4(output.ndc, 0.0, 1.0);
    return output;
}

// Clouds: a flat deck seen from below, in value-noise fBm. The ray is projected onto the deck (its x and z over how far it climbs, so the clouds close up toward
// the horizon), the deck drifts with the world's wind, and a second tap of the noise a step toward the sun says which side of a cloud the sun is on: the near
// side is lit and the far side is shaded. They fade out toward the horizon, where the projection runs away.
float CloudHash(float2 p) {
    p = frac(p * float2(0.1031, 0.1030));
    p += dot(p, p.yx + 33.33);
    return frac((p.x + p.y) * p.x);
}
float CloudNoise(float2 x) {
    float2 i = floor(x);
    float2 f = frac(x);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(CloudHash(i), CloudHash(i + float2(1, 0)), f.x), lerp(CloudHash(i + float2(0, 1)), CloudHash(i + float2(1, 1)), f.x), f.y);
}
float CloudFbm(float2 p) {
    float amplitude = 0.5, sum = 0.0;
    [unroll] for (int i = 0; i < 5; ++i) {
        sum += amplitude * CloudNoise(p);
        p = p * 2.03 + float2(17.1, 9.2);
        amplitude *= 0.5;
    }
    return sum;
}

// The environment's level 0 in the view ray's direction, tinted like the lighting, with the sun's disc (half a degree across, a soft
// edge) where a DirectionalLight is: its colour times intensity spread over the disc, capped inside half-float range.
float4 PSMain(PSInput input) : SV_TARGET {
    float3 direction = normalize(g_PC.right.xyz * input.ndc.x + g_PC.up.xyz * input.ndc.y + g_PC.forward.xyz);
    float3 color = EnvironmentRadiance(direction, 0.0) * g_Frame.sky_color.rgb;
    float clear = 1.0; // how much of the sun's disc shows through the clouds
    if (g_Frame.clouds.x > 0.0 && direction.y > 0.0) {
        float coverage = g_Frame.clouds.x;
        float2 plane = direction.xz / max(direction.y, 0.03) * g_Frame.clouds.z;
        float2 drift = g_Frame.wind.xy * (g_Frame.time.x * g_Frame.clouds.y);
        float2 q = plane + drift;
        float n = CloudFbm(q);
        float density = smoothstep(1.0 - coverage - 0.12, 1.0 - coverage + 0.18, n);
        float3 sun_toward = normalize(g_Frame.sun_direction.xyz);
        float toward = CloudFbm(q + sun_toward.xz * 0.14);
        float lit = g_Frame.sun_direction.w > 0.5 ? saturate(0.5 + (n - toward) * 3.5) : 0.0;
        float3 ambient = EnvironmentRadiance(float3(0, 1, 0), 0.0) * g_Frame.sky_color.rgb;
        float3 cloud = ambient * 0.8 + g_Frame.sun_color.rgb * (0.3 * lit);
        float fade = smoothstep(0.0, 0.2, direction.y);
        float amount = density * fade;
        color = lerp(color, cloud, amount);
        clear = 1.0 - amount;
    }
    if (g_Frame.sun_direction.w > 0.5) {
        float cosine = dot(direction, normalize(g_Frame.sun_direction.xyz));
        float disc = smoothstep(0.999962, 0.999990, cosine); // 0.5 to 0.26 degrees from its centre
        color += min(g_Frame.sun_color.rgb * 4000.0, 20000.0) * disc * clear;
    }
    return float4(color, 1.0);
}
)";

} // namespace

const char* SkyPass::ShaderSource() {
    static const std::string source = std::string(kSkyShaderHead) + FrameDataHlsl() + kSkyShaderBody;
    return source.c_str();
}

SkyConstants SkyPass::MakeConstants(const View& view) {
    SkyConstants c;
    const Vec3 right = view.Right(), up = view.Up(), forward = view.forward;
    const f32 tan_y = view.perspective ? std::tan(view.fov_degrees * 0.5f * 3.14159265f / 180.0f) : 0.0f;
    const f32 tan_x = tan_y * view.aspect;
    c.right[0] = right.x * tan_x, c.right[1] = right.y * tan_x, c.right[2] = right.z * tan_x;
    c.up[0] = up.x * tan_y, c.up[1] = up.y * tan_y, c.up[2] = up.z * tan_y;
    c.forward[0] = forward.x, c.forward[1] = forward.y, c.forward[2] = forward.z;
    return c;
}

SkyConstants SkyPass::MakeMirroredConstants(const View& view) {
    SkyConstants c = MakeConstants(view);
    c.right[1] = -c.right[1];
    c.up[1] = -c.up[1];
    c.forward[1] = -c.forward[1];
    return c;
}

SkyPass::SkyPass(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& swap_chain, bool hdr_target) : device_(device), swap_chain_(swap_chain), hdr_target_(hdr_target) {
    if (!device.SupportsFrameConstants()) return;
    // Probe for half-float textures with a 1x1 one.
    const u16 probe_texel[4] = {0, 0, 0, 0x3C00};
    const gfx::rhi::SampledTextureHandle probe = device.CreateHalfFloatTexture(1, 1, 1, probe_texel);
    if (!probe.IsValid()) {
        AETHER_LOG_INFO("PlayerRenderer", "No half-float textures on this device; the sky keeps its flat ambient light");
        return;
    }
    device.DestroyTexture(probe);
    supported_ = true;
    if (!hdr_target) return; // the background is drawn into the HDR scene colour target only
    gfx::rhi::PipelineDesc desc;
    desc.hlsl_source = ShaderSource();
    desc.push_constant_size_bytes = sizeof(SkyConstants);
    desc.frame_constants_size_bytes = sizeof(FrameData);
    desc.enable_bindless_textures = true;
    desc.scene_color_target = true;
    desc.depth_test = false;
    desc.depth_write = false;
    pipeline_ = device.CreatePipeline(desc, swap_chain);
    if (!pipeline_.IsValid()) AETHER_LOG_WARN("PlayerRenderer", "Sky pipeline unavailable; the background stays a flat colour");
}

SkyPass::~SkyPass() {
    Release();
    if (pipeline_.IsValid()) device_.DestroyPipeline(pipeline_);
    if (mirror_pipeline_.IsValid()) device_.DestroyPipeline(mirror_pipeline_);
}

void SkyPass::Release() {
    if (texture_.IsValid()) device_.DestroyTexture(texture_);
    texture_ = {};
    environment_ = {};
}

bool SkyPass::SetEnvironment(const std::string& key, const EnvironmentImage& image, const PrefilterSettings& settings) {
    Release();
    key_ = key;
    if (!supported_) return false;
    const auto start = std::chrono::steady_clock::now();
    return Upload(PrefilterEnvironment(image.width == 0 ? MakeProceduralSky() : image, settings), start);
}

bool SkyPass::SetPrefilteredEnvironment(const std::string& key, PrefilteredEnvironment&& environment) {
    Release();
    key_ = key;
    if (!supported_) return false;
    return Upload(std::move(environment), std::chrono::steady_clock::now());
}

bool SkyPass::Upload(PrefilteredEnvironment&& environment, std::chrono::steady_clock::time_point start) {
    environment_ = std::move(environment);
    if (environment_.mip_count > 0) {
        texture_ = device_.CreateHalfFloatTexture(environment_.width, environment_.height, environment_.mip_count, environment_.halves.data());
    }
    build_milliseconds_ = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (!texture_.IsValid() || texture_.index >= gfx::rhi::kMaxBindlessTextures) {
        if (texture_.IsValid()) device_.DestroyTexture(texture_);
        texture_ = {};
        AETHER_LOG_WARN("PlayerRenderer", "No room for the sky's environment texture; the sky keeps its flat ambient light");
        return false;
    }
    environment_.halves.clear(); // on the GPU now
    environment_.halves.shrink_to_fit();
    return true;
}

void SkyPass::Fill(FrameData& data) const {
    if (!texture_.IsValid()) return;
    std::memcpy(data.sky_sh, environment_.irradiance_sh, sizeof(data.sky_sh));
    data.environment_info[0] = texture_.index;
    data.environment_info[1] = environment_.mip_count;
    data.environment_info[2] = environment_.height;
    data.environment_info[3] = 0;
}

void SkyPass::Draw(gfx::rhi::ICommandList& commands, const View& view, gfx::rhi::BufferHandle frame_constants) {
    if (!pipeline_.IsValid() || !texture_.IsValid() || !frame_constants.IsValid()) return;
    const SkyConstants constants = MakeConstants(view);
    commands.BindPipeline(pipeline_);
    commands.BindFrameConstants(frame_constants);
    commands.BindBindlessTextures();
    commands.SetPushConstants(&constants, sizeof(constants));
    commands.Draw(3);
}

bool SkyPass::DrawMirrored(gfx::rhi::ICommandList& commands, const View& view, gfx::rhi::BufferHandle frame_constants) {
    if (!supported_ || !hdr_target_ || !texture_.IsValid() || !frame_constants.IsValid() || mirror_failed_) return false;
    if (!mirror_pipeline_.IsValid()) {
        gfx::rhi::PipelineDesc desc;
        desc.hlsl_source = ShaderSource();
        desc.push_constant_size_bytes = sizeof(SkyConstants);
        desc.frame_constants_size_bytes = sizeof(FrameData);
        desc.enable_bindless_textures = true;
        desc.render_texture_target = true;
        desc.render_texture_depth = true;
        desc.depth_test = false;
        desc.depth_write = false;
        try {
            mirror_pipeline_ = device_.CreatePipeline(desc, swap_chain_);
        } catch (const std::exception&) {
        }
        if (!mirror_pipeline_.IsValid()) {
            mirror_failed_ = true;
            return false;
        }
    }
    const SkyConstants constants = MakeMirroredConstants(view);
    commands.BindPipeline(mirror_pipeline_);
    commands.BindFrameConstants(frame_constants);
    commands.BindBindlessTextures();
    commands.SetPushConstants(&constants, sizeof(constants));
    commands.Draw(3);
    return true;
}

} // namespace aether::player
