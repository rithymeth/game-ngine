#include "virtual_shadow.h"

#include "aether/assets/environment.h"
#include "aether/core/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <tuple>

namespace aether::player {

namespace {

constexpr char kVsmLibrary[] = R"(
#define VSM_LEVELS 6
#define VSM_PAGE_TEXELS 128
#define VSM_BASE_TEXEL 0.002

float VsmTexel(uint level) { return VSM_BASE_TEXEL * (float)(1u << level); }

// The level whose texel is the smallest not smaller than `footprint` (a pixel's size on the surface), as player/src/virtual_shadow.cpp.
uint VsmLevelForFootprint(float footprint) {
    return (uint)clamp(ceil(log2(max(footprint, 1e-9) / VSM_BASE_TEXEL)), 0.0, VSM_LEVELS - 1.0);
}
)";

constexpr char kFeedbackShader[] = R"(
struct FeedbackConstants {
    float4x4 g_InvViewProjection;
    float3 g_CameraPosition;
    float g_FootprintScale;
    float3 g_LightRight;
    float g_FootprintConstant;
    float3 g_LightUp;
    uint g_Depth;
    int2 g_Jitter;
    int2 g_ScreenSize;
};
#ifdef __spirv__
[[vk::push_constant]]
#endif
ConstantBuffer<FeedbackConstants> g_PC : register(b0);
Texture2D g_Textures[128] : register(t0, space0);
SamplerState g_Sampler : register(s0, space1);

float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

// One pixel of each block of the screen: the page its surface falls in, in the light's space (level + 1, x & 31, y & 31); zero for the sky.
float4 PSMain(float4 position : SV_Position) : SV_Target {
    int2 texel = int2(position.xy) * 8 + g_PC.g_Jitter;
    if (any(texel >= g_PC.g_ScreenSize)) return float4(0, 0, 0, 0);
    float depth = g_Textures[g_PC.g_Depth].Load(int3(texel, 0)).r;
    if (depth >= 1.0) return float4(0, 0, 0, 0);
    float2 uv = (float2(texel) + 0.5) / float2(g_PC.g_ScreenSize);
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 world = mul(g_PC.g_InvViewProjection, float4(ndc, depth, 1.0));
    float3 p = world.xyz / world.w;
    float footprint = g_PC.g_FootprintScale * length(p - g_PC.g_CameraPosition) + g_PC.g_FootprintConstant;
    uint level = VsmLevelForFootprint(footprint);
    float page = VsmTexel(level) * VSM_PAGE_TEXELS;
    int2 index = (int2)floor(float2(dot(p, g_PC.g_LightRight), dot(p, g_PC.g_LightUp)) / page);
    return float4(level + 1, index.x & 31, index.y & 31, 1.0);
}
)";

struct FeedbackConstants {
    f32 inverse_view_projection[16];
    f32 camera_position[3];
    f32 footprint_scale;
    f32 light_right[3];
    f32 footprint_constant;
    f32 light_up[3];
    u32 depth;
    i32 jitter[2];
    i32 screen_size[2];
};
static_assert(sizeof(FeedbackConstants) == 128);

} // namespace

f32 VsmTexelSize(u32 level) { return kVsmBaseTexel * static_cast<f32>(1u << std::min(level, kVsmLevels - 1)); }
f32 VsmPageSize(u32 level) { return VsmTexelSize(level) * static_cast<f32>(kVsmPageTexels); }

u32 VsmLevelForFootprint(f32 footprint) {
    const f32 level = std::ceil(std::log2(std::max(footprint, 1e-9f) / kVsmBaseTexel));
    return static_cast<u32>(std::clamp(level, 0.0f, static_cast<f32>(kVsmLevels - 1)));
}

VsmLight VsmLightBasis(const Vec3& light_direction) {
    VsmLight light;
    light.direction = light_direction.LengthSq() > 1e-12f ? light_direction.Normalized() : Vec3(0, -1, 0);
    const Vec3 hint = std::fabs(light.direction.y) < 0.99f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
    light.right = hint.Cross(light.direction).Normalized();
    light.up = light.direction.Cross(light.right);
    return light;
}

void VsmLightSpace(const VsmLight& light, const Vec3& p, f32& u, f32& v, f32& depth) {
    u = p.Dot(light.right);
    v = p.Dot(light.up);
    depth = p.Dot(light.direction);
}

VsmPage VsmPageOf(const VsmLight& light, u32 level, const Vec3& p) {
    f32 u = 0, v = 0, depth = 0;
    VsmLightSpace(light, p, u, v, depth);
    const f32 size = VsmPageSize(level);
    return {level, static_cast<i32>(std::floor(u / size)), static_cast<i32>(std::floor(v / size))};
}

i32 VsmWindowOrigin(i32 camera_page) { return camera_page - static_cast<i32>(kVsmWindowPages / 2); }

i32 VsmRecoverPage(i32 window_origin, u32 wrapped) {
    const i32 size = static_cast<i32>(kVsmWindowPages);
    const i32 offset = ((static_cast<i32>(wrapped & (kVsmWindowPages - 1)) - window_origin) % size + size) % size;
    return window_origin + offset;
}

std::array<std::array<i32, 2>, kVsmLevels> VsmWindowOrigins(const VsmLight& light, const Vec3& camera_position) {
    std::array<std::array<i32, 2>, kVsmLevels> origins{};
    for (u32 level = 0; level < kVsmLevels; ++level) {
        const VsmPage page = VsmPageOf(light, level, camera_position);
        origins[level] = {VsmWindowOrigin(page.x), VsmWindowOrigin(page.y)};
    }
    return origins;
}

const char* VsmShaderLibrary() { return kVsmLibrary; }

std::vector<VsmRequest> VsmDecodeFeedback(const u8* rows, u32 width, u32 height, u32 pitch_bytes,
                                          const std::array<std::array<i32, 2>, kVsmLevels>& window_origins) {
    std::map<std::tuple<u32, i32, i32>, u32> hits;
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            u16 texel[4];
            std::memcpy(texel, rows + static_cast<usize>(y) * pitch_bytes + static_cast<usize>(x) * 8, sizeof(texel));
            const f32 marker = assets::HalfToFloat(texel[0]);
            if (!(marker >= 0.5f)) continue; // nothing drawn here
            const u32 level = static_cast<u32>(marker + 0.5f) - 1;
            if (level >= kVsmLevels) continue;
            const u32 wrapped_x = static_cast<u32>(assets::HalfToFloat(texel[1]) + 0.5f);
            const u32 wrapped_y = static_cast<u32>(assets::HalfToFloat(texel[2]) + 0.5f);
            ++hits[{level, VsmRecoverPage(window_origins[level][0], wrapped_x), VsmRecoverPage(window_origins[level][1], wrapped_y)}];
        }
    }
    std::vector<VsmRequest> out;
    out.reserve(hits.size());
    for (const auto& [key, count] : hits) out.push_back({{std::get<0>(key), std::get<1>(key), std::get<2>(key)}, count});
    std::stable_sort(out.begin(), out.end(), [](const VsmRequest& a, const VsmRequest& b) { return a.hits > b.hits; });
    return out;
}

VsmFeedback::VsmFeedback(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& swap_chain) : device_(device) {
    if (!device.SupportsReadback()) return;
    const gfx::rhi::SampledTextureHandle probe = device.CreateRenderTexture(4, 4);
    if (!probe.IsValid()) return;
    device.DestroyTexture(probe);
    static const std::string source = std::string(kVsmLibrary) + kFeedbackShader;
    gfx::rhi::PipelineDesc desc;
    desc.hlsl_source = source.c_str();
    desc.push_constant_size_bytes = sizeof(FeedbackConstants);
    desc.enable_bindless_textures = true;
    desc.render_texture_target = true;
    pipeline_ = device.CreatePipeline(desc, swap_chain);
    if (!pipeline_.IsValid()) AETHER_LOG_WARN("PlayerRenderer", "Shadow feedback pipeline unavailable; no virtual shadow maps");
}

VsmFeedback::~VsmFeedback() {
    Release();
    if (pipeline_.IsValid()) device_.DestroyPipeline(pipeline_);
}

void VsmFeedback::Release() {
    if (image_.IsValid()) device_.DestroyTexture(image_);
    image_ = {};
    for (gfx::rhi::BufferHandle& buffer : readback_) {
        if (buffer.IsValid()) device_.DestroyBuffer(buffer);
        buffer = {};
    }
    for (Pending& pending : pending_) pending = {};
    width_ = height_ = 0;
}

bool VsmFeedback::Ensure(u32 width, u32 height) {
    if (image_.IsValid() && width_ == width && height_ == height) return true;
    Release();
    image_ = device_.CreateRenderTexture(width, height);
    if (!image_.IsValid()) return false;
    const u64 bytes = static_cast<u64>(gfx::rhi::ReadbackRowPitch(width, 8)) * height;
    for (gfx::rhi::BufferHandle& buffer : readback_) {
        buffer = device_.CreateReadbackBuffer(bytes);
        if (!buffer.IsValid()) {
            Release();
            return false;
        }
    }
    width_ = width;
    height_ = height;
    return true;
}

bool VsmFeedback::Record(gfx::rhi::ICommandList& commands, const VsmFeedbackParams& params, u32 slot) {
    if (!Valid() || params.screen_width == 0 || params.screen_height == 0 || slot >= kSlots) return false;
    const u32 width = (params.screen_width + kVsmFeedbackScale - 1) / kVsmFeedbackScale;
    const u32 height = (params.screen_height + kVsmFeedbackScale - 1) / kVsmFeedbackScale;
    if (!Ensure(width, height)) return false;

    FeedbackConstants constants{};
    std::memcpy(constants.inverse_view_projection, &params.inverse_view_projection.cols[0], sizeof(constants.inverse_view_projection));
    constants.camera_position[0] = params.camera_position.x, constants.camera_position[1] = params.camera_position.y,
    constants.camera_position[2] = params.camera_position.z;
    constants.footprint_scale = params.footprint_scale;
    constants.footprint_constant = params.footprint_constant;
    constants.light_right[0] = params.light.right.x, constants.light_right[1] = params.light.right.y, constants.light_right[2] = params.light.right.z;
    constants.light_up[0] = params.light.up.x, constants.light_up[1] = params.light.up.y, constants.light_up[2] = params.light.up.z;
    constants.depth = params.depth_texture;
    const u32 block = params.frame % (kVsmFeedbackScale * kVsmFeedbackScale);
    constants.jitter[0] = static_cast<i32>(block % kVsmFeedbackScale);
    constants.jitter[1] = static_cast<i32>(block / kVsmFeedbackScale);
    constants.screen_size[0] = static_cast<i32>(params.screen_width);
    constants.screen_size[1] = static_cast<i32>(params.screen_height);

    const gfx::rhi::ClearColor none{0.0f, 0.0f, 0.0f, 0.0f};
    if (!commands.BeginTexturePass(image_, &none)) return false;
    commands.BindPipeline(pipeline_);
    commands.BindBindlessTextures();
    commands.SetPushConstants(&constants, sizeof(constants));
    commands.Draw(3);
    commands.EndRenderPass();
    if (!commands.CopyRenderTextureToReadback(image_, readback_[slot])) return false;
    Pending& pending = pending_[slot];
    pending.valid = true;
    pending.width = width;
    pending.height = height;
    pending.window_origins = VsmWindowOrigins(params.light, params.camera_position);
    return true;
}

std::vector<VsmRequest> VsmFeedback::Collect(u32 slot) {
    if (slot >= kSlots || !pending_[slot].valid || !readback_[slot].IsValid()) return {};
    Pending& pending = pending_[slot];
    pending.valid = false;
    const u32 pitch = gfx::rhi::ReadbackRowPitch(pending.width, 8);
    std::vector<u8> rows(static_cast<usize>(pitch) * pending.height);
    if (!device_.ReadReadbackBuffer(readback_[slot], rows.data(), rows.size())) return {};
    return VsmDecodeFeedback(rows.data(), pending.width, pending.height, pitch, pending.window_origins);
}

} // namespace aether::player
