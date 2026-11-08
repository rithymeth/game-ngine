#include "post_pass.h"

#include "aether/core/log.h"
#include "aether/renderer/post.h"

#include <algorithm>
#include <cmath>

namespace aether::player {

namespace {

// The order matches PostReference exactly: exposure, saturation (in linear light, before the curve compresses it), the tone mapper, the
// vignette, then the sRGB encoding. The tone mappers are the fits renderer/post.cpp holds as CPU references.
constexpr char kPostShader[] = R"(
struct PushConstants {
    uint g_SceneColor;
    float g_Exposure;
    uint g_Tonemapper;
    float g_Saturation;
    float g_Vignette;
    float3 g_Padding;
};
#ifdef __spirv__
[[vk::push_constant]]
#endif
ConstantBuffer<PushConstants> g_PC : register(b0);
Texture2D g_Textures[32] : register(t0, space0);
SamplerState g_Sampler : register(s0, space1);

// One triangle that covers the screen.
float4 VSMain(uint id : SV_VertexID) : SV_POSITION {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float AcesFit(float v) {
    float a = v * (v + 0.0245786) - 0.000090537;
    float b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}
float3 TonemapAces(float3 c) {
    // Rows of the input and output matrices (Stephen Hill's fit of the ACES RRT + ODT).
    c = max(c, 0.0);
    c = float3(dot(float3(0.59719, 0.35458, 0.04823), c), dot(float3(0.07600, 0.90834, 0.01566), c),
               dot(float3(0.02840, 0.13383, 0.83777), c));
    c = float3(AcesFit(c.x), AcesFit(c.y), AcesFit(c.z));
    c = float3(dot(float3(1.60475, -0.53108, -0.07367), c), dot(float3(-0.10208, 1.10813, -0.00605), c),
               dot(float3(-0.00327, -0.07276, 1.07602), c));
    return saturate(c);
}
float AgxCurve(float x) {
    float x2 = x * x;
    float x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}
float AgxEncode(float v) {
    const float min_ev = -12.47393;
    const float max_ev = 4.026069;
    return (clamp(log2(max(v, 1e-10)), min_ev, max_ev) - min_ev) / (max_ev - min_ev);
}
float3 TonemapAgx(float3 c) {
    c = max(c, 1e-10);
    c = float3(dot(float3(0.842479062253094, 0.0784335999999992, 0.0792237451477643), c),
               dot(float3(0.0423282422610123, 0.878468636469772, 0.0791661274605434), c),
               dot(float3(0.0423756549057051, 0.0784336, 0.879142973793104), c));
    c = float3(AgxCurve(AgxEncode(c.x)), AgxCurve(AgxEncode(c.y)), AgxCurve(AgxEncode(c.z)));
    c = float3(dot(float3(1.19687900512017, -0.0980208811401368, -0.0990297440797205), c),
               dot(float3(-0.0528968517574562, 1.15190312990417, -0.0989611768448433), c),
               dot(float3(-0.0529716355144438, -0.0980434501171241, 1.15107367264116), c));
    return pow(saturate(c), 2.2);
}
float3 LinearToSrgb(float3 c) {
    c = saturate(c);
    return lerp(c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055, step(0.0031308, c));
}

float4 PSMain(float4 position : SV_POSITION) : SV_TARGET {
    uint width, height;
    g_Textures[g_PC.g_SceneColor].GetDimensions(width, height);
    float3 c = g_Textures[g_PC.g_SceneColor].Load(int3(int2(position.xy), 0)).rgb * g_PC.g_Exposure;
    float luminance = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = max(luminance + (c - luminance) * g_PC.g_Saturation, 0.0);
    if (g_PC.g_Tonemapper == 0) c = TonemapAces(c);
    else if (g_PC.g_Tonemapper == 1) c = TonemapAgx(c);
    else c = saturate(c);
    float2 d = position.xy / float2(width, height) - 0.5;
    c *= 1.0 - saturate(g_PC.g_Vignette) * saturate(dot(d, d) / 0.5);
    return float4(LinearToSrgb(c), 1.0);
}
)";

} // namespace

const char* PostShaderSource() { return kPostShader; }

PostConstants PackPostConstants(const PostProcessSettings& settings, u32 scene_color_index) {
    PostConstants c;
    c.scene_color = scene_color_index;
    c.exposure = std::exp2(std::clamp(settings.exposure_compensation, -10.0f, 10.0f));
    c.tonemapper = static_cast<u32>(settings.tonemapper);
    if (c.tonemapper > 2) c.tonemapper = 0;
    c.saturation = std::clamp(settings.saturation, 0.0f, 2.0f);
    c.vignette = std::clamp(settings.vignette, 0.0f, 1.0f);
    return c;
}

Vec3 PostReference(const Vec3& linear, const PostConstants& constants, f32 u, f32 v) {
    Vec3 c = linear * constants.exposure;
    c = AdjustSaturation(c, constants.saturation);
    c = Vec3(std::max(c.x, 0.0f), std::max(c.y, 0.0f), std::max(c.z, 0.0f));
    c = Tonemap(c, constants.tonemapper == 1 ? Tonemapper::AgX : constants.tonemapper == 2 ? Tonemapper::None : Tonemapper::ACES);
    c = c * VignetteFactor(u, v, constants.vignette);
    return Vec3(LinearToSrgb(c.x), LinearToSrgb(c.y), LinearToSrgb(c.z));
}

PostPass::PostPass(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& swap_chain) : device_(device) {
    const gfx::rhi::SampledTextureHandle color = swap_chain.SceneColorTexture();
    scene_color_ = color.IsValid() && color.index < gfx::rhi::kMaxBindlessTextures ? color.index : 0u;
    if (scene_color_ == 0) {
        AETHER_LOG_INFO("PlayerRenderer", "No HDR scene colour target on this device; the scene shader tone maps itself");
        return;
    }
    gfx::rhi::PipelineDesc desc;
    desc.hlsl_source = kPostShader;
    desc.push_constant_size_bytes = sizeof(PostConstants);
    desc.enable_bindless_textures = true;
    pipeline_ = device.CreatePipeline(desc, swap_chain);
    if (!pipeline_.IsValid()) AETHER_LOG_WARN("PlayerRenderer", "Post pipeline unavailable; the scene shader tone maps itself");
}

PostPass::~PostPass() {
    if (pipeline_.IsValid()) device_.DestroyPipeline(pipeline_);
}

void PostPass::Draw(gfx::rhi::ICommandList& commands, const PostProcessSettings& settings) {
    if (!Valid()) return;
    const PostConstants constants = PackPostConstants(settings, scene_color_);
    commands.BindPipeline(pipeline_);
    commands.BindBindlessTextures();
    commands.SetPushConstants(&constants, sizeof(constants));
    commands.Draw(3);
}

} // namespace aether::player
