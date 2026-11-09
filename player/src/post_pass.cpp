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
    uint g_Bloom;
    float g_BloomIntensity;
    float g_BloomScale;
};
#ifdef __spirv__
[[vk::push_constant]]
#endif
ConstantBuffer<PushConstants> g_PC : register(b0);
Texture2D g_Textures[128] : register(t0, space0);
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

// A bilinear tap kept half a texel inside the edges (the shared sampler wraps).
float3 BloomTap(uint index, float2 uv, float2 texel) {
    return g_Textures[index].SampleLevel(g_Sampler, clamp(uv, 0.5 * texel, 1.0 - 0.5 * texel), 0).rgb;
}

float4 PSMain(float4 position : SV_POSITION) : SV_TARGET {
    uint width, height;
    g_Textures[g_PC.g_SceneColor].GetDimensions(width, height);
    float3 c = g_Textures[g_PC.g_SceneColor].Load(int3(int2(position.xy), 0)).rgb;
    if (g_PC.g_Bloom < 128 && g_PC.g_BloomIntensity > 0.0) {
        // The chain's top level, half the screen's size, read with the same 3x3 tent the upsample uses.
        uint bloom_width, bloom_height;
        g_Textures[g_PC.g_Bloom].GetDimensions(bloom_width, bloom_height);
        float2 t = 1.0 / float2(bloom_width, bloom_height);
        float2 uv = position.xy / float2(width, height);
        float3 bloom = BloomTap(g_PC.g_Bloom, uv, t) * 4.0;
        bloom += (BloomTap(g_PC.g_Bloom, uv + float2(-t.x, 0), t) + BloomTap(g_PC.g_Bloom, uv + float2(t.x, 0), t) +
                  BloomTap(g_PC.g_Bloom, uv + float2(0, -t.y), t) + BloomTap(g_PC.g_Bloom, uv + float2(0, t.y), t)) * 2.0;
        bloom += BloomTap(g_PC.g_Bloom, uv + float2(-t.x, -t.y), t) + BloomTap(g_PC.g_Bloom, uv + float2(t.x, -t.y), t) +
                 BloomTap(g_PC.g_Bloom, uv + float2(-t.x, t.y), t) + BloomTap(g_PC.g_Bloom, uv + float2(t.x, t.y), t);
        c = lerp(c, bloom * (g_PC.g_BloomScale / 16.0), g_PC.g_BloomIntensity);
    }
    c *= g_PC.g_Exposure;
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

// The bloom chain's passes, one source for both: the downsample is Jimenez's 13-tap filter (Call of Duty: Advanced Warfare, SIGGRAPH 2014),
// with the Karis average (each 2x2 box weighted by 1 / (1 + luma)) on the first level; the upsample a 3x3 tent added onto the level above.
constexpr char kBloomShader[] = R"(
struct PushConstants {
    uint g_Source;
    uint g_Karis;
    float2 g_SourceTexel;
    float2 g_TargetInverse;
    float g_Radius;
    float g_Padding;
};
#ifdef __spirv__
[[vk::push_constant]]
#endif
ConstantBuffer<PushConstants> g_PC : register(b0);
Texture2D g_Textures[128] : register(t0, space0);
SamplerState g_Sampler : register(s0, space1);

float4 VSMain(uint id : SV_VertexID) : SV_POSITION {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float3 Tap(float2 uv, float2 offset) {
    float2 t = g_PC.g_SourceTexel;
    // Half a texel inside the edges: the shared sampler wraps, and light must not bleed across the screen.
    float3 c = g_Textures[g_PC.g_Source].SampleLevel(g_Sampler, clamp(uv + offset * t, 0.5 * t, 1.0 - 0.5 * t), 0).rgb;
    return min(max(c, 0.0), 65000.0); // nothing negative or infinite enters the chain
}

float KarisWeight(float3 c) { return 1.0 / (1.0 + dot(c, float3(0.2126, 0.7152, 0.0722))); }

float4 PSMain(float4 position : SV_POSITION) : SV_TARGET {
    float2 uv = position.xy * g_PC.g_TargetInverse;
#ifdef AETHER_BLOOM_UPSAMPLE
    float r = g_PC.g_Radius;
    float3 c = Tap(uv, float2(0, 0)) * 4.0;
    c += (Tap(uv, float2(-r, 0)) + Tap(uv, float2(r, 0)) + Tap(uv, float2(0, -r)) + Tap(uv, float2(0, r))) * 2.0;
    c += Tap(uv, float2(-r, -r)) + Tap(uv, float2(r, -r)) + Tap(uv, float2(-r, r)) + Tap(uv, float2(r, r));
    return float4(c / 16.0, 1.0);
#else
    float3 a = Tap(uv, float2(-2, -2)), b = Tap(uv, float2(0, -2)), c = Tap(uv, float2(2, -2));
    float3 d = Tap(uv, float2(-2, 0)), e = Tap(uv, float2(0, 0)), f = Tap(uv, float2(2, 0));
    float3 g = Tap(uv, float2(-2, 2)), h = Tap(uv, float2(0, 2)), i = Tap(uv, float2(2, 2));
    float3 j = Tap(uv, float2(-1, -1)), k = Tap(uv, float2(1, -1)), l = Tap(uv, float2(-1, 1)), m = Tap(uv, float2(1, 1));
    // Five overlapping 2x2 boxes: the centre one weighs 0.5, the four corner ones 0.125 each.
    float3 box0 = (j + k + l + m) * 0.25;
    float3 box1 = (a + b + d + e) * 0.25, box2 = (b + c + e + f) * 0.25, box3 = (d + e + g + h) * 0.25, box4 = (e + f + h + i) * 0.25;
    float w0 = 0.5, w1 = 0.125, w2 = 0.125, w3 = 0.125, w4 = 0.125;
    if (g_PC.g_Karis != 0) {
        w0 *= KarisWeight(box0); w1 *= KarisWeight(box1); w2 *= KarisWeight(box2); w3 *= KarisWeight(box3); w4 *= KarisWeight(box4);
    }
    float3 result = (box0 * w0 + box1 * w1 + box2 * w2 + box3 * w3 + box4 * w4) / (w0 + w1 + w2 + w3 + w4);
    return float4(result, 1.0);
#endif
}
)";

} // namespace

const char* PostShaderSource() { return kPostShader; }

std::string BloomShaderSource(bool upsample) { return std::string(upsample ? "#define AETHER_BLOOM_UPSAMPLE 1\n" : "") + kBloomShader; }

std::vector<Extent> PostBloomChain(u32 width, u32 height) { return BloomChain(width, height, kBloomLevels, 8); }

PostConstants PackPostConstants(const PostProcessSettings& settings, u32 scene_color_index) {
    PostConstants c;
    c.scene_color = scene_color_index;
    c.exposure = std::exp2(std::clamp(settings.exposure_compensation, -10.0f, 10.0f));
    c.tonemapper = static_cast<u32>(settings.tonemapper);
    if (c.tonemapper > 2) c.tonemapper = 0;
    c.saturation = std::clamp(settings.saturation, 0.0f, 2.0f);
    c.vignette = std::clamp(settings.vignette, 0.0f, 1.0f);
    c.bloom_intensity = std::isfinite(settings.bloom_intensity) ? std::clamp(settings.bloom_intensity, 0.0f, 1.0f) : 0.0f;
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

PostPass::PostPass(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& swap_chain) : device_(device), swap_chain_(swap_chain) {
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
    if (!pipeline_.IsValid()) {
        AETHER_LOG_WARN("PlayerRenderer", "Post pipeline unavailable; the scene shader tone maps itself");
        return;
    }
    const std::string down_source = BloomShaderSource(false), up_source = BloomShaderSource(true);
    gfx::rhi::PipelineDesc bloom;
    bloom.push_constant_size_bytes = sizeof(BloomConstants);
    bloom.enable_bindless_textures = true;
    bloom.render_texture_target = true;
    bloom.hlsl_source = down_source.c_str();
    bloom_down_ = device.CreatePipeline(bloom, swap_chain);
    bloom.hlsl_source = up_source.c_str();
    bloom.enable_blending = true;
    bloom.additive_blending = true;
    bloom_up_ = device.CreatePipeline(bloom, swap_chain);
    if (!bloom_down_.IsValid() || !bloom_up_.IsValid()) AETHER_LOG_INFO("PlayerRenderer", "Bloom unavailable on this device");
}

PostPass::~PostPass() {
    ReleaseBloom();
    if (bloom_up_.IsValid()) device_.DestroyPipeline(bloom_up_);
    if (bloom_down_.IsValid()) device_.DestroyPipeline(bloom_down_);
    if (pipeline_.IsValid()) device_.DestroyPipeline(pipeline_);
}

void PostPass::ReleaseBloom() {
    for (const gfx::rhi::SampledTextureHandle level : bloom_levels_) device_.DestroyTexture(level);
    bloom_levels_.clear();
    bloom_extents_.clear();
    bloom_width_ = bloom_height_ = 0;
    bloom_ready_ = false;
}

bool PostPass::EnsureBloomChain(u32 width, u32 height) {
    if (!bloom_levels_.empty() && bloom_width_ == width && bloom_height_ == height) return true;
    ReleaseBloom();
    const std::vector<Extent> chain = PostBloomChain(width, height);
    if (chain.empty()) return false;
    for (const Extent& extent : chain) {
        const gfx::rhi::SampledTextureHandle level = device_.CreateRenderTexture(extent.width, extent.height);
        // The pass binds by index into a 32-slot table; a slot outside it can't be read.
        if (!level.IsValid() || level.index >= gfx::rhi::kMaxBindlessTextures) {
            if (level.IsValid()) device_.DestroyTexture(level);
            ReleaseBloom();
            if (!bloom_warned_) {
                AETHER_LOG_WARN("PlayerRenderer", "No room in the bindless table for the bloom chain (%zu levels); drawing without bloom",
                                chain.size());
                bloom_warned_ = true;
            }
            return false;
        }
        bloom_levels_.push_back(level);
    }
    bloom_extents_ = chain;
    bloom_width_ = width;
    bloom_height_ = height;
    return true;
}

bool PostPass::RenderBloom(gfx::rhi::ICommandList& commands, const PostProcessSettings& settings) {
    bloom_ready_ = false;
    if (!Valid() || !bloom_down_.IsValid() || !bloom_up_.IsValid()) return false;
    if (!(PackPostConstants(settings, Source()).bloom_intensity > 0.0f)) return false;
    const u32 width = swap_chain_.Width(), height = swap_chain_.Height();
    if (width == 0 || height == 0 || !EnsureBloomChain(width, height)) return false;

    const usize levels = bloom_levels_.size();
    for (usize i = 0; i < levels; ++i) {
        BloomConstants constants;
        constants.source = i == 0 ? Source() : bloom_levels_[i - 1].index;
        constants.karis = i == 0 ? 1u : 0u;
        const f32 source_width = static_cast<f32>(i == 0 ? width : bloom_extents_[i - 1].width);
        const f32 source_height = static_cast<f32>(i == 0 ? height : bloom_extents_[i - 1].height);
        constants.source_texel[0] = 1.0f / source_width;
        constants.source_texel[1] = 1.0f / source_height;
        constants.target_inverse[0] = 1.0f / static_cast<f32>(bloom_extents_[i].width);
        constants.target_inverse[1] = 1.0f / static_cast<f32>(bloom_extents_[i].height);
        // The full-screen triangle writes every texel, so the old contents needn't be cleared.
        if (!commands.BeginTexturePass(bloom_levels_[i], nullptr)) return false;
        commands.BindPipeline(bloom_down_);
        commands.BindBindlessTextures();
        commands.SetPushConstants(&constants, sizeof(constants));
        commands.Draw(3);
        commands.EndRenderPass();
    }
    for (usize i = levels - 1; i > 0; --i) {
        BloomConstants constants;
        constants.source = bloom_levels_[i].index;
        constants.source_texel[0] = 1.0f / static_cast<f32>(bloom_extents_[i].width);
        constants.source_texel[1] = 1.0f / static_cast<f32>(bloom_extents_[i].height);
        constants.target_inverse[0] = 1.0f / static_cast<f32>(bloom_extents_[i - 1].width);
        constants.target_inverse[1] = 1.0f / static_cast<f32>(bloom_extents_[i - 1].height);
        constants.radius = 1.0f;
        if (!commands.BeginTexturePass(bloom_levels_[i - 1], nullptr)) return false; // kept: the upsample adds onto it
        commands.BindPipeline(bloom_up_);
        commands.BindBindlessTextures();
        commands.SetPushConstants(&constants, sizeof(constants));
        commands.Draw(3);
        commands.EndRenderPass();
    }
    bloom_ready_ = true;
    return true;
}

void PostPass::Draw(gfx::rhi::ICommandList& commands, const PostProcessSettings& settings) {
    if (!Valid()) return;
    PostConstants constants = PackPostConstants(settings, Source());
    if (bloom_ready_ && !bloom_levels_.empty()) {
        constants.bloom = bloom_levels_.front().index;
        constants.bloom_scale = 1.0f / static_cast<f32>(bloom_levels_.size());
    }
    bloom_ready_ = false;
    commands.BindPipeline(pipeline_);
    commands.BindBindlessTextures();
    commands.SetPushConstants(&constants, sizeof(constants));
    commands.Draw(3);
}

} // namespace aether::player
