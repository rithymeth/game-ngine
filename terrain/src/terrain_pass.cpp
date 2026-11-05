#include "aether/terrain/terrain_pass.h"

#include "aether/core/log.h"

#include <algorithm>
#include <limits>
#include <cmath>
#include <cstring>

namespace aether::terrain {
namespace {

constexpr usize kVertexStride = 32; // position(3) + normal(3) + uv(2) floats, what GenerateChunkVertices makes

// The blend of painted layer weights by layer height, the arithmetic of BlendTerrainLayerHeights (terrain/splat.cpp). A layer with no painted
// weight stays at exactly zero; a flat or black height map keeps a small baseline so the layers aren't all lost; all-zero gives layer 0.
constexpr char kBlendHlsl[] = R"(
float4 AetherBlendTerrainHeights(float4 painted, float4 heights, float contrast) {
    float strength = saturate(contrast);
    float4 weights = max(painted, float4(0.0, 0.0, 0.0, 0.0));
    float4 h = saturate(heights);
    float4 result = weights * ((1.0 - strength) + strength * (0.01 + h));
    float total = result.x + result.y + result.z + result.w;
    if (total <= 0.0) return float4(1.0, 0.0, 0.0, 0.0);
    return result / total;
}
)";

constexpr char kTerrainBody[] = R"(
struct PushConstants {
    float4x4 g_ViewProjection;
    float4 g_SplatRect;   // origin x, origin z, 1 / width, 1 / depth
    float4 g_Camera;      // camera position, surface bits (roughness 6 bits and metallic 2 bits per layer)
    float4 g_Light;       // sun direction (towards the sun), height contrast
    uint4 g_Packed;       // x: layer 0..3 indices (6 bits each) and the splatmap's at bit 24; y: sun and sky intensity (halves); z, w: tiles (halves)
};
#ifdef __spirv__
[[vk::push_constant]]
#endif
ConstantBuffer<PushConstants> g_PC : register(b0);
Texture2D g_Textures[32] : register(t0, space0);
SamplerState g_Sampler : register(s0, space1);

struct VSInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};
struct PSInput {
    float4 position : SV_POSITION;
    float3 world : TEXCOORD0;
    float3 normal : TEXCOORD1;
};

PSInput VSMain(VSInput input) {
    PSInput output;
    output.position = mul(g_PC.g_ViewProjection, float4(input.position, 1.0));
    output.world = input.position;
    output.normal = input.normal;
    return output;
}

float3 SRGBToLinear(float3 encoded) {
    encoded = saturate(encoded);
    float3 linear_low = encoded / 12.92;
    float3 linear_high = pow((encoded + 0.055) / 1.055, 2.4);
    return lerp(linear_low, linear_high, step(0.04045, encoded));
}
float3 LinearToSRGB(float3 linear_color) {
    linear_color = max(linear_color, 0.0);
    float3 encoded_low = linear_color * 12.92;
    float3 encoded_high = 1.055 * pow(linear_color, 1.0 / 2.4) - 0.055;
    return lerp(encoded_low, encoded_high, step(0.0031308, linear_color));
}

float4 PSMain(PSInput input) : SV_TARGET {
    uint splat_index = (g_PC.g_Packed.x >> 24) & 63;
    // The painted weights, read with the texel mapping of SplatmapData::GetWeights: u = 0 is the first texel's centre and u = 1 the last's.
    float2 splat_uv = saturate((input.world.xz - g_PC.g_SplatRect.xy) * g_PC.g_SplatRect.zw);
    uint splat_width, splat_height;
    g_Textures[splat_index].GetDimensions(splat_width, splat_height);
    float2 splat_size = float2(splat_width, splat_height);
    float4 painted = g_Textures[splat_index].Sample(g_Sampler, (splat_uv * (splat_size - 1.0) + 0.5) / splat_size);

    float3 albedo[4];
    float heights[4];
    float tiles[4] = { f16tof32(g_PC.g_Packed.z & 0xffff), f16tof32(g_PC.g_Packed.z >> 16), f16tof32(g_PC.g_Packed.w & 0xffff), f16tof32(g_PC.g_Packed.w >> 16) };
    uint surface = asuint(g_PC.g_Camera.w);
    float rough[4];
    float metal[4];
    for (uint layer = 0; layer < 4; ++layer) {
        uint index = (g_PC.g_Packed.x >> (6 * layer)) & 63;
        float4 texel = g_Textures[index].Sample(g_Sampler, input.world.xz * tiles[layer]);
        albedo[layer] = SRGBToLinear(texel.rgb);
        heights[layer] = texel.a;
        rough[layer] = float((surface >> (6 * layer)) & 63) / 63.0;
        metal[layer] = float((surface >> (24 + 2 * layer)) & 3) / 3.0;
    }
    float4 weights = AetherBlendTerrainHeights(painted, float4(heights[0], heights[1], heights[2], heights[3]), g_PC.g_Light.w);
    float3 base_color = albedo[0] * weights.x + albedo[1] * weights.y + albedo[2] * weights.z + albedo[3] * weights.w;

    float roughness = dot(weights, float4(rough[0], rough[1], rough[2], rough[3]));
    float metallic = dot(weights, float4(metal[0], metal[1], metal[2], metal[3]));

    float3 normal = normalize(input.normal);
    float3 sun_direction = normalize(g_PC.g_Light.xyz);
    float sun_intensity = f16tof32(g_PC.g_Packed.y & 0xffff);
    float sky_intensity = f16tof32(g_PC.g_Packed.y >> 16);
    float sky_fill = 0.28 + 0.14 * saturate(normal.y);
    float n_dot_l = saturate(dot(normal, sun_direction));

    float3 view = g_PC.g_Camera.xyz - input.world;
    float view_length = length(view);
    view = view_length > 0.0001 ? view / view_length : normal;
    float3 half_vector = sun_direction + view;
    float half_length = length(half_vector);
    half_vector = half_length > 0.0001 ? half_vector / half_length : normal;
    float n_dot_v = max(dot(normal, view), 0.0001);
    float n_dot_h = saturate(dot(normal, half_vector));
    float v_dot_h = saturate(dot(view, half_vector));
    float r = max(roughness, 0.045);
    float a2 = r * r * r * r;
    float d_denominator = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    float distribution = a2 / (3.14159265 * d_denominator * d_denominator);
    float k = (r + 1.0) * (r + 1.0) / 8.0;
    float geometry = (n_dot_l / (n_dot_l * (1.0 - k) + k)) * (n_dot_v / (n_dot_v * (1.0 - k) + k));
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), base_color, metallic);
    float3 fresnel = f0 + (1.0 - f0) * pow(1.0 - v_dot_h, 5.0);
    float3 specular = distribution * geometry * fresnel / (4.0 * n_dot_v);
    float3 color = base_color * (1.0 - metallic) * (sky_fill * sky_intensity + sun_intensity * n_dot_l / 3.14159265) + sun_intensity * specular;
    color = color / (color + 1.0);
    return float4(LinearToSRGB(color), 1.0);
}
)";

std::string FullTerrainShader() { return std::string(kBlendHlsl) + kTerrainBody; }

f32 LinearToSrgb(f32 c) {
    c = std::max(c, 0.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}
u32 FloatToHalf(f32 v) {
    u32 b;
    std::memcpy(&b, &v, 4);
    const u32 sign = (b >> 16) & 0x8000u, m = b & 0x7fffffu;
    const int e = static_cast<int>((b >> 23) & 0xff) - 127 + 15;
    if (((b >> 23) & 0xff) == 0xff) return sign | 0x7c00u | (m ? 0x200u : 0u);
    if (e >= 31) return sign | 0x7c00u;
    if (e <= 0) {
        if (e < -10) return sign;
        const u32 full = m | 0x800000u, shift = static_cast<u32>(14 - e);
        u32 r = full >> shift;
        if ((full >> (shift - 1)) & 1u) ++r;
        return sign | r;
    }
    u32 r = sign | (static_cast<u32>(e) << 10) | (m >> 13);
    if (m & 0x1000u) ++r;
    return r;
}
f32 HalfToFloat(u32 h) {
    const u32 exponent = (h >> 10) & 31u, mantissa = h & 1023u;
    f32 v;
    if (exponent == 0) v = std::ldexp(static_cast<f32>(mantissa), -24);
    else if (exponent == 31) v = mantissa ? std::numeric_limits<f32>::quiet_NaN() : std::numeric_limits<f32>::infinity();
    else v = std::ldexp(static_cast<f32>(1024 + mantissa), static_cast<int>(exponent) - 25);
    return (h & 0x8000u) ? -v : v;
}
f32 ThroughHalf(f32 v) { return HalfToFloat(FloatToHalf(v)); }
u32 Quantize(f32 v, u32 steps) { return static_cast<u32>(std::lround(std::clamp(v, 0.0f, 1.0f) * static_cast<f32>(steps))); }

u8 ToByte(f32 v) { return static_cast<u8>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

} // namespace

const char* TerrainBlendHlsl() { return kBlendHlsl; }

const char* TerrainShaderSource() {
    static const std::string source = FullTerrainShader();
    return source.c_str();
}

u64 RhiTerrainGpu::CreateBuffer(const void* data, u64 bytes, bool index) {
    if (data == nullptr || bytes == 0) return 0;
    gfx::rhi::BufferHandle handle;
    if (index) {
        handle = device_.CreateIndexBuffer(data, bytes, gfx::rhi::IndexFormat::UInt32);
    } else {
        if (bytes % kVertexStride != 0) return 0; // the layout's stride must divide it
        handle = device_.CreateVertexBuffer(data, bytes);
    }
    if (!handle.IsValid()) return 0;
    buffers_.push_back(handle);
    ++live_;
    return buffers_.size(); // id - 1 is the slot
}

RhiTerrainGpu::~RhiTerrainGpu() {
    for (const gfx::rhi::BufferHandle handle : buffers_) {
        if (handle.IsValid()) device_.DestroyBuffer(handle);
    }
}

void RhiTerrainGpu::DestroyBuffer(u64 buffer) {
    if (buffer == 0 || buffer > buffers_.size() || !buffers_[buffer - 1].IsValid()) return;
    device_.DestroyBuffer(buffers_[buffer - 1]);
    buffers_[buffer - 1] = gfx::rhi::BufferHandle{};
    --live_;
}

gfx::rhi::BufferHandle RhiTerrainGpu::Find(u64 buffer) const {
    if (buffer == 0 || buffer > buffers_.size()) return {};
    return buffers_[buffer - 1];
}

gfx::rhi::SampledTextureHandle UploadSplatmap(gfx::rhi::IDevice& device, const SplatmapData& splatmap) {
    if (splatmap.width == 0 || splatmap.height == 0 || splatmap.pixels.size() < static_cast<usize>(splatmap.width) * splatmap.height * 4) return {};
    return device.CreateTexture(splatmap.width, splatmap.height, splatmap.pixels.data());
}

TerrainConstants PackTerrainConstants(const Mat4& view_projection, const TerrainDrawParams& params) {
    TerrainConstants c{};
    c.view_projection = view_projection;
    const f32 width = params.splat_size[0] > 0.0f ? params.splat_size[0] : 1.0f;
    const f32 depth = params.splat_size[1] > 0.0f ? params.splat_size[1] : 1.0f;
    c.splat_rect[0] = params.splat_origin[0], c.splat_rect[1] = params.splat_origin[1];
    c.splat_rect[2] = 1.0f / width, c.splat_rect[3] = 1.0f / depth;
    u32 packed = 0;
    for (u32 i = 0; i < 4; ++i) {
        packed |= (params.layers[i].albedo_height.index & 63u) << (6 * i);
    }
    packed |= (params.splatmap.index & 63u) << 24;
    c.packed[0] = packed;
    c.packed[1] = FloatToHalf(params.sun_intensity) | (FloatToHalf(params.sky_intensity) << 16);
    c.packed[2] = FloatToHalf(params.layers[0].tile_scale) | (FloatToHalf(params.layers[1].tile_scale) << 16);
    c.packed[3] = FloatToHalf(params.layers[2].tile_scale) | (FloatToHalf(params.layers[3].tile_scale) << 16);
    u32 surface = 0;
    for (u32 i = 0; i < 4; ++i) surface |= (Quantize(params.layers[i].roughness, 63) << (6 * i)) | (Quantize(params.layers[i].metallic, 3) << (24 + 2 * i));
    c.camera[0] = params.camera_position.x, c.camera[1] = params.camera_position.y, c.camera[2] = params.camera_position.z;
    std::memcpy(&c.camera[3], &surface, sizeof(u32));
    c.light[0] = params.sun_direction.x, c.light[1] = params.sun_direction.y, c.light[2] = params.sun_direction.z;
    c.light[3] = std::clamp(params.height_contrast, 0.0f, 1.0f);
    return c;
}

TerrainPass::~TerrainPass() {
    if (pipeline_.IsValid()) device_.DestroyPipeline(pipeline_);
}

TerrainPass::TerrainPass(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& swap_chain) : device_(device) {
    gfx::rhi::PipelineDesc desc;
    desc.hlsl_source = TerrainShaderSource();
    desc.push_constant_size_bytes = sizeof(TerrainConstants);
    desc.use_vertex_buffer = true;
    desc.vertex_layout = gfx::rhi::VertexLayout::PositionNormalUv;
    desc.enable_bindless_textures = true;
    desc.depth_test = true;
    desc.depth_write = true;
    pipeline_ = device.CreatePipeline(desc, swap_chain);
    if (!pipeline_.IsValid()) AETHER_LOG_WARN("Terrain", "The terrain pipeline did not compile; terrain is not drawn");
}

u32 TerrainPass::Draw(gfx::rhi::ICommandList& commands, const RhiTerrainGpu& gpu, std::span<const RenderChunk> chunks, const Mat4& view_projection,
                      const TerrainDrawParams& params) {
    if (!Valid()) return 0;
    const TerrainConstants constants = PackTerrainConstants(view_projection, params);
    commands.BindPipeline(pipeline_);
    commands.BindBindlessTextures();
    u32 drawn = 0;
    for (const RenderChunk& chunk : chunks) {
        const gfx::rhi::BufferHandle vertices = gpu.Find(chunk.vertex_buffer), indices = gpu.Find(chunk.index_buffer);
        if (!vertices.IsValid() || !indices.IsValid() || chunk.index_count == 0) continue;
        commands.BindVertexBuffer(vertices, static_cast<u32>(kVertexStride));
        commands.BindIndexBuffer(indices, gfx::rhi::IndexFormat::UInt32);
        commands.SetPushConstants(&constants, sizeof(constants));
        commands.DrawIndexed(chunk.index_count);
        ++drawn;
    }
    return drawn;
}

std::array<u8, 3> TerrainReferenceColor(const std::array<Vec3, 4>& albedo_linear, const std::array<f32, 4>& painted_weights,
                                        const std::array<f32, 4>& heights, const TerrainDrawParams& params, const Vec3& world_position) {
    const std::array<f32, 4> w = BlendTerrainLayerHeights(painted_weights, heights, params.height_contrast);
    Vec3 base{0, 0, 0};
    f32 roughness = 0.0f, metallic = 0.0f;
    for (usize i = 0; i < 4; ++i) {
        base = base + albedo_linear[i] * w[i];
        roughness += w[i] * static_cast<f32>(Quantize(params.layers[i].roughness, 63)) / 63.0f; // the same steps the constants hold
        metallic += w[i] * static_cast<f32>(Quantize(params.layers[i].metallic, 3)) / 3.0f;
    }
    const Vec3 normal{0, 1, 0};
    const Vec3 sun = params.sun_direction.Normalized();
    const f32 sun_intensity = ThroughHalf(params.sun_intensity), sky_intensity = ThroughHalf(params.sky_intensity);
    const f32 n_dot_l = std::clamp(normal.Dot(sun), 0.0f, 1.0f);
    const f32 sky_fill = 0.28f + 0.14f;
    Vec3 view = params.camera_position - world_position;
    const f32 view_length = std::sqrt(view.Dot(view));
    view = view_length > 0.0001f ? view * (1.0f / view_length) : normal;
    Vec3 half_vector = sun + view;
    const f32 half_length = std::sqrt(half_vector.Dot(half_vector));
    half_vector = half_length > 0.0001f ? half_vector * (1.0f / half_length) : normal;
    const f32 n_dot_v = std::max(normal.Dot(view), 0.0001f);
    const f32 n_dot_h = std::clamp(normal.Dot(half_vector), 0.0f, 1.0f);
    const f32 v_dot_h = std::clamp(view.Dot(half_vector), 0.0f, 1.0f);
    const f32 r = std::max(roughness, 0.045f);
    const f32 a2 = r * r * r * r;
    const f32 d_denominator = n_dot_h * n_dot_h * (a2 - 1.0f) + 1.0f;
    const f32 distribution = a2 / (3.14159265f * d_denominator * d_denominator);
    const f32 k = (r + 1.0f) * (r + 1.0f) / 8.0f;
    const f32 geometry = (n_dot_l / (n_dot_l * (1.0f - k) + k)) * (n_dot_v / (n_dot_v * (1.0f - k) + k));
    const f32 base_channel[3] = {base.x, base.y, base.z};
    const f32 diffuse_light = sky_fill * sky_intensity + sun_intensity * n_dot_l / 3.14159265f;
    const f32 fresnel_weight = std::pow(1.0f - v_dot_h, 5.0f);
    std::array<u8, 3> out{};
    for (usize i = 0; i < 3; ++i) {
        const f32 f0 = 0.04f + (base_channel[i] - 0.04f) * metallic;
        const f32 fresnel = f0 + (1.0f - f0) * fresnel_weight;
        const f32 specular = distribution * geometry * fresnel / (4.0f * n_dot_v);
        const f32 c = base_channel[i] * (1.0f - metallic) * diffuse_light + sun_intensity * specular;
        out[i] = ToByte(LinearToSrgb(c / (c + 1.0f)));
    }
    return out;
}

const std::vector<gfx::parity::Scene>& TerrainParityScenes() {
    static const std::vector<gfx::parity::Scene> scenes = [] {
        // painted = (u, 1 - u, v / 2, 0) and the layers' heights fixed; the contrast grows down the image.
        constexpr f32 kHeights[4] = {0.2f, 0.9f, 0.5f, 0.7f};
        std::vector<gfx::parity::Scene> out;
        gfx::parity::Scene scene;
        scene.kind = gfx::parity::SceneKind::Custom;
        scene.name = "terrain_height_blend";
        scene.hlsl = [](u32 width, u32 height) {
            std::string h = kBlendHlsl;
            h += "float4 VSMain(uint id : SV_VertexID) : SV_Position {\n"
                 "    float2 uv = float2((id << 1) & 2, id & 2);\n"
                 "    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
                 "}\n"
                 "float4 PSMain(float4 pos : SV_Position) : SV_Target {\n"
                 "    float u = pos.x / " + std::to_string(width) + ".0;\n"
                 "    float v = pos.y / " + std::to_string(height) + ".0;\n"
                 "    float4 painted = float4(u, 1.0 - u, v * 0.5, 0.0);\n"
                 "    float4 heights = float4(0.2, 0.9, 0.5, 0.7);\n"
                 "    float4 w = AetherBlendTerrainHeights(painted, heights, v);\n"
                 "    return float4(w.x, w.y, w.z, 1.0);\n"
                 "}\n";
            return h;
        };
        scene.reference = [kHeights](u32 width, u32 height) {
            std::vector<u8> pixels(static_cast<usize>(width) * height * 4);
            for (u32 y = 0; y < height; ++y) {
                for (u32 x = 0; x < width; ++x) {
                    const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(width), v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(height);
                    const std::array<f32, 4> w = BlendTerrainLayerHeights({u, 1.0f - u, v * 0.5f, 0.0f}, {kHeights[0], kHeights[1], kHeights[2], kHeights[3]}, v);
                    u8* p = &pixels[(static_cast<usize>(y) * width + x) * 4];
                    p[0] = ToByte(w[0]), p[1] = ToByte(w[1]), p[2] = ToByte(w[2]), p[3] = 255;
                }
            }
            return pixels;
        };
        out.push_back(std::move(scene));
        return out;
    }();
    return scenes;
}

} // namespace aether::terrain
