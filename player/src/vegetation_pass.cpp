#include "vegetation_pass.h"

#include "aether/core/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace aether::player {

namespace {

constexpr char kVegetationBody[] = R"(
struct PushConstants {
    float4x4 g_ViewProjection;
    float4 g_Camera;  // camera position; w: seconds
    float4 g_Light;   // sun direction (towards the sun); w: sun intensity
    uint4 g_Misc;     // x: the palette's texture index; y: the sky intensity (float bits)
};
#ifdef __spirv__
[[vk::push_constant]]
#endif
ConstantBuffer<PushConstants> g_PC : register(b0);
Texture2D g_Textures[128] : register(t0, space0);
SamplerState g_Sampler : register(s0, space1);

struct VSInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};
struct PSInput {
    float4 position : SV_POSITION;
    float3 normal : TEXCOORD0;
    float2 uv : TEXCOORD1;
};

PSInput VSMain(VSInput input) {
    PSInput output;
    float3 p = input.position;
    // The wind: what bends (uv.y > 0) sways more toward its tip, out of step with its neighbours by where it stands.
    float weight = input.uv.y * input.uv.y;
    float t = g_PC.g_Camera.w;
    float phase = dot(p.xz, float2(0.37, 0.29));
    float gust = sin(t * 1.7 + phase) + 0.5 * sin(t * 3.1 + phase * 1.9);
    p.x += gust * 0.22 * weight;
    p.z += cos(t * 1.3 + phase * 0.8) * 0.15 * weight;
    output.position = mul(g_PC.g_ViewProjection, float4(p, 1.0));
    output.normal = input.normal;
    output.uv = input.uv;
    return output;
}

float3 SRGBToLinear(float3 encoded) {
    encoded = saturate(encoded);
    return lerp(encoded / 12.92, pow((encoded + 0.055) / 1.055, 2.4), step(0.04045, encoded));
}
float3 LinearToSRGB(float3 linear_color) {
    linear_color = max(linear_color, 0.0);
    return lerp(linear_color * 12.92, 1.055 * pow(linear_color, 1.0 / 2.4) - 0.055, step(0.0031308, linear_color));
}

float4 PSMain(PSInput input) : SV_TARGET {
    float3 albedo = SRGBToLinear(g_Textures[g_PC.g_Misc.x].Sample(g_Sampler, float2(input.uv.x, 0.5)).rgb);
    // What sways is darker at its foot: a tuft or a crown shades itself.
    albedo *= input.uv.y > 0.0 ? lerp(0.62, 1.0, saturate(input.uv.y * 2.0)) : 1.0;
    float3 normal = normalize(input.normal);
    float3 sun = normalize(g_PC.g_Light.xyz);
    float sun_intensity = g_PC.g_Light.w;
    float sky_intensity = asfloat(g_PC.g_Misc.y);
    float sky_fill = 0.28 + 0.14 * saturate(normal.y);
    // Half-Lambert, a little: leaves are thin, and light comes through them.
    float n_dot_l = saturate(dot(normal, sun));
    float lit = lerp(n_dot_l, n_dot_l * 0.5 + 0.5, 0.35);
    float3 color = albedo * (sky_fill * sky_intensity + sun_intensity * lit / 3.14159265);
#ifdef AETHER_HDR_TARGET
    return float4(max(color, 0.0), 1.0);
#else
    color = color / (color + 1.0);
    return float4(LinearToSRGB(color), 1.0);
#endif
}
)";

struct Constants {
    Mat4 view_projection;
    f32 camera[4];
    f32 light[4];
    u32 misc[4];
};
static_assert(sizeof(Constants) == 112);

} // namespace

f32 VegetationDrawDistance(PlantKind kind) {
    switch (kind) {
    case PlantKind::Pine: return 450.0f;
    case PlantKind::Birch: return 450.0f;
    case PlantKind::Boulder: return 300.0f;
    case PlantKind::Shrub: return 170.0f;
    case PlantKind::Reeds: return 120.0f;
    case PlantKind::Grass: return 80.0f;
    }
    return 100.0f;
}

std::string VegetationShaderSource(bool hdr_target) { return std::string(hdr_target ? "#define AETHER_HDR_TARGET 1\n" : "") + kVegetationBody; }

VegetationPass::VegetationPass(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& swap_chain, bool hdr_target) : device_(device), gpu_(device) {
    gfx::rhi::PipelineDesc desc;
    desc.hlsl_source = VegetationShaderSource(hdr_target);
    desc.scene_color_target = hdr_target;
    desc.push_constant_size_bytes = sizeof(Constants);
    desc.use_vertex_buffer = true;
    desc.vertex_layout = gfx::rhi::VertexLayout::PositionNormalUv;
    desc.enable_bindless_textures = true;
    desc.depth_test = true;
    desc.depth_write = true;
    pipeline_ = device.CreatePipeline(desc, swap_chain);
    if (!pipeline_.IsValid()) {
        AETHER_LOG_WARN("Vegetation", "The vegetation pipeline did not compile; plants are not drawn");
        return;
    }
    const std::array<u8, kPaletteColumns * 4> palette = PalettePixels();
    palette_ = device.CreateTexture(kPaletteColumns, 1, palette.data());
    if (!palette_.IsValid()) AETHER_LOG_WARN("Vegetation", "The device has no room for the palette texture; plants are not drawn");
}

VegetationPass::~VegetationPass() {
    Clear();
    if (palette_.IsValid()) device_.DestroyTexture(palette_);
    if (pipeline_.IsValid()) device_.DestroyPipeline(pipeline_);
}

void VegetationPass::Clear() {
    for (const Mesh& m : meshes_) {
        gpu_.DestroyBuffer(m.vertex_buffer);
        gpu_.DestroyBuffer(m.index_buffer);
    }
    meshes_.clear();
    gpu_bytes_ = 0;
}

u32 VegetationPass::Upload(const VegetationBatch& batch) {
    Clear();
    if (!Valid()) return 0;
    for (const VegChunk& c : batch.chunks) {
        if (c.vertices.empty() || c.indices.empty()) continue;
        Mesh m;
        m.kind = c.kind;
        m.bounds_min = c.bounds_min;
        m.bounds_max = c.bounds_max;
        m.centre = (c.bounds_min + c.bounds_max) * 0.5f;
        const Vec3 half = (c.bounds_max - c.bounds_min) * 0.5f;
        m.radius = half.Length();
        const u64 vbytes = c.vertices.size() * sizeof(VegVertex), ibytes = c.indices.size() * sizeof(u32);
        m.vertex_buffer = gpu_.CreateBuffer(c.vertices.data(), vbytes, false);
        m.index_buffer = gpu_.CreateBuffer(c.indices.data(), ibytes, true);
        if (m.vertex_buffer == 0 || m.index_buffer == 0) {
            gpu_.DestroyBuffer(m.vertex_buffer);
            gpu_.DestroyBuffer(m.index_buffer);
            continue;
        }
        m.index_count = static_cast<u32>(c.indices.size());
        gpu_bytes_ += vbytes + ibytes;
        meshes_.push_back(m);
    }
    if (meshes_.size() != batch.chunks.size()) {
        AETHER_LOG_WARN("Vegetation", "%zu of %zu vegetation meshes could not be uploaded", batch.chunks.size() - meshes_.size(), batch.chunks.size());
    }
    return static_cast<u32>(meshes_.size());
}

VegetationPass::DrawStats VegetationPass::Draw(gfx::rhi::ICommandList& commands, const Mat4& view_projection, const ViewFrustum& frustum,
                                               const Vec3& camera_position, const Vec3& sun_direction, f32 sun_intensity, f32 time_seconds) {
    DrawStats stats;
    if (!Valid() || meshes_.empty()) return stats;
    Constants constants{};
    constants.view_projection = view_projection;
    constants.camera[0] = camera_position.x, constants.camera[1] = camera_position.y, constants.camera[2] = camera_position.z;
    constants.camera[3] = std::fmod(time_seconds, 3600.0f);
    constants.light[0] = sun_direction.x, constants.light[1] = sun_direction.y, constants.light[2] = sun_direction.z;
    constants.light[3] = sun_intensity;
    constants.misc[0] = palette_.index;
    const f32 sky_intensity = 1.0f;
    std::memcpy(&constants.misc[1], &sky_intensity, sizeof(f32));

    commands.BindPipeline(pipeline_);
    commands.BindBindlessTextures();
    for (const Mesh& m : meshes_) {
        // Distance to the nearest point of the chunk's plants, and the frustum against their bounding sphere.
        const Vec3 nearest(std::clamp(camera_position.x, m.bounds_min.x, m.bounds_max.x), std::clamp(camera_position.y, m.bounds_min.y, m.bounds_max.y),
                           std::clamp(camera_position.z, m.bounds_min.z, m.bounds_max.z));
        const f32 distance = (nearest - camera_position).Length();
        if (distance > VegetationDrawDistance(m.kind) || !SphereVisible(frustum, m.centre, m.radius)) {
            ++stats.meshes_culled;
            continue;
        }
        const gfx::rhi::BufferHandle vertices = gpu_.Find(m.vertex_buffer), indices = gpu_.Find(m.index_buffer);
        if (!vertices.IsValid() || !indices.IsValid()) continue;
        commands.BindVertexBuffer(vertices, static_cast<u32>(sizeof(VegVertex)));
        commands.BindIndexBuffer(indices, gfx::rhi::IndexFormat::UInt32);
        commands.SetPushConstants(&constants, sizeof(constants));
        commands.DrawIndexed(m.index_count);
        ++stats.meshes_drawn;
        stats.triangles_drawn += m.index_count / 3;
    }
    return stats;
}

} // namespace aether::player
