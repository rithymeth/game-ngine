#include "decal_pass.h"

#include "aether/core/log.h"

#include <algorithm>
#include <cmath>

namespace aether::player {
namespace {

constexpr char kDecalShader[] = R"(
struct PushConstants {
    float4x4 g_ClipToUnit;
    float4 g_Rect;
    float4 g_Color;
    float4 g_Fade;
    float4 g_Extra;
};
#ifdef __spirv__
[[vk::push_constant]]
#endif
ConstantBuffer<PushConstants> g_PC : register(b0);
Texture2D g_Textures[32] : register(t0, space0);
SamplerState g_Sampler : register(s0, space1);

// The decal's rectangle as two triangles.
float4 VSMain(uint id : SV_VertexID) : SV_POSITION {
    float2 corners[6] = { float2(0.0, 0.0), float2(1.0, 0.0), float2(0.0, 1.0), float2(1.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0) };
    float2 ndc = lerp(g_PC.g_Rect.xy, g_PC.g_Rect.zw, corners[id]);
    return float4(ndc, 0.0, 1.0);
}

float4 PSMain(float4 position : SV_POSITION) : SV_TARGET {
    uint depth_index = (uint)(g_PC.g_Extra.z + 0.5);
    uint texture_index = (uint)(g_PC.g_Extra.y + 0.5);
    uint width, height;
    g_Textures[depth_index].GetDimensions(width, height);
    // The depth buffer's value at this pixel, read with a texel load (not a filtered sample).
    float depth = g_Textures[depth_index].Load(int3(int2(position.xy), 0)).r;
    float2 ndc = float2(position.x / width * 2.0 - 1.0, 1.0 - position.y / height * 2.0);
    float4 unit_point = mul(g_PC.g_ClipToUnit, float4(ndc, depth, 1.0));
    float3 p = unit_point.xyz / unit_point.w;
    // Outside the decal's box (and the sky, at depth 1, which projects far outside it): nothing to paint.
    clip(0.5 - max(abs(p.x), max(abs(p.y), abs(p.z))));
    // The surface's tilt against the projection, in metres so a stretched box doesn't skew it.
    float3 metric = p * float3(g_PC.g_Fade.z, g_PC.g_Fade.w, g_PC.g_Extra.x);
    float3 surface_normal = cross(ddx(metric), ddy(metric));
    float normal_length = length(surface_normal);
    float facing = 1.0;
    if (normal_length > 1e-12) facing = abs(surface_normal.z) / normal_length;
    float angle_fade = saturate((facing - g_PC.g_Fade.y) / max(g_PC.g_Fade.x - g_PC.g_Fade.y, 1e-4));
    // Not a ?: — HLSL evaluates both arms, so the "no texture" index would still be sampled out of bounds.
    float4 texel = float4(1.0, 1.0, 1.0, 1.0);
    if (texture_index < 32) texel = g_Textures[texture_index].Sample(g_Sampler, float2(p.x + 0.5, 0.5 - p.y));
#ifdef AETHER_HDR_TARGET
    // Into the linear HDR target: the texture's colour decoded from sRGB, the tint already linear (the post pass encodes the whole scene).
    texel.rgb = lerp(texel.rgb / 12.92, pow((texel.rgb + 0.055) / 1.055, 2.4), step(0.04045, texel.rgb));
#endif
    float4 color = texel * g_PC.g_Color;
    color.a = color.a * angle_fade;
    return color;
}
)";

} // namespace

const char* DecalShaderSource() { return kDecalShader; }

f32 DecalTint(f32 linear) {
    const f32 c = std::clamp(linear, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

DecalConstants PackDecalConstants(const DecalPassItem& item, u32 scene_depth_index) {
    DecalConstants c{};
    c.clip_to_unit = item.draw.clip_to_unit;
    for (int i = 0; i < 4; ++i) {
        c.rect[i] = item.draw.rect[i];
        c.color[i] = item.color[i];
    }
    c.fade[0] = item.fade_start, c.fade[1] = item.fade_end;
    c.fade[2] = item.draw.size.x, c.fade[3] = item.draw.size.y;
    c.extra[0] = item.draw.size.z;
    c.extra[1] = static_cast<f32>(item.texture);
    c.extra[2] = static_cast<f32>(scene_depth_index);
    return c;
}

DecalPass::~DecalPass() {
    if (pipeline_.IsValid()) device_.DestroyPipeline(pipeline_);
}

DecalPass::DecalPass(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& swap_chain, bool hdr_target)
    : device_(device), hdr_target_(hdr_target) {
    const gfx::rhi::SampledTextureHandle depth = swap_chain.SceneDepthTexture();
    scene_depth_ = depth.IsValid() && depth.index < gfx::rhi::kMaxBindlessTextures ? depth.index : 0u;
    if (scene_depth_ == 0) {
        AETHER_LOG_INFO("PlayerRenderer", "No scene depth texture on this device; decals are not drawn");
        return;
    }
    gfx::rhi::PipelineDesc desc;
    desc.hlsl_source = std::string(hdr_target ? "#define AETHER_HDR_TARGET 1\n" : "") + kDecalShader;
    desc.scene_color_target = hdr_target;
    desc.push_constant_size_bytes = sizeof(DecalConstants);
    desc.enable_bindless_textures = true;
    desc.enable_blending = true; // alpha: the decal over the lit surface
    pipeline_ = device.CreatePipeline(desc, swap_chain);
    if (!pipeline_.IsValid()) AETHER_LOG_WARN("PlayerRenderer", "Decal pipeline unavailable; decals are not drawn");
}

u32 DecalPass::Draw(gfx::rhi::ICommandList& commands, const std::vector<DecalPassItem>& items) {
    if (!Valid() || items.empty()) return 0;
    if (!commands.CopySceneDepth()) return 0;
    commands.BindPipeline(pipeline_);
    commands.BindBindlessTextures();
    for (const DecalPassItem& item : items) {
        const DecalConstants constants = PackDecalConstants(item, scene_depth_);
        commands.SetPushConstants(&constants, sizeof(constants));
        commands.Draw(6);
    }
    return static_cast<u32>(items.size());
}

} // namespace aether::player
