#include "taa_pass.h"

#include "aether/core/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

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
    uint g_Motion;
    float g_Padding;
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
    int2 nearest = int2(0, 0);
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            float3 c = ToYCoCg(Current(p + int2(x, y)));
            m1 += c;
            m2 += c * c;
            float d = Depth(p + int2(x, y));
            if (d < depth) {
                depth = d;
                nearest = int2(x, y);
            }
        }
    }
    float3 mean = m1 / 9.0;
    float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));

    // Where this pixel was last frame: the motion texture's offset where an object moved on its own (read at the nearest neighbour,
    // like the depth), else the camera's reprojection of the depth.
    float4 motion = float4(0, 0, 0, 0);
    if (g_PC.g_Motion != 0) {
        motion = g_Textures[g_PC.g_Motion].Load(int3(clamp(p + nearest, int2(0, 0), TargetSize() - 1), 0));
    }
    float2 previous_uv;
    if (motion.a > 0.5) {
        previous_uv = position.xy * g_PC.g_Texel + motion.xy;
    } else {
        float2 ndc = float2(position.x * g_PC.g_Texel.x * 2.0 - 1.0, 1.0 - position.y * g_PC.g_Texel.y * 2.0);
        float4 previous = mul(g_PC.g_Reprojection, float4(ndc, depth, 1.0));
        if (previous.w <= 1e-6) return float4(current, 1.0);
        previous_uv = float2(previous.x / previous.w * 0.5 + 0.5, 0.5 - previous.y / previous.w * 0.5);
    }
    if (any(previous_uv < 0.0) || any(previous_uv > 1.0)) return float4(current, 1.0); // off screen last frame: nothing to keep

    float3 history = HistoryCatmullRom(previous_uv);
    if (any(isnan(history)) || any(isinf(history))) return float4(current, 1.0);
    history = FromYCoCg(ClipToBox(ToYCoCg(history), mean, 1.25 * sigma));

    float current_weight = g_PC.g_Blend / (1.0 + Luma(current));
    float history_weight = (1.0 - g_PC.g_Blend) / (1.0 + Luma(history));
    return float4(max((current * current_weight + history * history_weight) / (current_weight + history_weight), 0.0), 1.0);
}
)";

// The motion pass: each mover rasterised with this frame's jitter (so it covers the same pixels as in the scene), keeping only the pixels
// where it is the surface in the scene depth copy, and writing the UV offset from this frame's position to last frame's (both without
// jitter). Alpha 1 marks the pixels it wrote; the target is cleared to 0.
constexpr char kTaaMotionShader[] = R"(
struct MotionConstants {
    float4x4 g_Mvp;
    float4x4 g_PreviousMvp;
    float2 g_Jitter;
    uint g_Depth;
    uint g_PreviousPositions;
};
#ifdef __spirv__
[[vk::push_constant]]
#endif
ConstantBuffer<MotionConstants> g_PC : register(b0);
Texture2D g_Textures[128] : register(t0, space0);
SamplerState g_Sampler : register(s0, space1);
#ifdef AETHER_PREVIOUS_POSITIONS
ByteAddressBuffer g_FrameLights : register(t0, space3); // last frame's skinned positions, three floats a vertex
#endif

struct VSInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 tangent : TANGENT;
};
struct PSInput {
    float4 position : SV_POSITION;
    float3 current : TEXCOORD0;
    float3 previous : TEXCOORD1;
};

PSInput VSMain(VSInput input, uint vertex_id : SV_VertexID) {
    float3 previous_position = input.position;
#ifdef AETHER_PREVIOUS_POSITIONS
    if (g_PC.g_PreviousPositions != 0xffffffffu) {
        previous_position = asfloat(g_FrameLights.Load3(g_PC.g_PreviousPositions + vertex_id * 12));
    }
#endif
    float4 clip = mul(g_PC.g_Mvp, float4(input.position, 1.0));
    float4 previous = mul(g_PC.g_PreviousMvp, float4(previous_position, 1.0));
    PSInput output;
    output.position = float4(clip.xy + g_PC.g_Jitter * clip.w, clip.zw);
    output.current = clip.xyw;
    output.previous = previous.xyw;
    return output;
}

float4 PSMain(PSInput input) : SV_Target {
    float stored = g_Textures[g_PC.g_Depth].Load(int3(int2(input.position.xy), 0)).r;
    if (input.position.z > stored + 1e-5) discard; // behind the visible surface
    if (input.previous.z <= 1e-6) return float4(-4.0, -4.0, 0.0, 1.0); // behind last frame's camera: off screen, nothing to keep
    float2 current_uv = float2(input.current.x / input.current.z * 0.5 + 0.5, 0.5 - input.current.y / input.current.z * 0.5);
    float2 previous_uv = float2(input.previous.x / input.previous.z * 0.5 + 0.5, 0.5 - input.previous.y / input.previous.z * 0.5);
    return float4(previous_uv - current_uv, 0.0, 1.0);
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
const char* TaaMotionShaderSource() { return kTaaMotionShader; }

bool TaaTransformMoved(const Mat4& previous, const Mat4& current) {
    f32 largest = 0.0f, difference = 0.0f;
    for (int c = 0; c < 4; ++c) {
        const Vec4& a = previous.cols[c];
        const Vec4& b = current.cols[c];
        largest = std::max({largest, std::fabs(b.x), std::fabs(b.y), std::fabs(b.z), std::fabs(b.w)});
        difference = std::max({difference, std::fabs(a.x - b.x), std::fabs(a.y - b.y), std::fabs(a.z - b.z), std::fabs(a.w - b.w)});
    }
    return difference > largest * 1e-6f;
}

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
    if (!pipeline_.IsValid()) {
        AETHER_LOG_WARN("PlayerRenderer", "Temporal anti-aliasing pipeline unavailable");
        return;
    }
    // With frame read buffers the shader reads skinned movers' last-frame positions; the buffer must be bound for every draw, so a
    // small one stands in on frames without any.
    static const std::string reading_source = std::string("#define AETHER_PREVIOUS_POSITIONS\n") + kTaaMotionShader;
    if (device.SupportsFrameConstants()) no_positions_ = device.CreateFrameConstantBuffer(16);
    motion_reads_positions_ = no_positions_.IsValid();
    gfx::rhi::PipelineDesc motion;
    motion.hlsl_source = motion_reads_positions_ ? reading_source.c_str() : kTaaMotionShader;
    motion.frame_read_buffer = motion_reads_positions_;
    // The read buffer comes with frame constants (Vulkan's set 3 needs a set 2); the shader names none, and no_positions_ is bound there.
    motion.frame_constants_size_bytes = motion_reads_positions_ ? 16u : 0u;
    motion.push_constant_size_bytes = sizeof(TaaMotionConstants);
    motion.enable_bindless_textures = true;
    motion.render_texture_target = true;
    motion.use_vertex_buffer = true;
    motion.vertex_layout = gfx::rhi::VertexLayout::PositionNormalUvTangent;
    motion.cull_back_face = false; // the depth test keeps the front surface
    motion_pipeline_ = device.CreatePipeline(motion, swap_chain);
    if (!motion_pipeline_.IsValid()) {
        AETHER_LOG_WARN("PlayerRenderer", "Motion pipeline unavailable; objects moving on their own keep the colour clip alone");
    }
}

TaaPass::~TaaPass() {
    Release();
    if (motion_pipeline_.IsValid()) device_.DestroyPipeline(motion_pipeline_);
    if (no_positions_.IsValid()) device_.DestroyBuffer(no_positions_);
    if (pipeline_.IsValid()) device_.DestroyPipeline(pipeline_);
}

void TaaPass::Release() {
    for (gfx::rhi::SampledTextureHandle& texture : history_) {
        if (texture.IsValid()) device_.DestroyTexture(texture);
        texture = {};
    }
    if (motion_.IsValid()) device_.DestroyTexture(motion_);
    motion_ = {};
    last_motion_ = 0;
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
    // As JitterProjection adds it.
    jitter_ndc_[0] = 2.0f * x / static_cast<f32>(std::max(swap_chain_.Width(), 1u));
    jitter_ndc_[1] = 2.0f * y / static_cast<f32>(std::max(swap_chain_.Height(), 1u));
}

u32 TaaPass::DrawMotion(gfx::rhi::ICommandList& commands, const Mat4& view_projection, std::span<const TaaMover> movers,
                        gfx::rhi::BufferHandle previous_positions) {
    if (movers.empty() || !motion_pipeline_.IsValid()) return 0;
    if (!motion_.IsValid()) {
        motion_ = device_.CreateRenderTexture(width_, height_);
        if (!motion_.IsValid() || motion_.index >= gfx::rhi::kMaxBindlessTextures) {
            if (motion_.IsValid()) device_.DestroyTexture(motion_);
            motion_ = {};
            return 0;
        }
    }
    const gfx::rhi::ClearColor none{0.0f, 0.0f, 0.0f, 0.0f};
    if (!commands.BeginTexturePass(motion_, &none)) return 0;
    commands.BindPipeline(motion_pipeline_);
    commands.BindBindlessTextures();
    const bool have_positions = motion_reads_positions_ && previous_positions.IsValid();
    if (motion_reads_positions_) {
        commands.BindFrameConstants(no_positions_);
        commands.BindFrameReadBuffer(have_positions ? previous_positions : no_positions_);
    }
    for (const TaaMover& mover : movers) {
        if (!mover.vertices.IsValid() || !mover.indices.IsValid() || mover.index_count == 0) continue;
        if (mover.previous_positions != kTaaNoPreviousPositions && !have_positions && !TaaTransformMoved(mover.previous_model, mover.model)) {
            continue; // a pose change this device can't follow: the colour clip holds it, as before
        }
        TaaMotionConstants constants;
        const Mat4 mvp = view_projection * mover.model;
        const Mat4 previous_mvp = previous_view_projection_ * mover.previous_model;
        std::memcpy(constants.mvp, &mvp.cols[0], sizeof(constants.mvp));
        std::memcpy(constants.previous_mvp, &previous_mvp.cols[0], sizeof(constants.previous_mvp));
        constants.jitter[0] = jitter_ndc_[0];
        constants.jitter[1] = jitter_ndc_[1];
        constants.depth = scene_depth_;
        constants.previous_positions = have_positions ? mover.previous_positions : kTaaNoPreviousPositions;
        commands.BindVertexBuffer(mover.vertices, mover.vertex_stride);
        commands.BindIndexBuffer(mover.indices, gfx::rhi::IndexFormat::UInt32);
        commands.SetPushConstants(&constants, sizeof(constants));
        commands.DrawIndexed(mover.index_count);
    }
    commands.EndRenderPass();
    return motion_.index;
}

u32 TaaPass::Resolve(gfx::rhi::ICommandList& commands, const Mat4& view_projection, u64 camera, std::span<const TaaMover> movers,
                     gfx::rhi::BufferHandle previous_positions) {
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
    last_motion_ = keep ? DrawMotion(commands, view_projection, movers, previous_positions) : 0;
    constants.motion = last_motion_;

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
