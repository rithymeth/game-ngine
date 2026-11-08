#include "taa_pass.h"

#include "aether/core/log.h"

#include <cstring>

namespace aether::player {

namespace {

constexpr char kTaaShader[] = R"(
struct TaaConstants {
    float4x4 g_Reprojection;
    uint g_Current;
    uint g_History;
    uint g_Depth;
    float g_Blend;
    float2 g_Texel;
    float2 g_Padding;
};
#ifdef __spirv__
[[vk::push_constant]]
#endif
ConstantBuffer<TaaConstants> g_PC : register(b0);
Texture2D g_Textures[128] : register(t0, space0);
SamplerState g_Sampler : register(s0, space1);

float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

int2 TargetSize() { return int2(round(1.0 / g_PC.g_Texel)); }
float3 Current(int2 p) { return g_Textures[g_PC.g_Current].Load(int3(clamp(p, int2(0, 0), TargetSize() - 1), 0)).rgb; }
float Depth(int2 p) { return g_Textures[g_PC.g_Depth].Load(int3(clamp(p, int2(0, 0), TargetSize() - 1), 0)).r; }

float3 ToYCoCg(float3 c) { return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25))); }
float3 FromYCoCg(float3 c) { return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }
float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

float3 History(float2 uv) {
    // A bilinear tap clamped half a texel inside the edges (the sampler wraps).
    return g_Textures[g_PC.g_History].SampleLevel(g_Sampler, clamp(uv, 0.5 * g_PC.g_Texel, 1.0 - 0.5 * g_PC.g_Texel), 0).rgb;
}

// Catmull-Rom in 5 bilinear taps (the corners dropped): sharper than one bilinear tap, which would blur the history a little more every
// frame the camera moves.
float3 HistoryCatmullRom(float2 uv) {
    float2 position = uv / g_PC.g_Texel;
    float2 center = floor(position - 0.5) + 0.5;
    float2 f = position - center;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 t0 = (center - 1.0) * g_PC.g_Texel;
    float2 t3 = (center + 2.0) * g_PC.g_Texel;
    float2 t12 = (center + w2 / w12) * g_PC.g_Texel;
    float3 sum = History(float2(t12.x, t0.y)) * (w12.x * w0.y) + History(float2(t0.x, t12.y)) * (w0.x * w12.y) +
                 History(t12) * (w12.x * w12.y) + History(float2(t3.x, t12.y)) * (w3.x * w12.y) + History(float2(t12.x, t3.y)) * (w12.x * w3.y);
    float weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(sum / weight, 0.0);
}

// Moves `history` toward `center` until it is inside the box center +- extent (all YCoCg).
float3 ClipToBox(float3 history, float3 center, float3 extent) {
    float3 offset = history - center;
    float3 units = abs(offset / max(extent, 1e-5));
    float largest = max(units.x, max(units.y, units.z));
    return largest > 1.0 ? center + offset / largest : history;
}

float4 PSMain(float4 position : SV_Position) : SV_Target {
    int2 p = int2(position.xy);
    float3 current = Current(p);
    if (g_PC.g_Blend >= 1.0) return float4(current, 1.0);

    // The neighbourhood: its colour moments, and its nearest depth (so an edge's pixels follow the surface in front).
    float3 m1 = float3(0, 0, 0), m2 = float3(0, 0, 0);
    float depth = 1.0;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            float3 c = ToYCoCg(Current(p + int2(x, y)));
            m1 += c;
            m2 += c * c;
            depth = min(depth, Depth(p + int2(x, y)));
        }
    }
    float3 mean = m1 / 9.0;
    float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));

    // Where this pixel was last frame.
    float2 ndc = float2(position.x * g_PC.g_Texel.x * 2.0 - 1.0, 1.0 - position.y * g_PC.g_Texel.y * 2.0);
    float4 previous = mul(g_PC.g_Reprojection, float4(ndc, depth, 1.0));
    if (previous.w <= 1e-6) return float4(current, 1.0);
    float2 previous_uv = float2(previous.x / previous.w * 0.5 + 0.5, 0.5 - previous.y / previous.w * 0.5);
    if (any(previous_uv < 0.0) || any(previous_uv > 1.0)) return float4(current, 1.0); // off screen last frame: nothing to keep

    float3 history = HistoryCatmullRom(previous_uv);
    if (any(isnan(history)) || any(isinf(history))) return float4(current, 1.0);
    history = FromYCoCg(ClipToBox(ToYCoCg(history), mean, 1.25 * sigma));

    float current_weight = g_PC.g_Blend / (1.0 + Luma(current));
    float history_weight = (1.0 - g_PC.g_Blend) / (1.0 + Luma(history));
    return float4(max((current * current_weight + history * history_weight) / (current_weight + history_weight), 0.0), 1.0);
}
)";

} // namespace

bool TaaReprojection(const Mat4& previous_view_projection, const Mat4& view_projection, Mat4& out) {
    Mat4 inverse;
    if (!view_projection.TryInverse(inverse)) return false;
    out = previous_view_projection * inverse;
    return true;
}

const char* TaaShaderSource() { return kTaaShader; }

TaaPass::TaaPass(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& swap_chain) : device_(device), swap_chain_(swap_chain) {
    const gfx::rhi::SampledTextureHandle color = swap_chain.SceneColorTexture();
    const gfx::rhi::SampledTextureHandle depth = swap_chain.SceneDepthTexture();
    scene_color_ = color.IsValid() && color.index < gfx::rhi::kMaxBindlessTextures ? color.index : 0u;
    scene_depth_ = depth.IsValid() && depth.index < gfx::rhi::kMaxBindlessTextures ? depth.index : 0u;
    if (scene_color_ == 0 || scene_depth_ == 0) {
        AETHER_LOG_INFO("PlayerRenderer", "Temporal anti-aliasing unavailable: no scene colour target or depth copy on this device");
        return;
    }
    gfx::rhi::PipelineDesc desc;
    desc.hlsl_source = kTaaShader;
    desc.push_constant_size_bytes = sizeof(TaaConstants);
    desc.enable_bindless_textures = true;
    desc.render_texture_target = true;
    pipeline_ = device.CreatePipeline(desc, swap_chain);
    if (!pipeline_.IsValid()) AETHER_LOG_WARN("PlayerRenderer", "Temporal anti-aliasing pipeline unavailable");
}

TaaPass::~TaaPass() {
    Release();
    if (pipeline_.IsValid()) device_.DestroyPipeline(pipeline_);
}

void TaaPass::Release() {
    for (gfx::rhi::SampledTextureHandle& texture : history_) {
        if (texture.IsValid()) device_.DestroyTexture(texture);
        texture = {};
    }
    width_ = height_ = 0;
    history_valid_ = false;
}

bool TaaPass::EnsureHistory(u32 width, u32 height) {
    if (history_[0].IsValid() && history_[1].IsValid() && width_ == width && height_ == height) return true;
    Release();
    for (gfx::rhi::SampledTextureHandle& texture : history_) {
        texture = device_.CreateRenderTexture(width, height);
        if (!texture.IsValid() || texture.index >= gfx::rhi::kMaxBindlessTextures) {
            Release();
            return false;
        }
    }
    width_ = width;
    height_ = height;
    return true;
}

void TaaPass::NextJitter(f32& x, f32& y) {
    const Vec3 jitter = TaaJitter(frame_++, kTaaSequenceLength);
    x = jitter.x;
    y = jitter.y;
}

u32 TaaPass::Resolve(gfx::rhi::ICommandList& commands, const Mat4& view_projection, u64 camera) {
    if (!Valid()) return 0;
    const u32 width = swap_chain_.Width(), height = swap_chain_.Height();
    if (width == 0 || height == 0 || !EnsureHistory(width, height)) return 0;

    TaaConstants constants;
    constants.current = scene_color_;
    constants.depth = scene_depth_;
    constants.history = history_[current_ ^ 1u].index;
    constants.texel[0] = 1.0f / static_cast<f32>(width);
    constants.texel[1] = 1.0f / static_cast<f32>(height);
    Mat4 reprojection;
    const bool keep = history_valid_ && camera == previous_camera_ &&
                      TaaReprojection(previous_view_projection_, view_projection, reprojection);
    constants.blend = keep ? kTaaBlend : 1.0f;
    if (keep) std::memcpy(constants.reprojection, &reprojection.cols[0], sizeof(constants.reprojection));

    // The full-screen triangle writes every texel, so the target needn't be cleared.
    if (!commands.BeginTexturePass(history_[current_], nullptr)) return 0;
    commands.BindPipeline(pipeline_);
    commands.BindBindlessTextures();
    commands.SetPushConstants(&constants, sizeof(constants));
    commands.Draw(3);
    commands.EndRenderPass();

    const u32 resolved = history_[current_].index;
    current_ ^= 1u;
    history_valid_ = true;
    previous_view_projection_ = view_projection;
    previous_camera_ = camera;
    return resolved;
}

} // namespace aether::player
