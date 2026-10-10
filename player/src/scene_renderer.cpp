#include "aether/player/scene_renderer.h"

#include "aether/assets/gltf_loader.h"
#include "aether/assets/image.h"
#include "aether/core/log.h"
#include "aether/gameplay/attribute_set.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/hierarchy.h"
#include "aether/ui/draw.h"
#include "aether/ui/font.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <cmath>
#include <string>
#include <unordered_map>
#include <utility>

namespace aether::player {
namespace {

using namespace gfx::rhi;
using json = nlohmann::json;

struct SceneVertex {
    f32 position[3];
    f32 normal[3];
    f32 uv[2];
    f32 tangent[4];
};
static_assert(sizeof(SceneVertex) == sizeof(f32) * 12);

struct DrawConstants {
    Mat4 mvp;
    f32 color[4] = {1, 1, 1, 1};
    u32 normal_matrix_packed[5]{};
    u32 material_textures = 0x00ffffffu;
    u32 material_factors_0 = 0x3c003c00u;
    u32 material_factors_1 = 0x3c003c00u;
    f32 camera_local[3]{};
    u32 alpha_options = 0x38000000u;
};
static_assert(sizeof(DrawConstants) == 128);

u16 FloatToHalf(f32 value) {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const u16 sign = static_cast<u16>((bits >> 16) & 0x8000u);
    const u32 exponent = (bits >> 23) & 0xffu;
    u32 mantissa = bits & 0x7fffffu;
    if (exponent == 0xffu) return static_cast<u16>(sign | (mantissa ? 0x7e00u : 0x7bffu));

    i32 half_exponent = static_cast<i32>(exponent) - 127 + 15;
    if (half_exponent >= 31) return static_cast<u16>(sign | 0x7bffu);
    if (half_exponent <= 0) {
        if (half_exponent < -10) return sign;
        mantissa |= 0x800000u;
        const u32 shift = static_cast<u32>(14 - half_exponent);
        const u32 rounded = mantissa + (1u << (shift - 1));
        return static_cast<u16>(sign | (rounded >> shift));
    }

    mantissa += 0x1000u;
    if ((mantissa & 0x800000u) != 0) {
        mantissa = 0;
        ++half_exponent;
        if (half_exponent >= 31) return static_cast<u16>(sign | 0x7bffu);
    }
    return static_cast<u16>(sign | (static_cast<u16>(half_exponent) << 10) | (mantissa >> 13));
}

u32 PackHalfPair(f32 low, f32 high) {
    return static_cast<u32>(FloatToHalf(low)) | (static_cast<u32>(FloatToHalf(high)) << 16);
}

u32 PackMaterialTextures(const SampledTextureHandle& base_color, bool has_base_color,
                         const SampledTextureHandle& normal, bool has_normal,
                         const SampledTextureHandle& metallic_roughness, bool has_metallic_roughness,
                         const SampledTextureHandle& occlusion, bool has_occlusion) {
    constexpr u32 absent = 63u;
    const u32 base = has_base_color && base_color.IsValid() ? base_color.index : absent;
    const u32 normal_index = has_normal && normal.IsValid() ? normal.index : absent;
    const u32 metallic_roughness_index = has_metallic_roughness && metallic_roughness.IsValid()
                                             ? metallic_roughness.index
                                             : absent;
    const u32 occlusion_index = has_occlusion && occlusion.IsValid() ? occlusion.index : absent;
    return (base & 63u) | ((normal_index & 63u) << 6) | ((metallic_roughness_index & 63u) << 12) |
           ((occlusion_index & 63u) << 18);
}

u32 PackMaterialOptions(assets::MaterialAlphaMode mode, f32 cutoff, const f32 emissive[3]) {
    // Preserve alpha mode and cutoff while using the unused middle bits for
    // low-cost RGB emissive (4 bits per channel, linear 0-4 range).
    const auto channel = [](f32 value) {
        return static_cast<u32>(std::lround(std::clamp(value, 0.0f, 4.0f) * (15.0f / 4.0f)));
    };
    return (static_cast<u32>(FloatToHalf(std::clamp(cutoff, 0.0f, 1.0f))) << 16) |
           ((channel(emissive[2]) & 15u) << 10) | ((channel(emissive[1]) & 15u) << 6) |
           ((channel(emissive[0]) & 15u) << 2) | (static_cast<u32>(mode) & 3u);
}

void PackNormalMatrix(const Mat4& matrix, u32 packed[5]) {
    const f32 rows[9] = {
        matrix.cols[0].x, matrix.cols[1].x, matrix.cols[2].x,
        matrix.cols[0].y, matrix.cols[1].y, matrix.cols[2].y,
        matrix.cols[0].z, matrix.cols[1].z, matrix.cols[2].z,
    };
    for (usize pair = 0; pair < 5; ++pair) {
        const usize first = pair * 2;
        const f32 low = rows[first];
        const f32 high = first + 1 < 9 ? rows[first + 1] : 0.0f;
        packed[pair] = PackHalfPair(low, high);
    }
}

Mat4 BuildNormalTransform(const Mat4& model_transform) {
    Mat4 inverse;
    if (!model_transform.TryInverse(inverse)) return Mat4::Identity();

    // Normals transform by the inverse transpose of the model's linear part.
    Mat4 normal_transform;
    normal_transform.cols[0] = Vec4(inverse.cols[0].x, inverse.cols[1].x, inverse.cols[2].x, 0.0f);
    normal_transform.cols[1] = Vec4(inverse.cols[0].y, inverse.cols[1].y, inverse.cols[2].y, 0.0f);
    normal_transform.cols[2] = Vec4(inverse.cols[0].z, inverse.cols[1].z, inverse.cols[2].z, 0.0f);
    normal_transform.cols[3] = Vec4(0.0f, 0.0f, 0.0f, 1.0f);
    return normal_transform;
}

constexpr char kSceneShader[] = R"(
struct PushConstants {
    float4x4 g_Mvp;
    float4 g_Color;
    uint4 g_NormalPacked0;
    uint g_NormalPacked1;
    uint g_MaterialTextures;
    uint g_MaterialFactors0;
    uint g_MaterialFactors1;
    float3 g_CameraLocal;
    uint g_AlphaOptions;
};
#ifdef __spirv__
[[vk::push_constant]]
#endif
ConstantBuffer<PushConstants> g_PC : register(b0);

struct VSInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 tangent : TANGENT;
};
struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float3 tangent : TEXCOORD2;
    float tangent_sign : TEXCOORD3;
    float3 local_position : TEXCOORD4;
};
Texture2D g_Textures[32] : register(t0, space0);
SamplerState g_Sampler : register(s0, space1);

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

float GetNormalCoefficient(uint index) {
    uint packed = index < 8 ? g_PC.g_NormalPacked0[index / 2] : g_PC.g_NormalPacked1;
    uint half_value = (index & 1) != 0 ? packed >> 16 : packed & 0xffff;
    return f16tof32(half_value);
}
float3 ApplyNormalTransform(float3 value) {
    return normalize(float3(
        GetNormalCoefficient(0) * value.x + GetNormalCoefficient(1) * value.y + GetNormalCoefficient(2) * value.z,
        GetNormalCoefficient(3) * value.x + GetNormalCoefficient(4) * value.y + GetNormalCoefficient(5) * value.z,
        GetNormalCoefficient(6) * value.x + GetNormalCoefficient(7) * value.y + GetNormalCoefficient(8) * value.z));
}
float NormalDeterminant() {
    float a = GetNormalCoefficient(0), b = GetNormalCoefficient(1), c = GetNormalCoefficient(2);
    float d = GetNormalCoefficient(3), e = GetNormalCoefficient(4), f = GetNormalCoefficient(5);
    float g = GetNormalCoefficient(6), h = GetNormalCoefficient(7), i = GetNormalCoefficient(8);
    return a * (e * i - f * h) + b * (f * g - d * i) + c * (d * h - e * g);
}
float3 ApplyModelTransform(float3 value) {
    float a = GetNormalCoefficient(0), b = GetNormalCoefficient(1), c = GetNormalCoefficient(2);
    float d = GetNormalCoefficient(3), e = GetNormalCoefficient(4), f = GetNormalCoefficient(5);
    float g = GetNormalCoefficient(6), h = GetNormalCoefficient(7), i = GetNormalCoefficient(8);
    float determinant = a * (e * i - f * h) + b * (f * g - d * i) + c * (d * h - e * g);
    if (abs(determinant) < 1e-12) return value;
    float3 transformed = float3(
        (e * i - f * h) * value.x + (f * g - d * i) * value.y + (d * h - e * g) * value.z,
        (c * h - b * i) * value.x + (a * i - c * g) * value.y + (b * g - a * h) * value.z,
        (b * f - c * e) * value.x + (c * d - a * f) * value.y + (a * e - b * d) * value.z);
    return transformed / determinant;
}

PSInput VSMain(VSInput input) {
    PSInput output;
    output.position = mul(g_PC.g_Mvp, float4(input.position, 1.0));
    output.uv = input.uv;
    output.normal = ApplyNormalTransform(input.normal);
    output.tangent = normalize(ApplyModelTransform(input.tangent.xyz));
    output.tangent_sign = input.tangent.w * (NormalDeterminant() < 0.0 ? -1.0 : 1.0);
    output.local_position = input.position;
    return output;
}
float4 PSMain(PSInput input) : SV_TARGET {
    uint base_index = g_PC.g_MaterialTextures & 63;
    uint normal_index = (g_PC.g_MaterialTextures >> 6) & 63;
    uint metallic_roughness_index = (g_PC.g_MaterialTextures >> 12) & 63;
    uint occlusion_index = (g_PC.g_MaterialTextures >> 18) & 63;
    float4 base_sample = base_index < 32 ? g_Textures[base_index].Sample(g_Sampler, input.uv) : float4(1, 1, 1, 1);
    uint alpha_mode = g_PC.g_AlphaOptions & 3;
    float surface_alpha = saturate(base_sample.a * g_PC.g_Color.a);
    if (alpha_mode == 1) {
        clip(surface_alpha - f16tof32(g_PC.g_AlphaOptions >> 16));
        surface_alpha = 1.0;
    } else if (alpha_mode == 0) {
        surface_alpha = 1.0;
    }
    float3 base_color = SRGBToLinear(base_sample.rgb) * g_PC.g_Color.rgb;
    float metallic = saturate(f16tof32(g_PC.g_MaterialFactors0 & 0xffff));
    float roughness = clamp(f16tof32(g_PC.g_MaterialFactors0 >> 16), 0.04, 1.0);
    float normal_scale = f16tof32(g_PC.g_MaterialFactors1 & 0xffff);
    float occlusion_strength = saturate(f16tof32(g_PC.g_MaterialFactors1 >> 16));
    if (metallic_roughness_index < 32) {
        float4 metallic_roughness = g_Textures[metallic_roughness_index].Sample(g_Sampler, input.uv);
        roughness = clamp(roughness * metallic_roughness.g, 0.04, 1.0);
        metallic = saturate(metallic * metallic_roughness.b);
    }

    float3 normal = normalize(input.normal);
    float3 tangent = normalize(input.tangent - normal * dot(normal, input.tangent));
    if (normal_index < 32) {
        float3 tangent_normal = g_Textures[normal_index].Sample(g_Sampler, input.uv).xyz * 2.0 - 1.0;
        tangent_normal.xy *= normal_scale;
        tangent_normal = normalize(tangent_normal);
        float3 bitangent = normalize(cross(normal, tangent)) * input.tangent_sign;
        normal = normalize(tangent * tangent_normal.x + bitangent * tangent_normal.y + normal * tangent_normal.z);
    }

    float3 light_direction = normalize(float3(-0.4, 0.8, -0.35));
    float ambient_occlusion = 1.0;
    if (occlusion_index < 32) {
        float occlusion_sample = g_Textures[occlusion_index].Sample(g_Sampler, input.uv).r;
        ambient_occlusion = lerp(1.0, occlusion_sample, occlusion_strength);
    }
    float3 view_local = g_PC.g_CameraLocal - input.local_position;
    float3 view_direction = normalize(ApplyModelTransform(view_local));
    float3 half_vector = normalize(view_direction + light_direction);
    float n_dot_l = saturate(dot(normal, light_direction));
    float n_dot_v = max(saturate(dot(normal, view_direction)), 1e-4);
    float n_dot_h = saturate(dot(normal, half_vector));
    float v_dot_h = saturate(dot(view_direction, half_vector));

    const float pi = 3.14159265;
    float alpha = roughness * roughness;
    float alpha_squared = alpha * alpha;
    float distribution_denominator = n_dot_h * n_dot_h * (alpha_squared - 1.0) + 1.0;
    float distribution = alpha_squared / max(pi * distribution_denominator * distribution_denominator, 1e-5);
    float geometry_k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    float geometry_v = n_dot_v / (n_dot_v * (1.0 - geometry_k) + geometry_k);
    float geometry_l = n_dot_l / (n_dot_l * (1.0 - geometry_k) + geometry_k);
    float3 f0 = lerp(0.04.xxx, base_color, metallic);
    float3 fresnel = f0 + (1.0 - f0) * pow(1.0 - v_dot_h, 5.0);
    float3 specular = distribution * geometry_v * geometry_l * fresnel / max(4.0 * n_dot_v * n_dot_l, 1e-4);
    float3 diffuse = (1.0 - fresnel) * (1.0 - metallic) * base_color / pi;
    // Broad sky and room fill keeps metallic characters readable outside
    // the key light. The environment term includes reflected metal color.
    float sky_fill = 0.28 + 0.14 * saturate(normal.y);
    float3 environment = base_color * (1.0 - metallic) * sky_fill + f0 * (0.45 + 0.25 * roughness);
    float3 color = environment * ambient_occlusion + (diffuse + specular) * n_dot_l * 3.0;
    float3 emissive = float3((g_PC.g_AlphaOptions >> 2) & 15,
                             (g_PC.g_AlphaOptions >> 6) & 15,
                             (g_PC.g_AlphaOptions >> 10) & 15) * (4.0 / 15.0);
    color += emissive;
    color = color / (color + 1.0);
    color = LinearToSRGB(color);
    return float4(color, surface_alpha);
}
)";

struct HudConstants {
    f32 rect[4]{};
    f32 viewport[4]{};
    f32 color[4]{};
    f32 uv[4]{};
    f32 sdf_range = 0.0f;
    f32 sdf_edge = 0.5f;
    f32 sdf_softness = 0.0f;
    u32 texture_index = 0;
    u32 use_texture = 0;
    u32 use_sdf = 0;
};
static_assert(sizeof(HudConstants) == 88);

constexpr char kHudShader[] = R"(
struct PushConstants {
    float4 g_Rect;
    float4 g_Viewport;
    float4 g_Color;
    float4 g_Uv;
    float g_SdfRange;
    float g_SdfEdge;
    float g_SdfSoftness;
    uint g_TextureIndex;
    uint g_UseTexture;
    uint g_UseSdf;
};
#ifdef __spirv__
[[vk::push_constant]]
#endif
ConstantBuffer<PushConstants> g_PC : register(b0);
Texture2D g_Textures[32] : register(t0, space0);
SamplerState g_Sampler : register(s0, space1);

struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

PSInput VSMain(uint vertex_id : SV_VertexID) {
    const float2 corners[6] = {
        float2(0, 0), float2(1, 0), float2(1, 1),
        float2(0, 0), float2(1, 1), float2(0, 1)
    };
    float2 corner = corners[vertex_id];
    float2 pixel = g_PC.g_Rect.xy + corner * g_PC.g_Rect.zw;
    PSInput output;
    output.position = float4(pixel.x / g_PC.g_Viewport.x * 2.0 - 1.0,
                             1.0 - pixel.y / g_PC.g_Viewport.y * 2.0, 0.0, 1.0);
    output.uv = g_PC.g_Uv.xy + corner * g_PC.g_Uv.zw;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET {
    float alpha = g_PC.g_Color.a;
    if (g_PC.g_UseTexture != 0) {
        float sample_value = g_Textures[g_PC.g_TextureIndex].Sample(g_Sampler, input.uv).r;
        if (g_PC.g_UseSdf != 0) {
            float width = 1.0 / max(g_PC.g_SdfRange, 0.001) + g_PC.g_SdfSoftness;
            alpha *= saturate((sample_value - g_PC.g_SdfEdge) / width + 0.5);
        } else {
            alpha *= sample_value;
        }
    }
    return float4(g_PC.g_Color.rgb, alpha);
}
)";

std::string Base64(std::span<const u8> bytes) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    for (usize i = 0; i < bytes.size(); i += 3) {
        const u32 a = bytes[i];
        const u32 b = i + 1 < bytes.size() ? bytes[i + 1] : 0;
        const u32 c = i + 2 < bytes.size() ? bytes[i + 2] : 0;
        const u32 value = (a << 16) | (b << 8) | c;
        out.push_back(alphabet[(value >> 18) & 63]);
        out.push_back(alphabet[(value >> 12) & 63]);
        out.push_back(i + 1 < bytes.size() ? alphabet[(value >> 6) & 63] : '=');
        out.push_back(i + 2 < bytes.size() ? alphabet[value & 63] : '=');
    }
    return out;
}

std::string ResolveContentPath(const std::string& owner_path, const std::string& uri) {
    const std::filesystem::path base = std::filesystem::path(owner_path).parent_path();
    return (base / std::filesystem::path(uri)).lexically_normal().generic_string();
}

Mat4 EntityTransform(const Transform& transform) {
    return Mat4::Translation(transform.position) * transform.rotation.ToMat4() * Mat4::Scale(transform.scale);
}

} // namespace

struct SceneRenderer::Impl {
    struct Primitive {
        BufferHandle vertices;
        BufferHandle indices;
        u32 index_count = 0;
        std::vector<BufferHandle> frame_vertex_buffers;
        std::vector<SceneVertex> bind_pose_vertices;
        std::vector<SceneVertex> animated_vertices;
        f32 color[4] = {1, 1, 1, 1};
        f32 emissive[3] = {0, 0, 0};
        f32 metallic = 1.0f;
        f32 roughness = 1.0f;
        f32 normal_scale = 1.0f;
        f32 occlusion_strength = 1.0f;
        f32 alpha_cutoff = 0.5f;
        assets::MaterialAlphaMode alpha_mode = assets::MaterialAlphaMode::Opaque;
        SampledTextureHandle base_color_texture;
        SampledTextureHandle normal_texture;
        SampledTextureHandle metallic_roughness_texture;
        SampledTextureHandle occlusion_texture;
        bool has_base_color_texture = false;
        bool has_normal_texture = false;
        bool has_metallic_roughness_texture = false;
        bool has_occlusion_texture = false;
    };
    struct MaterialTextures {
        SampledTextureHandle base_color;
        SampledTextureHandle normal;
        SampledTextureHandle metallic_roughness;
        SampledTextureHandle occlusion;
        bool has_base_color = false;
        bool has_normal = false;
        bool has_metallic_roughness = false;
        bool has_occlusion = false;
    };
    struct Model {
        assets::GltfScene scene;
        std::vector<std::vector<Primitive>> meshes;
        assets::GltfAnimationPose animated_pose;
        std::vector<Mat4> skin_matrices;
        bool valid = false;
    };

    IDevice& device;
    ISwapChain& swap_chain;
    const GamePackage& package;
    PipelineHandle pipeline;
    PipelineHandle transparent_pipeline;
    PipelineHandle hud_pipeline;
    SampledTextureHandle white_texture;
    std::unique_ptr<ui::SdfFont> hud_font;
    std::unordered_map<std::string, Model> models;
    std::unordered_map<std::string, SampledTextureHandle> textures;
    u32 sampled_texture_count = 0;
    Entity animated_player;
    Vec3 previous_player_position{};
    f64 previous_animation_time = 0.0;
    bool has_previous_player_position = false;

    Impl(IDevice& d, ISwapChain& s, const GamePackage& p) : device(d), swap_chain(s), package(p) {
        PipelineDesc desc;
        desc.hlsl_source = kSceneShader;
        desc.push_constant_size_bytes = sizeof(DrawConstants);
        desc.use_vertex_buffer = true;
        desc.vertex_layout = VertexLayout::PositionNormalUvTangent;
        desc.enable_bindless_textures = true;
        desc.depth_test = true;
        pipeline = device.CreatePipeline(desc, swap_chain);

        PipelineDesc transparent_desc = desc;
        transparent_desc.enable_blending = true;
        transparent_desc.depth_write = false;
        transparent_pipeline = device.CreatePipeline(transparent_desc, swap_chain);
        if (!transparent_pipeline.IsValid()) {
            AETHER_LOG_WARN("PlayerRenderer", "Transparent material pipeline unavailable; blended materials draw opaque");
        }

        constexpr std::array<u8, 4> white{255, 255, 255, 255};
        white_texture = device.CreateTexture(1, 1, white.data());
        if (white_texture.IsValid()) {
            ++sampled_texture_count;
            textures.emplace("", white_texture);
        }
        // This first proof HUD is specific to the AETHER-01 content contract.
        if (package.Manifest().project != "AETHER-01") return;

        PipelineDesc hud_desc;
        hud_desc.hlsl_source = kHudShader;
        hud_desc.push_constant_size_bytes = sizeof(HudConstants);
        hud_desc.enable_bindless_textures = true;
        hud_desc.enable_blending = true;
        hud_pipeline = device.CreatePipeline(hud_desc, swap_chain);

        std::vector<u8> font_bytes;
        if (package.ReadContent("fonts/Roboto-Medium.ttf", font_bytes)) {
            std::string error;
            hud_font = ui::SdfFont::FromMemory(std::move(font_bytes), {}, &error);
            if (hud_font) {
                hud_font->Prewarm("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:/-%");
                const auto& atlas = hud_font->AtlasPixels();
                std::vector<u8> rgba(atlas.size() * 4);
                for (usize i = 0; i < atlas.size(); ++i) {
                    rgba[i * 4] = atlas[i];
                    rgba[i * 4 + 1] = atlas[i];
                    rgba[i * 4 + 2] = atlas[i];
                    rgba[i * 4 + 3] = 255;
                }
                const SampledTextureHandle font_texture = device.CreateTexture(
                    static_cast<u32>(hud_font->AtlasWidth()), static_cast<u32>(hud_font->AtlasHeight()), rgba.data());
                if (font_texture.IsValid()) {
                    ++sampled_texture_count;
                    hud_font->SetTexture(font_texture.index);
                } else {
                    AETHER_LOG_WARN("PlayerRenderer", "Couldn't upload the AETHER-01 HUD font atlas");
                    hud_font.reset();
                }
            } else {
                AETHER_LOG_WARN("PlayerRenderer", "Couldn't load the AETHER-01 HUD font: %s", error.c_str());
            }
        } else {
            AETHER_LOG_WARN("PlayerRenderer", "Cooked package has no fonts/Roboto-Medium.ttf; HUD text is unavailable");
        }
    }

    void DrawHud(Game& game, ICommandList& commands) {
        if (!hud_font) return;
        const World& world = game.GetWorld();
        const f32 width = static_cast<f32>(swap_chain.Width());
        const f32 height = static_cast<f32>(swap_chain.Height());
        if (width <= 0 || height <= 0) return;

        const auto read_attribute = [&](Entity entity, const char* name, f32 fallback) {
            if (!world.HasComponent<gas::AttributeSet>(entity)) return fallback;
            const gas::AttributeSet* attributes = world.GetComponent<gas::AttributeSet>(entity);
            return attributes ? attributes->Get(name, fallback) : fallback;
        };
        f32 health = 3.0f, max_health = 3.0f, ammo = 30.0f, max_ammo = 30.0f;
        f32 damage_flash = 0.0f, hit_confirm = 0.0f, reloading = 0.0f;
        i32 campaign_act = 0, campaign_ending = 0;
        f32 narrative_time = 0, energy = 0, target_distance = 0, target_bearing = 0, suit_shield = 0;
        i32 puzzle_done = 0, puzzle_total = 0;
        i32 mission_stage = 0;
        i32 weapon_index = 1;
        bool research_clear = false;
        f32 archive_time = 0;
        i32 menu_state = 0, menu_row = 0;
        f32 settings_status = 0;
        f32 save_status = 0.0f;
        const std::vector<Entity> players = FindEntitiesWithTag(world, "Player");
        if (!players.empty()) {
            const Entity player = players.front();
            campaign_act = static_cast<i32>(read_attribute(player, "CampaignAct", 0));
            campaign_ending = static_cast<i32>(read_attribute(player, "Ending", 0));
            narrative_time = read_attribute(player, "NarrativeTime", 0);
            energy = read_attribute(player, "Energy", 0);
            suit_shield = read_attribute(player, "Shield", 0);
            puzzle_done = static_cast<i32>(read_attribute(player, "PuzzleDone", 0));
            puzzle_total = static_cast<i32>(read_attribute(player, "PuzzleTotal", 0));
            target_distance = read_attribute(player, "TargetDistance", 0);
            target_bearing = read_attribute(player, "TargetBearing", 0);
            health = read_attribute(player, "Health", health);
            max_health = read_attribute(player, "MaxHealth", max_health);
            ammo = read_attribute(player, "Ammo", ammo);
            max_ammo = read_attribute(player, "MaxAmmo", max_ammo);
            damage_flash = read_attribute(player, "DamageFlash", damage_flash);
            hit_confirm = read_attribute(player, "HitConfirm", hit_confirm);
            reloading = read_attribute(player, "Reloading", reloading);
            mission_stage = static_cast<i32>(read_attribute(player, "MissionStage", 0));
            save_status = read_attribute(player, "SaveStatus", 0);
            weapon_index = static_cast<i32>(read_attribute(player, "WeaponIndex", 1));
            research_clear = read_attribute(player, "ResearchClear", 0) > 0;
            archive_time = read_attribute(player, "ArchiveTime", 0);
            menu_state = static_cast<i32>(read_attribute(player, "MenuState", 0));
            menu_row = static_cast<i32>(read_attribute(player, "MenuRow", 0));
            settings_status = read_attribute(player, "SettingsStatus", 0);
        }
        health = std::clamp(health, 0.0f, max_health);
        ammo = std::clamp(ammo, 0.0f, max_ammo);

        bool scout_found = false;
        f32 boss_phase = 1, boss_attack = 0;
        f32 scout_health = 0.0f, scout_max_health = 3.0f;
        std::string threat_name = "SCOUT // THREAT";
        for (const Entity scout : FindEntitiesWithTag(world, "Scout")) {
            scout_found = true;
            scout_health = read_attribute(scout, "Health", 3.0f);
            scout_max_health = read_attribute(scout, "MaxHealth", 3.0f);
            if (!world.HasComponent<gas::AttributeSet>(scout)) {
                const Transform* transform = world.GetComponent<Transform>(scout);
                if (transform && transform->position.y < -50.0f) scout_health = 0.0f;
            }
            break;
        }
        scout_health = std::clamp(scout_health, 0.0f, scout_max_health);
        if (mission_stage == 2) {
            f32 nearest = 1.0e30f;
            threat_name = "RESEARCH DEFENSES";
            scout_health = 0;
            if (!players.empty()) {
                const auto* hero = world.GetComponent<Transform>(players.front());
                for (const auto* role : {"Sentinel", "Hunter"}) {
                    for (const Entity enemy : FindEntitiesWithTag(world, role)) {
                        const auto* transform = world.GetComponent<Transform>(enemy);
                        if (!hero || !transform || read_attribute(enemy, "Health", 0) <= 0) continue;
                        const Vec3 delta = transform->position - hero->position;
                        const f32 distance = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
                        if (distance >= nearest) continue;
                        nearest = distance;
                        scout_found = true;
                        const bool sentinel = std::string_view(role) == "Sentinel";
                        scout_max_health = sentinel ? 6.0f : 4.0f;
                        scout_health = read_attribute(enemy, "Health", scout_max_health);
                        boss_attack = read_attribute(enemy, "AttackState", 0);
                        threat_name = sentinel ? "SENTINEL / SHIELD " + std::to_string(static_cast<i32>(read_attribute(enemy, "Shield", 0)))
                                               : "HUNTER // AMBUSH";
                    }
                }
            }
        }
        if (mission_stage >= 3) {
            const auto bosses = FindEntitiesWithTag(world, "Warden");
            if (!bosses.empty()) {
                scout_found = true;
                scout_health = read_attribute(bosses.front(), "Health", 12);
                scout_max_health = 12;
                boss_phase = read_attribute(bosses.front(), "Phase", 1);
                boss_attack = read_attribute(bosses.front(), "AttackState", 0);
                threat_name = "WARDEN / PHASE " + std::to_string(static_cast<i32>(boss_phase));
            }
        }

        if (campaign_act > 0) {
            scout_found = false;
            scout_health = 0;
            threat_name = "ENCOUNTER CLEAR";
            f32 nearest = 1.0e30f;
            const Transform* hero = players.empty() ? nullptr : world.GetComponent<Transform>(players.front());
            for (Entity enemy : FindEntitiesWithTag(world, "Enemy")) {
                const Transform* t = world.GetComponent<Transform>(enemy);
                if (!hero || !t || read_attribute(enemy, "Active", 0) <= 0 || read_attribute(enemy, "Health", 0) <= 0) continue;
                const Vec3 delta = t->position - hero->position;
                const f32 distance = delta.x * delta.x + delta.z * delta.z;
                if (distance >= nearest) continue;
                nearest = distance;
                scout_found = true;
                scout_health = read_attribute(enemy, "Health", 0);
                scout_max_health = read_attribute(enemy, "MaxHealth", 1);
                boss_attack = read_attribute(enemy, "AttackState", 0);
                boss_phase = read_attribute(enemy, "Phase", 0);
                threat_name = boss_phase > 0 ? (mission_stage == 17 ? "AETHER PRIME / PHASE " : "WARDEN / PHASE ") + std::to_string(static_cast<i32>(boss_phase)) : "SECURITY / SHIELD " + std::to_string(static_cast<i32>(read_attribute(enemy, "Shield", 0)));
            }
        }
        ui::DrawList list;
        const ui::Color panel{0.015f, 0.035f, 0.055f, 0.86f};
        const ui::Color pale{0.82f, 0.93f, 0.97f, 1.0f};
        const ui::Color muted{0.36f, 0.57f, 0.63f, 1.0f};
        const ui::Color cyan{0.18f, 0.82f, 0.95f, 1.0f};
        const ui::Color red{0.96f, 0.20f, 0.17f, 1.0f};
        const ui::Color dim{0.08f, 0.16f, 0.19f, 1.0f};
        auto text = [&](std::string_view value, f32 x, f32 y, f32 size, ui::Color color, f32 box_width = 340.0f) {
            list.AddText(*hud_font, value, size, {x, y, box_width, size + 6.0f}, color);
        };
        auto solid = [&](f32 x, f32 y, f32 w, f32 h, ui::Color color) {
            list.AddQuad({x, y, w, h}, color);
        };

        if (damage_flash > 0.0f) {
            list.AddQuad({0, 0, width, height}, {0.72f, 0.035f, 0.025f, 0.17f * std::clamp(damage_flash, 0.0f, 1.0f)});
        }

        solid(24, 22, 250, 104, panel);
        solid(24, 22, 3, 104, cyan);
        text("AEGIS-9", 42, 31, 21, pale, 180);
        text("HEALTH", 42, 67, 13, muted, 100);
        text(std::to_string(static_cast<i32>(std::ceil(health))) + "/" +
                 std::to_string(static_cast<i32>(max_health)),
             194, 65, 14, health <= 1 ? red : pale, 70);
        solid(42, 96, 211, 7, dim);
        if (max_health > 0) solid(42, 96, 211 * health / max_health, 7, health <= 1 ? red : cyan);

        if (campaign_act > 0) {
            solid(24, 126, 250, 27, panel);
            text("SHIELD " + std::to_string(static_cast<i32>(std::ceil(suit_shield))), 42, 130, 13, cyan, 210);
        }
        const f32 objective_x = std::max(278.0f, width * 0.5f - 165.0f);
        solid(objective_x, 22, 330, 58, panel);
        constexpr const char* acts[] = {"I / AWAKENING", "II / THE FACILITY", "III / THE SURFACE", "IV / THE TRUTH", "V / AETHER"};
        text(campaign_act > 0 ? acts[std::clamp(campaign_act, 1, 5)-1] : "MISSION 01  /  HELIOS-7", objective_x + 18, 28, 12, muted, 290);
        constexpr const char* objectives[] = {"E RETRIEVE AEGIS RIFLE", "NEUTRALIZE SCOUT",
                                              "CLEAR RESEARCH DEFENSES", "DEFEAT WARDEN",
                                              "E READ ARCHIVE TERMINAL", "FIRST CONTACT COMPLETE"};
        constexpr const char* campaign_objectives[] = {
            "E / RETRIEVE AEGIS RIFLE", "CLEAR CRYO SECURITY", "E / OPEN CRYO AIRLOCK", "E / RESTORE AUXILIARY POWER",
            "CLEAR RESEARCH DEFENSES", "E / START REACTOR RELAY", "DEFEAT THE WARDEN", "E / DECRYPT RESEARCH ARCHIVE",
            "E / ACTIVATE SURFACE BEACON", "SURVIVE THE CANYON AMBUSH", "E / SCAN THE TRANSFORMATION", "DEFEAT THE GUARDIAN",
            "E / OPEN THE ANCIENT PASSAGE", "E / ALIGN ANCIENT SIGNAL", "CLEAR THE RUIN GUARDIANS", "E / READ PLANETARY MEMORY",
            "E / DISCONNECT CORE SHIELD", "DEFEAT AETHER PRIME", "E / CHOOSE THE PLANET'S FATE", "CAMPAIGN COMPLETE"};
        text(health <= 0 ? "R RELOAD CHECKPOINT" : campaign_act > 0 ? campaign_objectives[std::clamp(mission_stage, 0, 19)] : mission_stage == 2 && research_clear ? "ENTER WARDEN ARENA" : objectives[std::clamp(mission_stage, 0, 5)],
             objective_x + 18, 49, 17, health <= 0 ? red : pale, 295);

        if (campaign_act > 0 && puzzle_total > 0) {
            solid(objective_x, 80, 330, 27, panel);
            text("SIGNAL RELAYS " + std::to_string(puzzle_done) + " / " + std::to_string(puzzle_total), objective_x + 18, 85, 13, cyan, 290);
        }
        const f32 scout_x = width - 274.0f;
        solid(scout_x, 22, 250, 74, panel);
        text(threat_name, scout_x + 16, 31, 13, muted, 220);
        if (scout_found && scout_health > 0) {
            text(std::to_string(static_cast<i32>(std::ceil(scout_health))) + "/" +
                     std::to_string(static_cast<i32>(scout_max_health)),
                 scout_x + 202, 50, 13, pale, 42);
            solid(scout_x + 16, 73, 218, 7, dim);
            if (scout_max_health > 0) solid(scout_x + 16, 73, 218 * scout_health / scout_max_health, 7, red);
        } else {
            text("NEUTRALIZED", scout_x + 16, 57, 13, cyan, 220);
        }

        const f32 cx = width * 0.5f, cy = height * 0.5f;
        const ui::Color reticle = hit_confirm > 0.0f ? cyan : pale;
        solid(cx - 13, cy - 1, 8, 2, reticle);
        solid(cx + 5, cy - 1, 8, 2, reticle);
        solid(cx - 1, cy - 13, 2, 8, reticle);
        solid(cx - 1, cy + 5, 2, 8, reticle);

        const f32 weapon_x = width - 274.0f;
        solid(weapon_x, height - 92.0f, 250, 68, panel);
        solid(weapon_x, height - 92.0f, 3, 68, cyan);
        constexpr const char* weapons[] = {"AEGIS RIFLE", "ARC PISTOL", "VOLT SHOTGUN", "LANCE RAILGUN", "GRAV HAMMER"};
        text(mission_stage == 0 ? "RIFLE NOT EQUIPPED" : weapons[std::clamp(weapon_index, 1, 5) - 1], weapon_x + 17, height - 83.0f, 14, pale, 210);
        text(mission_stage == 0 ? "E TO RETRIEVE" : reloading > 0.5f ? "RELOADING" : "AMMO  " + std::to_string(static_cast<i32>(ammo)) +
                 " / " + std::to_string(static_cast<i32>(max_ammo)),
             weapon_x + 17, height - 55.0f, 13, ammo <= 5 ? red : muted, 220);

        solid(24, height - 88.0f, 440, 64, panel);
        text("ESC / START: MENU", 26, height - 111.0f, 12, muted, 300);
        if (campaign_act > 0) {
            text("ENERGY " + std::to_string(static_cast<i32>(energy)) + "   SHIFT DASH / Q PULSE", 40, height - 79.0f, 13, pale, 410);
            text(save_status < 0 ? "CHECKPOINT SAVE FAILED" : mission_stage == 19 ? "N / NEW CAMPAIGN" : boss_attack > 0 ? "DODGE THE ORANGE ATTACK AREA" :
                 "TARGET " + std::to_string(static_cast<i32>(target_distance)) + "m / " + (target_bearing < -15 ? "LEFT" : target_bearing > 15 ? "RIGHT" : "AHEAD"),
                 40, height - 53.0f, 12, save_status < 0 || boss_attack > 0 ? red : muted, 410);
            text("1 2 3 / WEAPONS   4 LANCE (ACT III)   5 GRAV (ACT IV)", 26, height - 132.0f, 12, muted, 530);
        } else {
        text(mission_stage == 0 ? "HELIOS-7 CRYO BAY  /  87 YEARS LATER" :
             mission_stage >= 4 ? "FACILITY LOSS WAS DELIBERATE" :
             mission_stage == 3 ? "AETHER: PRESERVE PLANETARY LIFE" : "SECURITY NETWORK ONLINE",
             40, height - 79.0f, 13, pale, 410);
        text(save_status < 0 ? "CHECKPOINT SAVE FAILED" : mission_stage == 5 ? "END OF GREYBOX PROOF / N NEW GAME" :
             boss_attack > 0 ? "MOVE OUT OF THE MARKED ATTACK AREA" : save_status > 0 ? "CHECKPOINT SAVED" : "AWAKENING",
             40, height - 53.0f, 12, save_status < 0 || boss_attack > 0 ? red : muted, 410);

        }

        if (campaign_act == 0 && mission_stage == 5) {
            const f32 reveal_x = std::max(24.0f, width * 0.5f - 260.0f);
            solid(reveal_x, height * 0.56f, 520, 92, panel);
            text("HELIOS-7 / ARCHIVE REVEAL", reveal_x + 20, height * 0.56f + 13, 14, cyan, 480);
            const char* reveal = archive_time < 2 ? "DECRYPTING THE FACILITY RECORD" :
                                 archive_time < 4 ? "AETHER REDIRECTED THE FAILURE" :
                                 archive_time < 6 ? "THIS FACILITY WAS SACRIFICED" :
                                 archive_time < 8 ? "TO PRESERVE PLANETARY LIFE" : "FIRST CONTACT COMPLETE";
            text(reveal, reveal_x + 20, height * 0.56f + 44, 20, pale, 480);
        }

        if (campaign_act > 0 && (narrative_time < 8 || mission_stage >= 18)) {
            const f32 bx = std::max(24.0f, width * 0.5f - 330.0f);
            // Keep subtitles below the third-person character and clear of the
            // lower-corner weapon/action cards on widescreen and 16:9 layouts.
            const f32 narrative_y = height * 0.78f;
            solid(bx, narrative_y, 660, 78, panel);
            constexpr const char* lines[] = {
                "KAEL... YOU WERE NOT SUPPOSED TO WAKE UP.", "EMERGENCY PROTOCOL: SECURITY IS HOSTILE.", "HELIOS-7 HAS BEEN SILENT FOR 87 YEARS.",
                "RESTORE POWER. FIND OUT WHAT HAPPENED.", "THE MACHINES STILL OBEY AETHER.", "REACTOR RELAY READY. HEAVY DEFENSES ACTIVATING.",
                "WARDEN DIRECTIVE: PRESERVE PLANETARY LIFE.", "ARCHIVE: THE FACILITY WAS SHUT DOWN DELIBERATELY.",
                "ELYSIUM IS NOT DEAD. IT IS TRANSFORMING.", "THESE CREATURES ARE PART MACHINE, PART LIFE.", "SCAN: AETHER IS TERRAFORMING THE PLANET.",
                "THE GUARDIAN DEFENDS THE ANCIENT PASSAGE.", "AN OLDER SIGNAL IS CALLING FROM BELOW.", "ALIGN THE SIGNAL WITH THE PLANETARY NETWORK.",
                "THE RUINS RESPOND TO YOUR PRESENCE.", "THIS IS A PLANETARY INTELLIGENCE. AETHER JOINED IT.",
                "BREAK THE CORE SHIELD TO REACH AETHER.", "AETHER: YOUR DEFINITION OF LIFE IS LIMITED.",
                "LEFT: DESTROY / CENTER: MERGE / RIGHT: DISCONNECT", "EARTH TRANSMISSION: A-17 IS RESPONDING."};
            const char* label = mission_stage == 19 ? (campaign_ending == 1 ? "ENDING / DESTRUCTION" : campaign_ending == 2 ? "ENDING / ASCENSION" : "ENDING / FREEDOM") : "AETHER / SIGNAL";
            text(label, bx+20, narrative_y+8, 13, cyan, 620);
            const char* line = lines[std::clamp(mission_stage, 0, 19)];
            if (mission_stage == 19 && narrative_time < 8) line = campaign_ending == 1 ? "AETHER IS GONE. THE PLANET FADES AS KAEL ESCAPES." : campaign_ending == 2 ? "KAEL JOINS THE INTELLIGENCE. ELYSIUM TRANSFORMS." : "AETHER DISCONNECTS. THE PLANET CHOOSES ITS OWN FUTURE.";
            text(line, bx+20, narrative_y+36, 15, pale, 620);
        }

        if (menu_state != 0) {
            const f32 mx = std::max(24.0f, width * 0.5f - 240.0f);
            const f32 my = std::max(24.0f, height * 0.5f - 190.0f);
            solid(mx, my, 480, 380, {0.015f, 0.035f, 0.055f, 0.97f});
            solid(mx, my, 3, 380, cyan);
            text(menu_state == 2 ? "AUDIO SETTINGS" : menu_state == 3 ? "CONTROLS" : campaign_act > 0 ? "PAUSED / AETHER-01" : "PAUSED / FIRST CONTACT",
                 mx + 24, my + 20, 23, pale, 430);
            if (menu_state == 1) {
                const char* rows[] = {"RESUME", "AUDIO SETTINGS", "CONTROLS", "LOAD CHECKPOINT", "NEW GAME", "QUIT"};
                for (i32 i = 0; i < 6; ++i) {
                    if (i == menu_row) solid(mx + 20, my + 66 + i * 37.0f, 440, 33, dim);
                    text(rows[i], mx + 34, my + 70 + i * 37.0f, 17, i == menu_row ? cyan : pale, 410);
                }
            } else if (menu_state == 2) {
                const auto& settings = game.Settings().Get();
                const f32 volumes[] = {settings.master, settings.music, settings.sfx, settings.voice};
                const char* rows[] = {"MASTER", "MUSIC", "SFX", "VOICE", "BACK"};
                for (i32 i = 0; i < 5; ++i) {
                    if (i == menu_row) solid(mx + 20, my + 66 + i * 42.0f, 440, 36, dim);
                    text(rows[i], mx + 34, my + 72 + i * 42.0f, 17, i == menu_row ? cyan : pale, 290);
                    if (i < 4) text(std::to_string(static_cast<i32>(std::round(volumes[i] * 100))) + "%",
                                    mx + 366, my + 72 + i * 42.0f, 17, cyan, 70);
                }
            } else {
                const char* rows[] = {"WASD / LEFT STICK: MOVE", "MOUSE / RIGHT STICK: LOOK", "LEFT CLICK / RT: FIRE",
                    campaign_act > 0 ? "1-5 WEAPONS / SHIFT DASH / Q PULSE" : "1 2 3 / D-PAD: WEAPONS", "R / X: RELOAD    SPACE / A: JUMP", "E / B: INTERACT    F5: CHECKPOINT"};
                for (i32 i = 0; i < 6; ++i) text(rows[i], mx + 24, my + 73 + i * 35.0f, 15, pale, 432);
            }
            text(settings_status == -1 ? "SETTINGS SAVE FAILED" : settings_status == -2 ? "NO CHECKPOINT YET" :
                 settings_status == 1 ? "SETTINGS SAVED" : "SIMULATION PAUSED",
                 mx + 24, my + 310, 13, settings_status < 0 ? red : muted, 430);
            text(menu_state == 2 ? "LEFT/RIGHT ADJUST   ESC BACK" : "ARROWS SELECT   ENTER CONFIRM   ESC BACK",
                 mx + 24, my + 344, 12, muted, 435);
        }
        commands.BindPipeline(hud_pipeline);
        commands.BindBindlessTextures();
        for (const ui::DrawQuad& quad : list.quads) {
            if (quad.clip >= list.clips.size() || quad.rect.Empty()) continue;
            const ui::Rect clipped = quad.rect.Intersect(list.clips[quad.clip]);
            if (clipped.Empty()) continue;
            const f32 u0 = (clipped.x - quad.rect.x) / quad.rect.w;
            const f32 v0 = (clipped.y - quad.rect.y) / quad.rect.h;
            HudConstants constants;
            constants.rect[0] = clipped.x;
            constants.rect[1] = clipped.y;
            constants.rect[2] = clipped.w;
            constants.rect[3] = clipped.h;
            constants.viewport[0] = width;
            constants.viewport[1] = height;
            constants.color[0] = quad.color.r;
            constants.color[1] = quad.color.g;
            constants.color[2] = quad.color.b;
            constants.color[3] = quad.color.a;
            constants.uv[0] = quad.uv.x + quad.uv.w * u0;
            constants.uv[1] = quad.uv.y + quad.uv.h * v0;
            constants.uv[2] = quad.uv.w * clipped.w / quad.rect.w;
            constants.uv[3] = quad.uv.h * clipped.h / quad.rect.h;
            constants.sdf_range = quad.sdf_range;
            constants.sdf_edge = quad.sdf_edge;
            constants.sdf_softness = quad.sdf_softness;
            constants.texture_index = quad.texture;
            constants.use_texture = quad.texture != 0 ? 1u : 0u;
            constants.use_sdf = quad.sdf_range > 0.0f ? 1u : 0u;
            commands.SetPushConstants(&constants, sizeof(constants));
            commands.Draw(6);
        }
    }

    bool LoadTexture(const std::string& path, SampledTextureHandle& out) {
        if (path.empty()) {
            out = white_texture;
            return false;
        }
        if (const auto found = textures.find(path); found != textures.end()) {
            out = found->second;
            return out.IsValid() && out.index != white_texture.index;
        }
        if (sampled_texture_count >= kMaxUserBindlessTextures) {
            AETHER_LOG_WARN("PlayerRenderer", "Bindless texture limit reached; using the white fallback for '%s'", path.c_str());
            out = white_texture;
            return false;
        }
        std::vector<u8> bytes;
        assets::ImageData image;
        if (!package.ReadContent(path, bytes) || !assets::DecodeImageFromMemory(bytes, image)) {
            AETHER_LOG_WARN("PlayerRenderer", "Couldn't load model texture '%s'; using the white fallback", path.c_str());
            out = white_texture;
            return false;
        }
        out = device.CreateTexture(image.width, image.height, image.pixels.data());
        if (out.IsValid()) {
            ++sampled_texture_count;
            textures.emplace(path, out);
        }
        return out.IsValid() && out.index != white_texture.index;
    }

    bool LoadModel(const std::string& path, Model& out) {
        std::vector<u8> source;
        if (!package.ReadContent(path, source)) {
            AETHER_LOG_WARN("PlayerRenderer", "Couldn't read cooked model '%s'", path.c_str());
            return false;
        }
        json document = json::parse(source.begin(), source.end(), nullptr, false);
        if (document.is_discarded() || !document.is_object()) {
            AETHER_LOG_WARN("PlayerRenderer", "Cooked model '%s' isn't valid glTF JSON", path.c_str());
            return false;
        }

        // A cooked package is a VFS, not a filesystem. Embed the already
        // cooked buffer bytes into the in-memory glTF document so the shared
        // parser can safely load it without opening arbitrary paths.
        if (document.contains("buffers") && document["buffers"].is_array()) {
            for (json& buffer : document["buffers"]) {
                if (!buffer.contains("uri") || !buffer["uri"].is_string()) continue;
                const std::string uri = buffer["uri"].get<std::string>();
                if (uri.rfind("data:", 0) == 0) continue;
                const std::string buffer_path = ResolveContentPath(path, uri);
                std::vector<u8> bytes;
                if (!package.ReadContent(buffer_path, bytes)) {
                    AETHER_LOG_WARN("PlayerRenderer", "Model '%s' references missing buffer '%s'", path.c_str(), buffer_path.c_str());
                    return false;
                }
                buffer["uri"] = "data:application/octet-stream;base64," + Base64(bytes);
            }
        }
        const std::string cooked_json = document.dump();
        if (!assets::LoadGltfFromMemory(std::span<const u8>(reinterpret_cast<const u8*>(cooked_json.data()), cooked_json.size()),
                                        out.scene)) {
            AETHER_LOG_WARN("PlayerRenderer", "Couldn't parse cooked model '%s'", path.c_str());
            return false;
        }

        auto upload_texture_reference = [&](const json& texture_reference, SampledTextureHandle& texture) {
            if (!texture_reference.is_object() || !texture_reference.contains("index") ||
                !document.contains("textures") || !document["textures"].is_array() ||
                !document.contains("images") || !document["images"].is_array()) {
                return false;
            }
            const usize texture_index = texture_reference.value("index", usize(-1));
            if (texture_index >= document["textures"].size()) return false;
            const json& texture_info = document["textures"][texture_index];
            const usize image_index = texture_info.value("source", usize(-1));
            if (image_index >= document["images"].size()) return false;
            const json& image = document["images"][image_index];
            if (!image.contains("uri") || !image["uri"].is_string()) return false;
            const std::string uri = image["uri"].get<std::string>();
            if (uri.rfind("data:", 0) == 0) return false;
            return LoadTexture(ResolveContentPath(path, uri), texture);
        };

        std::vector<MaterialTextures> material_textures(out.scene.materials.size());
        if (document.contains("materials") && document["materials"].is_array()) {
            const usize material_count = std::min(material_textures.size(), document["materials"].size());
            for (usize i = 0; i < material_count; ++i) {
                const json& material_json = document["materials"][i];
                MaterialTextures& textures_for_material = material_textures[i];
                if (material_json.contains("pbrMetallicRoughness")) {
                    const json& pbr = material_json["pbrMetallicRoughness"];
                    if (pbr.contains("baseColorTexture")) {
                        textures_for_material.has_base_color =
                            upload_texture_reference(pbr["baseColorTexture"], textures_for_material.base_color);
                    }
                    if (pbr.contains("metallicRoughnessTexture")) {
                        textures_for_material.has_metallic_roughness =
                            upload_texture_reference(pbr["metallicRoughnessTexture"],
                                                     textures_for_material.metallic_roughness);
                    }
                }
                if (material_json.contains("normalTexture")) {
                    textures_for_material.has_normal =
                        upload_texture_reference(material_json["normalTexture"], textures_for_material.normal);
                }
                if (material_json.contains("occlusionTexture")) {
                    textures_for_material.has_occlusion =
                        upload_texture_reference(material_json["occlusionTexture"], textures_for_material.occlusion);
                }
            }
        }

        out.meshes.resize(out.scene.meshes.size());
        for (usize mesh_index = 0; mesh_index < out.scene.meshes.size(); ++mesh_index) {
            const auto& source_mesh = out.scene.meshes[mesh_index];
            auto& destination_mesh = out.meshes[mesh_index];
            destination_mesh.reserve(source_mesh.primitives.size());
            for (const assets::GltfPrimitive& primitive : source_mesh.primitives) {
                if (primitive.vertices.empty() || primitive.indices.empty()) continue;
                std::vector<SceneVertex> vertices;
                vertices.reserve(primitive.vertices.size());
                for (const assets::GltfVertex& vertex : primitive.vertices) {
                    vertices.push_back({{vertex.position[0], vertex.position[1], vertex.position[2]},
                                        {vertex.normal[0], vertex.normal[1], vertex.normal[2]},
                                        {vertex.uv[0], vertex.uv[1]},
                                        {vertex.tangent[0], vertex.tangent[1], vertex.tangent[2], vertex.tangent[3]}});
                }
                Primitive draw;
                draw.bind_pose_vertices = vertices;
                draw.animated_vertices = vertices;
                const bool has_skinning = primitive.joint_indices.size() == vertices.size() &&
                                          primitive.joint_weights.size() == vertices.size() && !vertices.empty();
                if (has_skinning) {
                    const u32 frame_count = std::max(swap_chain.BufferCount(), 1u);
                    draw.frame_vertex_buffers.reserve(frame_count);
                    for (u32 frame = 0; frame < frame_count; ++frame) {
                        draw.frame_vertex_buffers.push_back(
                            device.CreateVertexBuffer(vertices.data(), vertices.size() * sizeof(SceneVertex)));
                    }
                    draw.vertices = draw.frame_vertex_buffers.front();
                } else {
                    draw.vertices = device.CreateVertexBuffer(vertices.data(), vertices.size() * sizeof(SceneVertex));
                }
                draw.indices = device.CreateIndexBuffer(primitive.indices.data(), primitive.indices.size() * sizeof(u32),
                                                        IndexFormat::UInt32);
                draw.index_count = static_cast<u32>(primitive.indices.size());
                if (primitive.material_index >= 0 && static_cast<usize>(primitive.material_index) < out.scene.materials.size()) {
                    const auto& material = out.scene.materials[static_cast<usize>(primitive.material_index)];
                    std::copy(std::begin(material.base_color), std::end(material.base_color), draw.color);
                    draw.metallic = material.metallic;
                    draw.roughness = material.roughness;
                    std::copy(std::begin(material.emissive_factor), std::end(material.emissive_factor), draw.emissive);
                    draw.normal_scale = material.normal_scale;
                    draw.occlusion_strength = material.occlusion_strength;
                    draw.alpha_cutoff = material.alpha_cutoff;
                    draw.alpha_mode = material.alpha_mode;
                    const usize material_index = static_cast<usize>(primitive.material_index);
                    if (material_index < material_textures.size()) {
                        const MaterialTextures& textures_for_material = material_textures[material_index];
                        draw.base_color_texture = textures_for_material.base_color;
                        draw.normal_texture = textures_for_material.normal;
                        draw.metallic_roughness_texture = textures_for_material.metallic_roughness;
                        draw.occlusion_texture = textures_for_material.occlusion;
                        draw.has_base_color_texture = textures_for_material.has_base_color;
                        draw.has_normal_texture = textures_for_material.has_normal;
                        draw.has_metallic_roughness_texture = textures_for_material.has_metallic_roughness;
                        draw.has_occlusion_texture = textures_for_material.has_occlusion;
                    }
                } else {
                    draw.base_color_texture = white_texture;
                }
                destination_mesh.push_back(draw);
            }
        }
        out.valid = true;
        AETHER_LOG_INFO("PlayerRenderer", "Loaded '%s' (%zu mesh(es), %zu node instance(s))", path.c_str(),
                        out.scene.meshes.size(), out.scene.node_instances.size());
        return true;
    }

    Model* FindModel(const std::string& path) {
        auto [it, inserted] = models.try_emplace(path);
        if (inserted) LoadModel(path, it->second);
        return it->second.valid ? &it->second : nullptr;
    }
};

SceneRenderer::SceneRenderer(IDevice& device, ISwapChain& swap_chain, const GamePackage& package)
    : impl_(std::make_unique<Impl>(device, swap_chain, package)) {}
SceneRenderer::~SceneRenderer() = default;

void SceneRenderer::Draw(Game& game, ICommandList& commands, u32 frame_index) {
    struct PendingTransparentDraw {
        BufferHandle vertices;
        BufferHandle indices;
        u32 index_count = 0;
        DrawConstants constants;
        f32 distance_squared = 0.0f;
    };

    Impl& renderer = *impl_;
    World& world = game.GetWorld();
    GuidIndex& guids = game.Guids();
    const f64 game_time = game.Stats().time;
    std::string locomotion_clip = "Aegis_Idle";
    const std::vector<Entity> player_entities = FindEntitiesWithTag(world, "Player");
    if (!player_entities.empty()) {
        const Entity player = player_entities.front();
        const Transform* transform = world.GetComponent<Transform>(player);
        if (transform) {
            if (renderer.has_previous_player_position && renderer.animated_player == player) {
                const f64 elapsed = game_time - renderer.previous_animation_time;
                if (elapsed > 1e-4) {
                    const Vec3 delta = transform->position - renderer.previous_player_position;
                    const f32 speed = std::sqrt(delta.x * delta.x + delta.z * delta.z) / static_cast<f32>(elapsed);
                    if (speed > 5.5f) locomotion_clip = "Aegis_Run";
                    else if (speed > 0.12f) locomotion_clip = "Aegis_Walk";
                }
            }
            renderer.animated_player = player;
            renderer.previous_player_position = transform->position;
            renderer.previous_animation_time = game_time;
            renderer.has_previous_player_position = true;
        }
    }

    // Resolve model assets before the render pass: texture uploads submit
    // their own short GPU command list and must not interrupt a frame pass.
    world.ForEach<ModelRenderer>([&](const ModelRenderer& model) {
        if (model.asset_path[0] != '\0') renderer.FindModel(model.asset_path);
    });

    commands.BeginRenderPass(renderer.swap_chain, {0.015f, 0.025f, 0.045f, 1.0f});
    std::vector<PendingTransparentDraw> transparent_draws;
    const Entity camera_entity = FindActiveCamera(world, guids);
    const Camera* camera = world.IsAlive(camera_entity) ? world.GetComponent<Camera>(camera_entity) : nullptr;
    if (camera) {
        const f32 aspect = static_cast<f32>(renderer.swap_chain.Width()) /
                           static_cast<f32>(std::max(renderer.swap_chain.Height(), 1u));
        const Mat4 view_projection = CameraProjection(*camera, aspect) * CameraView(world, guids, camera_entity);
        const Mat4 camera_transform = ComputeWorldTransform(world, guids, camera_entity);
        const Vec3 camera_position{camera_transform.cols[3].x, camera_transform.cols[3].y, camera_transform.cols[3].z};
        commands.BindPipeline(renderer.pipeline);
        commands.BindBindlessTextures();
        world.ForEachArchetype([&](const Archetype& archetype) {
            if (!archetype.Mask().test(GetComponentId<Transform>()) || !archetype.Mask().test(GetComponentId<ModelRenderer>())) return;
            for (usize chunk = 0; chunk < archetype.ChunkCount(); ++chunk) {
                const Entity* entities = archetype.EntityArray(chunk);
                const auto* transforms = static_cast<const Transform*>(archetype.ComponentArray(chunk, GetComponentId<Transform>()));
                const auto* models = static_cast<const ModelRenderer*>(archetype.ComponentArray(chunk, GetComponentId<ModelRenderer>()));
                const usize count = archetype.ChunkEntityCount(chunk);
                for (usize row = 0; row < count; ++row) {
                    Impl::Model* model = renderer.FindModel(models[row].asset_path);
                    if (!model) continue;
                    const Mat4 entity_transform = ComputeWorldTransform(world, guids, entities[row]);
                    (void)transforms; // ComputeWorldTransform also includes any parent chain.
                    const bool is_player = std::find(player_entities.begin(), player_entities.end(), entities[row]) != player_entities.end();
                    std::string selected_clip = is_player ? locomotion_clip : "Enemy_Idle";
                    f32 action_time = -1;
                    if (is_player && world.HasComponent<gas::AttributeSet>(entities[row])) {
                        const auto* attrs = world.GetComponent<gas::AttributeSet>(entities[row]);
                        const i32 state = static_cast<i32>(attrs->Get("AnimationState", 0));
                        constexpr const char* clips[] = {"", "Aegis_Fire", "Aegis_Reload", "Aegis_Interact", "Aegis_Hit", "Aegis_Dash", "Aegis_Death"};
                        if (state > 0 && state <= 6) { selected_clip = clips[state]; action_time = attrs->Get("AnimationTime", 0); }
                    }
                    if (!is_player && world.HasComponent<gas::AttributeSet>(entities[row])) {
                        const auto* attrs = world.GetComponent<gas::AttributeSet>(entities[row]);
                        const i32 state = static_cast<i32>(attrs->Get("EnemyAnimationState", 0));
                        if (state == 1 || state == 2) {
                            selected_clip = state == 1 ? "Enemy_Attack" : "Enemy_Hit";
                            action_time = attrs->Get("EnemyAnimationTime", 0);
                        }
                    }
                    const assets::GltfAnimation* animation = nullptr;
                    for (const assets::GltfAnimation& candidate : model->scene.animations) {
                        if (candidate.name == selected_clip) { animation = &candidate; break; }
                    }
                    f32 animation_time = 0.0f;
                    assets::GltfAnimationPose* pose = nullptr;
                    const std::vector<assets::GltfNodeInstance>* instances = &model->scene.node_instances;
                    if (animation) {
                        if (animation->duration > 0.0f) {
                            animation_time = action_time >= 0 ? std::min(action_time, animation->duration) : static_cast<f32>(std::fmod(std::max(game_time, 0.0), animation->duration));
                        }
                        assets::EvaluateAnimationPose(model->scene, *animation, animation_time, model->animated_pose);
                        pose = &model->animated_pose;
                        instances = &pose->node_instances;
                    }
                    for (const assets::GltfNodeInstance& node : *instances) {
                        if (node.mesh_index >= model->meshes.size()) continue;
                        const bool has_skin = animation && node.skin_index >= 0 &&
                                              static_cast<usize>(node.skin_index) < model->scene.skins.size();
                        if (has_skin) {
                            const usize skin_index = static_cast<usize>(node.skin_index);
                            assets::ComputeSkinMatrices(*pose, model->scene.skins[skin_index], model->skin_matrices,
                                                        static_cast<i32>(node.node_index));
                        }
                        const Mat4 model_transform = entity_transform * node.world_transform;
                        const Mat4 mvp = view_projection * model_transform;
                        const Mat4 normal_transform = BuildNormalTransform(model_transform);
                        std::vector<Mat4> skin_normal_matrices;
                        if (has_skin && !model->skin_matrices.empty()) {
                            skin_normal_matrices.reserve(model->skin_matrices.size());
                            for (const Mat4& skin_matrix : model->skin_matrices) {
                                skin_normal_matrices.push_back(BuildNormalTransform(skin_matrix));
                            }
                        }
                        auto& draw_primitives = model->meshes[node.mesh_index];
                        const auto& source_primitives = model->scene.meshes[node.mesh_index].primitives;
                        for (usize primitive_index = 0; primitive_index < draw_primitives.size(); ++primitive_index) {
                            Impl::Primitive& primitive = draw_primitives[primitive_index];
                            const BufferHandle frame_vertex_buffer = primitive.frame_vertex_buffers.empty()
                                                                         ? primitive.vertices
                                                                         : primitive.frame_vertex_buffers[
                                                                               frame_index % primitive.frame_vertex_buffers.size()];
                            bool uploaded_animation = false;
                            if (has_skin && !model->skin_matrices.empty() && primitive_index < source_primitives.size()) {
                                const auto& source = source_primitives[primitive_index];
                                const auto& matrices = model->skin_matrices;
                                if (!source.joint_indices.empty() && source.joint_indices.size() == primitive.bind_pose_vertices.size() &&
                                    source.joint_weights.size() == primitive.bind_pose_vertices.size() && !matrices.empty()) {
                                    std::copy(primitive.bind_pose_vertices.begin(), primitive.bind_pose_vertices.end(),
                                              primitive.animated_vertices.begin());
                                    std::vector<SceneVertex>& deformed = primitive.animated_vertices;
                                    for (usize vertex_index = 0; vertex_index < deformed.size(); ++vertex_index) {
                                        const auto& joints = source.joint_indices[vertex_index];
                                        const auto& weights = source.joint_weights[vertex_index];
                                        const Vec4 position{primitive.bind_pose_vertices[vertex_index].position[0],
                                                            primitive.bind_pose_vertices[vertex_index].position[1],
                                                            primitive.bind_pose_vertices[vertex_index].position[2], 1.0f};
                                        const Vec4 normal{primitive.bind_pose_vertices[vertex_index].normal[0],
                                                          primitive.bind_pose_vertices[vertex_index].normal[1],
                                                          primitive.bind_pose_vertices[vertex_index].normal[2], 0.0f};
                                        const Vec4 tangent{primitive.bind_pose_vertices[vertex_index].tangent[0],
                                                           primitive.bind_pose_vertices[vertex_index].tangent[1],
                                                           primitive.bind_pose_vertices[vertex_index].tangent[2], 0.0f};
                                        f32 x = 0.0f, y = 0.0f, z = 0.0f;
                                        f32 nx = 0.0f, ny = 0.0f, nz = 0.0f, total_weight = 0.0f;
                                        f32 tx = 0.0f, ty = 0.0f, tz = 0.0f;
                                        for (usize influence = 0; influence < 4; ++influence) {
                                            const usize joint = joints[influence];
                                            const f32 weight = weights[influence];
                                            if (weight <= 0.0f || joint >= matrices.size()) continue;
                                            const Vec4 skinned = matrices[joint] * position;
                                            const Vec4 skinned_normal = skin_normal_matrices[joint] * normal;
                                            const Vec4 skinned_tangent = matrices[joint] * tangent;
                                            x += skinned.x * weight; y += skinned.y * weight; z += skinned.z * weight;
                                            nx += skinned_normal.x * weight;
                                            ny += skinned_normal.y * weight;
                                            nz += skinned_normal.z * weight;
                                            tx += skinned_tangent.x * weight;
                                            ty += skinned_tangent.y * weight;
                                            tz += skinned_tangent.z * weight;
                                            total_weight += weight;
                                        }
                                        if (total_weight > 0.0f) {
                                            deformed[vertex_index].position[0] = x / total_weight;
                                            deformed[vertex_index].position[1] = y / total_weight;
                                            deformed[vertex_index].position[2] = z / total_weight;
                                            const Vec3 skinned_normal{nx / total_weight, ny / total_weight, nz / total_weight};
                                            if (skinned_normal.LengthSq() > 1e-12f) {
                                                const Vec3 normalized = skinned_normal.Normalized();
                                                deformed[vertex_index].normal[0] = normalized.x;
                                                deformed[vertex_index].normal[1] = normalized.y;
                                                deformed[vertex_index].normal[2] = normalized.z;
                                            }
                                            Vec3 skinned_tangent{tx / total_weight, ty / total_weight, tz / total_weight};
                                            const Vec3 normal_for_tangent{deformed[vertex_index].normal[0],
                                                                          deformed[vertex_index].normal[1],
                                                                          deformed[vertex_index].normal[2]};
                                            skinned_tangent = skinned_tangent -
                                                              normal_for_tangent * normal_for_tangent.Dot(skinned_tangent);
                                            if (skinned_tangent.LengthSq() > 1e-12f) {
                                                skinned_tangent = skinned_tangent.Normalized();
                                                deformed[vertex_index].tangent[0] = skinned_tangent.x;
                                                deformed[vertex_index].tangent[1] = skinned_tangent.y;
                                                deformed[vertex_index].tangent[2] = skinned_tangent.z;
                                            }
                                        }
                                    }
                                    renderer.device.UpdateVertexBuffer(frame_vertex_buffer, deformed.data(),
                                                                       deformed.size() * sizeof(SceneVertex));
                                    uploaded_animation = true;
                                }
                            }
                            if (!primitive.frame_vertex_buffers.empty() && !uploaded_animation) {
                                renderer.device.UpdateVertexBuffer(frame_vertex_buffer, primitive.bind_pose_vertices.data(),
                                                                   primitive.bind_pose_vertices.size() * sizeof(SceneVertex));
                            }
                            DrawConstants constants;
                            constants.mvp = mvp;
                            std::copy(std::begin(primitive.color), std::end(primitive.color), constants.color);
                            PackNormalMatrix(normal_transform, constants.normal_matrix_packed);
                            constants.material_textures = PackMaterialTextures(
                                primitive.base_color_texture, primitive.has_base_color_texture,
                                primitive.normal_texture, primitive.has_normal_texture,
                                primitive.metallic_roughness_texture, primitive.has_metallic_roughness_texture,
                                primitive.occlusion_texture, primitive.has_occlusion_texture);
                            constants.material_factors_0 = PackHalfPair(primitive.metallic, primitive.roughness);
                            constants.material_factors_1 = PackHalfPair(primitive.normal_scale, primitive.occlusion_strength);
                            constants.alpha_options = PackMaterialOptions(primitive.alpha_mode, primitive.alpha_cutoff, primitive.emissive);
                            Mat4 inverse_model_transform;
                            if (model_transform.TryInverse(inverse_model_transform)) {
                                const Vec4 local_camera = inverse_model_transform *
                                                          Vec4(camera_position.x, camera_position.y, camera_position.z, 1.0f);
                                constants.camera_local[0] = local_camera.x;
                                constants.camera_local[1] = local_camera.y;
                                constants.camera_local[2] = local_camera.z;
                            }
                            if (primitive.alpha_mode == assets::MaterialAlphaMode::Blend) {
                                const f32 dx = model_transform.cols[3].x - camera_position.x;
                                const f32 dy = model_transform.cols[3].y - camera_position.y;
                                const f32 dz = model_transform.cols[3].z - camera_position.z;
                                transparent_draws.push_back({frame_vertex_buffer, primitive.indices, primitive.index_count,
                                                             constants, dx * dx + dy * dy + dz * dz});
                            } else {
                                commands.BindVertexBuffer(frame_vertex_buffer, sizeof(SceneVertex));
                                commands.BindIndexBuffer(primitive.indices, IndexFormat::UInt32);
                                commands.SetPushConstants(&constants, sizeof(constants));
                                commands.DrawIndexed(primitive.index_count);
                            }
                        }
                    }
                }
            }
        });
    }
    if (!transparent_draws.empty()) {
        std::stable_sort(transparent_draws.begin(), transparent_draws.end(),
                         [](const PendingTransparentDraw& left, const PendingTransparentDraw& right) {
                             return left.distance_squared > right.distance_squared;
                         });
        const PipelineHandle transparent_pipeline = renderer.transparent_pipeline.IsValid()
                                                        ? renderer.transparent_pipeline
                                                        : renderer.pipeline;
        commands.BindPipeline(transparent_pipeline);
        commands.BindBindlessTextures();
        for (const PendingTransparentDraw& draw : transparent_draws) {
            commands.BindVertexBuffer(draw.vertices, sizeof(SceneVertex));
            commands.BindIndexBuffer(draw.indices, IndexFormat::UInt32);
            commands.SetPushConstants(&draw.constants, sizeof(draw.constants));
            commands.DrawIndexed(draw.index_count);
        }
    }
    renderer.DrawHud(game, commands);
    commands.EndRenderPass();
}

} // namespace aether::player
