#include "aether/player/scene_renderer.h"

#include "aether/renderer/material_codegen.h"
#include "aether/renderer/components.h"
#include "aether/renderer/decal.h"

#include "aether/assets/gltf_loader.h"
#include "aether/assets/skinning.h"
#include "aether/job/job_system.h"
#include "aether/assets/image.h"
#include "aether/core/log.h"
#include "aether/gameplay/attribute_set.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/hierarchy.h"
#include "frame_lights.h"
#include "model_glb.h"
#include "normal_transform.h"
#include "render_pose.h"
#include "particle_pass.h"
#include "decal_pass.h"
#include "post_pass.h"
#include "shadow_pass.h"
#include "sky_pass.h"
#include "taa_pass.h"
#include "virtual_shadow.h"
#include "vsm_pass.h"
#include "package_textures.h"
#include "aether/ui/draw.h"
#include "aether/ui/font.h"
#include "aether/ui/viewport.h"
#include "aether/vfx/particle_system.h"
#include "aether/sprite2d/batch.h"
#include "aether/sprite2d/lights2d.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace aether::player {
namespace {

using namespace gfx::rhi;
using json = nlohmann::json;

// The skinner (assets/skinning.h) and the player share one vertex layout.
using SceneVertex = assets::SkinVertex;
// Skinned primitives at least this big are skinned across the job system (see bench_skinning).
constexpr usize kParallelSkinVertices = 4096;
static_assert(sizeof(SceneVertex) == sizeof(f32) * 12);

f32 Dot4(const Vec4& a, const Vec4& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

struct DrawConstants {
    Mat4 mvp;
    // The base colour factor as four halves, then the emissive colour as three halves (r|g, b) with the emissive texture's index (63 =
    // none) in bits 16-23 of the second word: halves keep HDR emission (KHR_materials_emissive_strength) up to 65504.
    u32 color_packed[2] = {0x3c003c00u, 0x3c003c00u};
    u32 emissive_packed[2] = {0u, 255u << 16};
    u32 normal_matrix_packed[5]{};
    u32 material_textures = 0xffffffffu; // base colour, normal, metallic-roughness and occlusion indices, 8 bits each (255 = none)
    u32 material_factors_0 = 0x3c003c00u;
    u32 material_factors_1 = 0x3c003c00u;
    f32 camera_local[3]{};
    u32 alpha_options = 0x38000000u | (255u << 2); // mode (bits 0-1), the extension texture's index (2-9, 255 = none), cutoff half (16-31)
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

// A texture's 8-bit shader index: 255 (none) when absent, invalid or outside the bindless table.
u32 TextureSlot(const SampledTextureHandle& texture, bool present) {
    return present && texture.IsValid() && texture.index < kMaxBindlessTextures ? texture.index : 255u;
}

u32 PackMaterialTextures(const SampledTextureHandle& base_color, bool has_base_color,
                         const SampledTextureHandle& normal, bool has_normal,
                         const SampledTextureHandle& metallic_roughness, bool has_metallic_roughness,
                         const SampledTextureHandle& occlusion, bool has_occlusion) {
    return TextureSlot(base_color, has_base_color) | (TextureSlot(normal, has_normal) << 8) |
           (TextureSlot(metallic_roughness, has_metallic_roughness) << 16) | (TextureSlot(occlusion, has_occlusion) << 24);
}

// The determinant of a transform's upper 3x3: negative when it mirrors (turning its triangles' winding round).
f32 ModelDeterminant(const Mat4& m) {
    const Vec3 x(m.cols[0].x, m.cols[0].y, m.cols[0].z), y(m.cols[1].x, m.cols[1].y, m.cols[1].z), z(m.cols[2].x, m.cols[2].y, m.cols[2].z);
    return x.Dot(y.Cross(z));
}

u32 PackMaterialOptions(assets::MaterialAlphaMode mode, f32 cutoff, const SampledTextureHandle& extension, bool has_extension,
                        bool double_sided = false, bool water = false, u32 wind_level = 0, u32 wind_zone = 0, u32 detail_level = 0) {
    // The alpha mode in bits 0-1, the surface-extension texture's index in bits 2-9, double-sided in bit 10, water in bit 11, the wind strength in
    // bits 12-15, the cutoff as 5 bits in bits 16-20, the detail strength (0 to 7) in bits 21-23 and the wind zone (WindZoneBitsAt) in bits 24-31.
    return ((wind_zone & 255u) << 24) | ((detail_level & 7u) << 21) | (static_cast<u32>(std::lround(std::clamp(cutoff, 0.0f, 1.0f) * 31.0f)) << 16) | (TextureSlot(extension, has_extension) << 2) |
           (double_sided ? 1u << 10 : 0u) | (water ? 1u << 11 : 0u) | ((wind_level & 15u) << 12) | (static_cast<u32>(mode) & 3u);
}

void PackEmissive(const f32 emissive[3], const SampledTextureHandle& texture, bool has_texture, u32 packed[2]) {
    const auto channel = [](f32 value) { return std::isfinite(value) ? std::max(value, 0.0f) : 0.0f; };
    packed[0] = PackHalfPair(channel(emissive[0]), channel(emissive[1]));
    packed[1] = static_cast<u32>(FloatToHalf(channel(emissive[2]))) | (TextureSlot(texture, has_texture) << 16);
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

constexpr char kSceneShader[] = R"(
struct PushConstants {
    float4x4 g_Mvp;
    uint2 g_ColorPacked;
    uint2 g_Emissive;
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
Texture2D g_Textures[128] : register(t0, space0);
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
    uint packed = g_PC.g_NormalPacked1;
    if (index < 2) packed = g_PC.g_NormalPacked0.x;
    else if (index < 4) packed = g_PC.g_NormalPacked0.y;
    else if (index < 6) packed = g_PC.g_NormalPacked0.z;
    else if (index < 8) packed = g_PC.g_NormalPacked0.w;
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

)";
// The vertex shader, after the frame constants (so it can read the game's time) and before the rest of the frame-data code.
constexpr char kSceneShaderVertex[] = R"(
PSInput VSMain(VSInput input) {
    PSInput output;
    float3 position = input.position;
    // AETHER_materials_wind: the vegetation sways. The strength is four bits of the packed options (0.02 m of sway a step), the phase rides in the colour's alpha
    // lane (an opaque material does not use it: the renderer packs a phase from the instance's place in the world, so a field of grass does not sway as one),
    // and the weight grows with height above the object's base, so the roots stay put. Needs the frame constants for the time; without them nothing sways.
    uint wind_level = (g_PC.g_AlphaOptions >> 12) & 15;
#ifdef AETHER_FRAME_DATA
    if (wind_level != 0) {
        float phase = f16tof32(g_PC.g_ColorPacked.y >> 16);
        // An object inside a wind zone takes the zone's wind, blended with the world's by how far inside it stands (the packed zone: number plus one, then 5 bits).
        float2 wind_direction = g_Frame.wind.xy;
        float wind_strength = g_Frame.wind.z;
        float gust_speed = g_Frame.wind.w;
        uint zone_bits = g_PC.g_AlphaOptions >> 24;
        if ((zone_bits & 7) != 0) {
            float4 zone = g_Frame.wind_zones[(zone_bits & 7) - 1];
            float blend = float(zone_bits >> 3) / 31.0;
            float2 blended = lerp(g_Frame.wind.xy * g_Frame.wind.z, zone.xy * zone.z, blend);
            wind_strength = length(blended);
            wind_direction = wind_strength > 1e-5 ? blended / wind_strength : g_Frame.wind.xy;
            gust_speed = lerp(g_Frame.wind.w, zone.w, blend);
        }
        float t = g_Frame.time.x * gust_speed;
        float height = max(position.y, 0.0);
        float weight = height / (height + 0.3);
        // Two swells: a steady one, and a slower, uneven one that varies along the object so its parts do not move in step. Gustiness is the share of the second.
        float gust = sin(t * 1.9 + phase) * (1.0 - g_Frame.wind_gusts.x) + sin(t * 0.83 + phase * 1.7 + position.x * 2.0 + position.z * 1.3) * g_Frame.wind_gusts.x;
        // The wind blows in the world; the sway is added to a position in the object's own space, so the world direction goes through the inverse of the model's
        // linear part (the stored normal matrix is its inverse transpose, so the inverse is that matrix transposed). It also divides out the object's scale: a
        // metre of sway is a metre in the world however big the model is made.
        float3 world_sway = float3(wind_direction.x, 0.0, wind_direction.y) * (float(wind_level) * 0.02 * weight * gust * wind_strength);
        position += float3(GetNormalCoefficient(0) * world_sway.x + GetNormalCoefficient(3) * world_sway.y + GetNormalCoefficient(6) * world_sway.z,
                           GetNormalCoefficient(1) * world_sway.x + GetNormalCoefficient(4) * world_sway.y + GetNormalCoefficient(7) * world_sway.z,
                           GetNormalCoefficient(2) * world_sway.x + GetNormalCoefficient(5) * world_sway.y + GetNormalCoefficient(8) * world_sway.z);
    }
#endif
    output.position = mul(g_PC.g_Mvp, float4(position, 1.0));
    output.uv = input.uv;
    output.normal = ApplyNormalTransform(input.normal);
    output.tangent = normalize(ApplyModelTransform(input.tangent.xyz));
    output.tangent_sign = input.tangent.w * (NormalDeterminant() < 0.0 ? -1.0 : 1.0);
    output.local_position = input.position;
    return output;
}
#ifdef AETHER_FRAME_DATA
)";
// The frame read buffer: the clustered lights, then (virtual shadow maps) the page table, aligned to 16 bytes.
constexpr u64 kVsmTableOffset = (kLightClusterBufferBytes + 15) / 16 * 16;
constexpr u64 kLightBufferBytes = kVsmTableOffset + VsmPageAllocator::kTableEntries * sizeof(u32);

constexpr char kSceneShaderVsm[] = R"(

// The sun's shadow from the virtual shadow map's pages (player/src/virtual_shadow.h, vsm_pages.h): 1 lit, 0 shadowed, and -1 where no resident
// page covers the point, when the caller falls back to the cascades. Starts at the level whose texel matches the pixel's footprint and goes
// coarser while a page is missing. The filter is the cascades' (4x4 texel loads, tent weights); a tap on a neighbouring page looks that page up.
#define VSM_DEPTH_RANGE 4096.0

int2 VsmPageOfTexel(int2 texel) { return texel >> 7; } // VSM_PAGE_TEXELS is 128; a signed shift rounds toward minus infinity

// The atlas texel of a page's first texel, or -1 when the page is outside its level's window or not resident (a page table entry is 0).
int2 VsmPageOrigin(uint level, int2 page) {
    int4 window = g_Frame.vsm_window[level >> 1];
    int2 origin = (level & 1) != 0 ? window.zw : window.xy;
    int2 relative = page - origin;
    if (any(relative < 0) || any(relative >= 32)) return int2(-1, -1);
    uint entry = g_FrameLights.Load(g_Frame.vsm_info.z + ((((level * 32 + (uint)(page.y & 31)) * 32) + (uint)(page.x & 31)) << 2));
    if (entry == 0) return int2(-1, -1);
    uint physical = entry - 1;
    uint side = g_Frame.vsm_info.y;
    return int2((physical % side) * VSM_PAGE_TEXELS, (physical / side) * VSM_PAGE_TEXELS);
}

float VsmSunShadow(float3 world_position, float3 geometric_normal, float3 sun_direction) {
    uint atlas = g_Frame.vsm_info.x;
    if (atlas >= 128) return -1.0;
    float footprint = g_Frame.vsm_camera.w * length(world_position - g_Frame.vsm_camera.xyz) + g_Frame.vsm_params.x;
    float n_dot_l = saturate(dot(geometric_normal, sun_direction));
    float slope = sqrt(saturate(1.0 - n_dot_l * n_dot_l));
    for (uint level = VsmLevelForFootprint(footprint); level < VSM_LEVELS; ++level) {
        float texel = VsmTexel(level);
        float3 p = world_position + geometric_normal * (texel * (0.5 + 1.5 * slope));
        float2 position = float2(dot(p, g_Frame.vsm_light_right.xyz), dot(p, g_Frame.vsm_light_up.xyz)) / texel; // in texels
        float depth = (dot(p, g_Frame.vsm_light_direction.xyz) - texel) / VSM_DEPTH_RANGE + 0.5;                  // a texel nearer, against acne
        float2 tc = position - 0.5;
        float2 base = floor(tc);
        float2 f = tc - base;
        float wx[4] = {1.0 - f.x, 1.0, 1.0, f.x};
        float wy[4] = {1.0 - f.y, 1.0, 1.0, f.y};
        int2 first = int2(base) - int2(1, 1);
        int2 cached_page = VsmPageOfTexel(first);
        int2 cached_origin = VsmPageOrigin(level, cached_page);
        bool complete = cached_origin.x >= 0;
        float lit = 0.0;
        if (complete && all(cached_page == VsmPageOfTexel(first + int2(3, 3)))) {
            // The common case: all sixteen taps are in one page, so the page is looked up once.
            int2 inside = first - cached_page * VSM_PAGE_TEXELS;
            [unroll] for (int y = 0; y < 4; ++y) {
                [unroll] for (int x = 0; x < 4; ++x) {
                    int2 atlas_texel = cached_origin + int2(inside.x + x, VSM_PAGE_TEXELS - 1 - (inside.y + y));
                    float stored = g_Textures[atlas].Load(int3(atlas_texel, 0)).r;
                    lit += wx[x] * wy[y] * (depth <= stored ? 1.0 : 0.0);
                }
            }
            return lit / 9.0;
        }
        [unroll] for (int y = 0; y < 4; ++y) {
            [unroll] for (int x = 0; x < 4; ++x) {
                if (complete) {
                    int2 index = first + int2(x, y);
                    int2 page = VsmPageOfTexel(index);
                    if (any(page != cached_page)) {
                        cached_page = page;
                        cached_origin = VsmPageOrigin(level, page);
                    }
                    if (cached_origin.x < 0) {
                        complete = false;
                    } else {
                        // A page is drawn with v up, so its texel row is counted from the top: the last texel of v is row 0.
                        int2 inside = index - page * VSM_PAGE_TEXELS;
                        int2 atlas_texel = cached_origin + int2(inside.x, VSM_PAGE_TEXELS - 1 - inside.y);
                        float stored = g_Textures[atlas].Load(int3(atlas_texel, 0)).r;
                        lit += wx[x] * wy[y] * (depth <= stored ? 1.0 : 0.0);
                    }
                }
            }
        }
        if (complete) return lit / 9.0;
    }
    return -1.0;
}
)";

constexpr char kSceneShaderFrame[] = R"(

// The sun's shadow at a world position: 1 lit, 0 shadowed. The first cascade the point falls inside (two texels clear of its edge) is
// read with a 3x3 bilinear PCF of texel loads (4x4 texels, tent weights), so filtering needs no comparison sampler. The point is pushed
// out along the surface normal by about a texel first (normal offset), and compared a texel's depth nearer, against acne; the caster
// pipelines add a slope-scaled bias of their own. The last cascade fades out over its outer tenth instead of ending in a hard line.
#ifdef AETHER_RT_SHADOWS
// Ray-traced sun shadows (AETHER_RENDER_RT_SHADOWS=1): one inline ray from the surface toward the sun against the rigid casters' acceleration
// structure, 1 lit and 0 shadowed. The origin is pushed off the surface along its normal against self-hits. Hard-edged, and blind to what is not in
// the structure: skinned and alpha-tested casters.
RaytracingAccelerationStructure g_RtScene : register(t0, space4);
float RtSunShadow(float3 world_position, float3 geometric_normal, float3 sun_direction) {
    if (g_Frame.shadow_info.x >= 128) return 1.0;
    float n_dot_l = saturate(dot(geometric_normal, sun_direction));
    float slope = sqrt(saturate(1.0 - n_dot_l * n_dot_l));
    RayDesc ray;
    ray.Origin = world_position + geometric_normal * (0.01 + 0.03 * slope);
    ray.Direction = sun_direction;
    ray.TMin = 0.005;
    ray.TMax = 2000.0;
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(g_RtScene, 0, 0xFF, ray);
    query.Proceed();
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
}
#endif

float SunShadow(float3 world_position, float3 geometric_normal, float3 sun_direction) {
#ifdef AETHER_RT_SHADOWS
    return RtSunShadow(world_position, geometric_normal, sun_direction);
#endif
    float virtual_shadow = VsmSunShadow(world_position, geometric_normal, sun_direction);
    if (virtual_shadow >= 0.0) return virtual_shadow;
    uint atlas = g_Frame.shadow_info.x;
    uint count = min(g_Frame.shadow_info.y, 4u);
    if (atlas >= 128 || count == 0) return 1.0;
    float size = (float)g_Frame.shadow_info.z;
    float n_dot_l = saturate(dot(geometric_normal, sun_direction));
    float slope = sqrt(saturate(1.0 - n_dot_l * n_dot_l));
    for (uint i = 0; i < count; ++i) {
        float texel = g_Frame.shadow_cascade[i].z;
        float3 p = world_position + geometric_normal * (texel * (0.5 + 1.5 * slope));
        float4 clip = mul(g_Frame.shadow_view_projection[i], float4(p, 1.0));
        float2 uv = float2(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5);
        float margin = 2.0 / size;
        if (any(uv < margin) || any(uv > 1.0 - margin) || clip.z > 1.0) continue;
        float depth = clip.z - texel * g_Frame.shadow_cascade[i].w;
        float2 tc = uv * size - 0.5;
        float2 base = floor(tc);
        float2 f = tc - base;
        float wx[4] = {1.0 - f.x, 1.0, 1.0, f.x};
        float wy[4] = {1.0 - f.y, 1.0, 1.0, f.y};
        float2 origin = g_Frame.shadow_cascade[i].xy;
        float lit = 0.0;
        [unroll] for (int y = 0; y < 4; ++y) {
            [unroll] for (int x = 0; x < 4; ++x) {
                float2 t = clamp(base + float2(x - 1, y - 1), 0.0, size - 1.0) + origin;
                float stored = g_Textures[atlas].Load(int3(int2(t), 0)).r;
                lit += wx[x] * wy[y] * (depth <= stored ? 1.0 : 0.0);
            }
        }
        lit /= 9.0;
        if (i + 1 == count) {
            float edge = max(abs(clip.x), abs(clip.y));
            lit = lerp(1.0, lit, saturate((1.0 - edge) / 0.1));
        }
        return lit;
    }
    return 1.0;
}
#endif

// The surface's response to one light from direction `light_direction` (towards the light), before the light's own radiance: the
// diffuse and specular terms times N.L.
float3 SurfaceLight(float3 light_direction, float3 normal, float3 view_direction, float3 base_color, float metallic, float roughness, float3 f0) {
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
    float3 fresnel = f0 + (1.0 - f0) * pow(1.0 - v_dot_h, 5.0);
    float3 specular = distribution * geometry_v * geometry_l * fresnel / max(4.0 * n_dot_v * n_dot_l, 1e-4);
    float3 diffuse = (1.0 - fresnel) * (1.0 - metallic) * base_color / pi;
    return (diffuse + specular) * n_dot_l;
}

// KHR_materials_clearcoat, sheen and anisotropy (Phase 39.3), read from a 2x1 texture of packed factors when the material has them
// (assets::PackSurfaceExtension). Materials without it take the plain SurfaceLight path unchanged.
struct SurfaceExtension {
    float active;
    float clear_coat;
    float clear_coat_roughness;
    float3 sheen_color;
    float sheen_roughness;
    float anisotropy;
    float anisotropy_rotation;
    float hair;
    float hair_shift;
    float hair_secondary_shift;
    float3 hair_tint;
};
SurfaceExtension NoSurfaceExtension() {
    SurfaceExtension e;
    e.active = 0.0;
    e.clear_coat = 0.0;
    e.clear_coat_roughness = 0.0;
    e.sheen_color = float3(0.0, 0.0, 0.0);
    e.sheen_roughness = 0.0;
    e.anisotropy = 0.0;
    e.anisotropy_rotation = 0.0;
    e.hair = 0.0;
    e.hair_shift = 0.0;
    e.hair_secondary_shift = 0.0;
    e.hair_tint = float3(1.0, 1.0, 1.0);
    return e;
}
SurfaceExtension LoadSurfaceExtension(uint index) {
    float4 first = g_Textures[index].Load(int3(0, 0, 0));
    float4 second = g_Textures[index].Load(int3(1, 0, 0));
    SurfaceExtension e;
    e.active = 1.0;
    e.clear_coat = first.x;
    e.clear_coat_roughness = first.y;
    e.sheen_roughness = first.z;
    e.anisotropy = first.w;
    e.sheen_color = second.xyz;
    e.anisotropy_rotation = second.w;
    e.hair = 0.0;
    e.hair_shift = 0.0;
    e.hair_secondary_shift = 0.0;
    e.hair_tint = float3(1.0, 1.0, 1.0);
    uint extension_width, extension_height;
    g_Textures[index].GetDimensions(extension_width, extension_height);
    if (extension_width >= 4) { // a hair material: two more texels
        float4 third = g_Textures[index].Load(int3(2, 0, 0));
        float4 fourth = g_Textures[index].Load(int3(3, 0, 0));
        e.hair = 1.0;
        e.hair_shift = third.x - 0.5;
        e.hair_secondary_shift = third.y - 0.5;
        e.hair_tint = float3(third.z, third.w, fourth.x);
    }
    return e;
}

// SurfaceLight plus the extensions: anisotropy replaces the specular lobe, sheen adds a fibre lobe on top (and takes a little of the base's
// energy), and the clear coat covers the lot with its own specular and attenuates what is below it. T and B are the surface tangent frame.
float3 SurfaceLightEx(float3 light_direction, float3 normal, float3 view_direction, float3 base_color, float metallic, float roughness,
                      float3 f0, SurfaceExtension ext, float3 T, float3 B) {
    float3 lit = SurfaceLight(light_direction, normal, view_direction, base_color, metallic, roughness, f0);
    if (ext.active < 0.5) return lit;
    float3 half_vector = normalize(view_direction + light_direction);
    float n_dot_l = saturate(dot(normal, light_direction));
    float n_dot_v = max(saturate(dot(normal, view_direction)), 1e-4);
    float n_dot_h = saturate(dot(normal, half_vector));
    float l_dot_h = saturate(dot(light_direction, half_vector));
    float v_dot_h = saturate(dot(view_direction, half_vector));
    if (ext.hair > 0.5) {
        // Strands run along the tangent: Kajiya-Kay diffuse and a shifted two-lobe highlight replace the surface lobes.
        float3 strand_specular = AetherHairSpecular(float3(1.0, 1.0, 1.0), ext.hair_tint, T, normal, view_direction, light_direction,
                                                    ext.hair_shift, ext.hair_secondary_shift, roughness);
        lit = (AetherHairDiffuse(base_color, T, light_direction) + strand_specular * 0.5) * n_dot_l;
    } else if (ext.anisotropy > 0.001) {
        float3 fresnel = f0 + (1.0 - f0) * pow(1.0 - v_dot_h, 5.0);
        float3 diffuse = (1.0 - fresnel) * (1.0 - metallic) * base_color / 3.14159265 * n_dot_l;
        lit = diffuse + AetherAnisotropicSpecular(f0, roughness, ext.anisotropy, normal, T, B, view_direction, light_direction,
                                                  ext.anisotropy_rotation);
    }
    float sheen_peak = max(ext.sheen_color.r, max(ext.sheen_color.g, ext.sheen_color.b));
    if (sheen_peak > 0.0) {
        lit = lit * (1.0 - 0.2 * sheen_peak) + AetherSheen(ext.sheen_color, ext.sheen_roughness, n_dot_v, n_dot_l, n_dot_h);
    }
    if (ext.clear_coat > 0.001) {
        AetherCoat coat = AetherClearCoat(ext.clear_coat, max(ext.clear_coat_roughness, 0.04), n_dot_h, l_dot_h);
        lit = lit * coat.attenuation + coat.spec * n_dot_l;
    }
    return lit;
}

)" // split: MSVC caps one string literal at 16 KB
R"(
// AETHER_materials_detail: a procedural micro-surface from world position, so a texture stretched over hundreds of metres does not go soft close up. Three scales
// of value noise (about 0.4, 1.3 and 4.5 m) make a height; its slope tilts the normal, and a little brightness variation rides on it. No texture to load.
float DetailHash(float3 p) {
    p = frac(p * 0.3183099 + 0.1);
    p *= 17.0;
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float DetailNoise(float3 x) {
    float3 i = floor(x);
    float3 f = frac(x);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(lerp(DetailHash(i + float3(0, 0, 0)), DetailHash(i + float3(1, 0, 0)), f.x), lerp(DetailHash(i + float3(0, 1, 0)), DetailHash(i + float3(1, 1, 0)), f.x), f.y),
                lerp(lerp(DetailHash(i + float3(0, 0, 1)), DetailHash(i + float3(1, 0, 1)), f.x), lerp(DetailHash(i + float3(0, 1, 1)), DetailHash(i + float3(1, 1, 1)), f.x), f.y), f.z);
}
float DetailHeight(float3 p) { return 0.5 * DetailNoise(p / 0.4) + 0.3 * DetailNoise(p / 1.3) + 0.2 * DetailNoise(p / 4.5); }

float4 PSMain(PSInput input, bool is_front : SV_IsFrontFace) : SV_TARGET {
    // A double-sided material's back face is lit from its own side: the normal reversed. (A mirrored model turns its faces' winding
    // round, so the face toward the camera is the front when exactly one of the two says so.)
    bool facing_front = is_front;
#ifdef AETHER_FRAME_DATA
    // The water's mirror pass draws the world with its winding turned round: what faces the camera there is a back face.
    if (g_Frame.reflection.y != 0) facing_front = !facing_front;
#endif
    if (((g_PC.g_AlphaOptions >> 10) & 1) != 0 && facing_front == (NormalDeterminant() < 0.0)) input.normal = -input.normal;
    float4 material_color = float4(f16tof32(g_PC.g_ColorPacked.x), f16tof32(g_PC.g_ColorPacked.x >> 16),
                                   f16tof32(g_PC.g_ColorPacked.y), f16tof32(g_PC.g_ColorPacked.y >> 16));
    uint base_index = g_PC.g_MaterialTextures & 255;
    uint normal_index = (g_PC.g_MaterialTextures >> 8) & 255;
    uint metallic_roughness_index = (g_PC.g_MaterialTextures >> 16) & 255;
    uint occlusion_index = (g_PC.g_MaterialTextures >> 24) & 255;
    // Not a ?: — HLSL evaluates both arms, so the "no texture" index (63) would still be sampled out of bounds.
    float4 base_sample = float4(1, 1, 1, 1);
    if (base_index < 128) base_sample = g_Textures[base_index].Sample(g_Sampler, input.uv);
    uint alpha_mode = g_PC.g_AlphaOptions & 3;
    float surface_alpha = saturate(base_sample.a * material_color.a);
    if (alpha_mode == 1) {
        clip(surface_alpha - float((g_PC.g_AlphaOptions >> 16) & 31) / 31.0);
        surface_alpha = 1.0;
    } else if (alpha_mode == 0) {
        surface_alpha = 1.0;
    }
    float3 base_color = SRGBToLinear(base_sample.rgb) * material_color.rgb;
    float metallic = saturate(f16tof32(g_PC.g_MaterialFactors0 & 0xffff));
    float roughness = clamp(f16tof32(g_PC.g_MaterialFactors0 >> 16), 0.04, 1.0);
    float normal_scale = f16tof32(g_PC.g_MaterialFactors1 & 0xffff);
    float occlusion_strength = saturate(f16tof32(g_PC.g_MaterialFactors1 >> 16));
    if (metallic_roughness_index < 128) {
        float4 metallic_roughness = g_Textures[metallic_roughness_index].Sample(g_Sampler, input.uv);
        roughness = clamp(roughness * metallic_roughness.g, 0.04, 1.0);
        metallic = saturate(metallic * metallic_roughness.b);
    }

    float3 normal = normalize(input.normal);
    float3 tangent = normalize(input.tangent - normal * dot(normal, input.tangent));
    if (normal_index < 128) {
        float3 tangent_normal = g_Textures[normal_index].Sample(g_Sampler, input.uv).xyz * 2.0 - 1.0;
        if (((g_PC.g_AlphaOptions >> 11) & 1) != 0) {
            // Water (AETHER_materials_water): the same ripples again, scrolled the other way at another scale, so the two layers slide through each other and the
            // surface moves. The speeds are in UV a second; the shared sampler wraps. Without the frame constants there is no time, and the water stands still.
            float water_time = 0.0;
#ifdef AETHER_FRAME_DATA
            water_time = g_Frame.time.x;
#endif
            float3 second = g_Textures[normal_index].Sample(g_Sampler, input.uv * 1.37 + float2(-0.0031, 0.0023) * water_time + 0.37).xyz * 2.0 - 1.0;
            float3 first = g_Textures[normal_index].Sample(g_Sampler, input.uv + float2(0.0022, 0.0034) * water_time).xyz * 2.0 - 1.0;
            tangent_normal = float3((first.xy + second.xy) * 0.5, 0.5 * (first.z + second.z));
        }
        tangent_normal.xy *= normal_scale;
        tangent_normal = normalize(tangent_normal);
        float3 bitangent = normalize(cross(normal, tangent)) * input.tangent_sign;
        normal = normalize(tangent * tangent_normal.x + bitangent * tangent_normal.y + normal * tangent_normal.z);
    }

    uint detail_level = (g_PC.g_AlphaOptions >> 21) & 7;
    if (detail_level != 0) {
        float3 detail_position = input.local_position;
        float detail_fade = 1.0;
#ifdef AETHER_FRAME_DATA
        // In world space, so the micro-surface does not move with the object, and fading out with distance (where it would only shimmer).
        float3 detail_view = ApplyModelTransform(g_PC.g_CameraLocal - input.local_position);
        detail_position = g_Frame.camera_world.xyz - detail_view;
        detail_fade = saturate((170.0 - length(detail_view)) / 130.0);
#endif
        float detail_strength = float(detail_level) / 7.0 * detail_fade;
        if (detail_strength > 0.0) {
            const float eps = 0.06;
            float h0 = DetailHeight(detail_position);
            float3 slope = float3(DetailHeight(detail_position + float3(eps, 0, 0)) - h0, DetailHeight(detail_position + float3(0, eps, 0)) - h0,
                                  DetailHeight(detail_position + float3(0, 0, eps)) - h0) / eps;
            slope -= normal * dot(slope, normal);
            normal = normalize(normal - slope * (0.22 * detail_strength));
            base_color *= 1.0 + detail_strength * (0.9 * (DetailNoise(detail_position / 0.9) - 0.5) + 0.5 * (DetailNoise(detail_position / 9.0) - 0.5));
        }
    }

    float ambient_occlusion = 1.0;
    if (occlusion_index < 128) {
        float occlusion_sample = g_Textures[occlusion_index].Sample(g_Sampler, input.uv).r;
        ambient_occlusion = lerp(1.0, occlusion_sample, occlusion_strength);
    }
    SurfaceExtension surface_ext = NoSurfaceExtension();
    uint extension_index = (g_PC.g_AlphaOptions >> 2) & 255;
    // Not a ?: for the same reason as above.
    if (extension_index < 128) surface_ext = LoadSurfaceExtension(extension_index);
    float3 surface_tangent = normalize(tangent - normal * dot(normal, tangent));
    float3 surface_bitangent = normalize(cross(normal, surface_tangent)) * input.tangent_sign;
    float3 view_local = g_PC.g_CameraLocal - input.local_position;
    float3 view_direction = normalize(ApplyModelTransform(view_local));
    float3 f0 = lerp(0.04.xxx, base_color, metallic);
    // The sun and the sky's ambient light: the scene's own (DirectionalLight and SkyLight components, through the frame
    // constants), or the fixed ones this shader always had.
    float3 sun_direction = normalize(float3(-0.4, 0.8, -0.35));
    float3 sun_radiance = float3(3.0, 3.0, 3.0);
    float3 sky_tint = float3(1.0, 1.0, 1.0);
#ifdef AETHER_FRAME_DATA
    sun_direction = normalize(g_Frame.sun_direction.xyz);
    sun_radiance = g_Frame.sun_color.rgb;
    sky_tint = g_Frame.sky_color.rgb;
#endif
    // Broad sky and room fill keeps metallic characters readable outside
    // the key light. The environment term includes reflected metal color.
    float sky_fill = 0.28 + 0.14 * saturate(normal.y);
    float3 ambient = (base_color * (1.0 - metallic) * sky_fill + f0 * (0.45 + 0.25 * roughness)) * sky_tint * ambient_occlusion;
#ifdef AETHER_FRAME_DATA
    // Image-based lighting from the sky's environment, where the renderer has one (player/src/environment.h).
    if (g_Frame.environment_info.x < 128) {
        ambient = SkyLighting(normal, view_direction, base_color, metallic, roughness, f0, ambient_occlusion) * sky_tint;
    }
#endif
    float sun_visibility = 1.0;
#ifdef AETHER_FRAME_DATA
    // The world position of this pixel: the camera minus the (world-space) vector from the pixel to the camera.
    float3 world_position = g_Frame.camera_world.xyz - ApplyModelTransform(view_local);
    sun_visibility = SunShadow(world_position, normalize(input.normal), sun_direction);
#endif
    float3 color = ambient + SurfaceLightEx(sun_direction, normal, view_direction, base_color, metallic, roughness, f0, surface_ext, surface_tangent, surface_bitangent) * sun_radiance * sun_visibility;
#ifdef AETHER_FRAME_DATA
    if (g_Frame.cluster_info.w != 0) {
        // Clustered lights (player/src/light_clusters.h): the point and spot lights that can reach this pixel's cell of the view grid.
        uint2 cluster = ClusterRange(input.position.xy, ClusterViewDepth(world_position));
        for (uint cluster_i = 0; cluster_i < cluster.y; ++cluster_i) {
            uint light_offset = ClusterLightIndex(cluster.x + cluster_i) * 48;
            float4 position_range = asfloat(g_FrameLights.Load4(light_offset));
            float4 color_inner = asfloat(g_FrameLights.Load4(light_offset + 16));
            float4 direction_outer = asfloat(g_FrameLights.Load4(light_offset + 32));
            float3 to_light = position_range.xyz - world_position;
            float distance = max(length(to_light), 1e-4);
            float3 light_direction = to_light / distance;
            float falloff = saturate(1.0 - pow(distance / position_range.w, 4.0));
            falloff = falloff * falloff / (distance * distance + 1.0);
            if (falloff <= 0.0) continue;
            // A point light's cosines are below -1, so its cone passes everywhere.
            float cone = smoothstep(direction_outer.w, color_inner.w, dot(-light_direction, direction_outer.xyz));
            color += SurfaceLightEx(light_direction, normal, view_direction, base_color, metallic, roughness, f0, surface_ext, surface_tangent, surface_bitangent) * color_inner.rgb * (falloff * cone);
        }
    } else {
    for (uint point_index = 0; point_index < g_Frame.counts.x; ++point_index) {
        float3 to_light = g_Frame.point_position_range[point_index].xyz - world_position;
        float distance = max(length(to_light), 1e-4);
        float falloff = saturate(1.0 - pow(distance / g_Frame.point_position_range[point_index].w, 4.0));
        falloff = falloff * falloff / (distance * distance + 1.0);
        color += SurfaceLightEx(to_light / distance, normal, view_direction, base_color, metallic, roughness, f0, surface_ext, surface_tangent, surface_bitangent) * g_Frame.point_color[point_index].rgb * falloff;
    }
    for (uint spot_index = 0; spot_index < g_Frame.counts.y; ++spot_index) {
        float3 to_light = g_Frame.spot_position_range[spot_index].xyz - world_position;
        float distance = max(length(to_light), 1e-4);
        float3 light_direction = to_light / distance;
        float cone = smoothstep(g_Frame.spot_direction_outer[spot_index].w, g_Frame.spot_color_inner[spot_index].w,
                                dot(-light_direction, g_Frame.spot_direction_outer[spot_index].xyz));
        float falloff = saturate(1.0 - pow(distance / g_Frame.spot_position_range[spot_index].w, 4.0));
        falloff = falloff * falloff / (distance * distance + 1.0);
        color += SurfaceLightEx(light_direction, normal, view_direction, base_color, metallic, roughness, f0, surface_ext, surface_tangent, surface_bitangent) * g_Frame.spot_color_inner[spot_index].rgb * falloff * cone;
    }
    }
#endif
    float3 emissive = float3(f16tof32(g_PC.g_Emissive.x), f16tof32(g_PC.g_Emissive.x >> 16), f16tof32(g_PC.g_Emissive.y));
    uint emissive_index = (g_PC.g_Emissive.y >> 16) & 255;
    if (emissive_index < 128) emissive *= SRGBToLinear(g_Textures[emissive_index].Sample(g_Sampler, input.uv).rgb);
    color += emissive;
#ifdef AETHER_FRAME_DATA
    // Water (AETHER_materials_water) mirrors the scene: the frame's reflection texture is the world drawn mirrored about the water's plane from this same
    // camera, so a water pixel finds its reflection at its own screen position, nudged by how far the ripples tilt the normal. Fresnel decides how much of it
    // shows (more at grazing angles), and where the mirror drew nothing (alpha 0, the sky) the sky lighting above stays.
    if (((g_PC.g_AlphaOptions >> 11) & 1) != 0 && g_Frame.reflection.x < 128) {
        float2 reflection_size = float2(g_Frame.reflection.z, g_Frame.reflection.w);
        float2 ripple = (normal.xz - normalize(input.normal).xz) * 0.05;
        float2 reflection_uv = saturate(input.position.xy / reflection_size + ripple);
        float4 mirrored = g_Textures[g_Frame.reflection.x].SampleLevel(g_Sampler, reflection_uv, 0);
        float facing = saturate(dot(normal, view_direction));
        float fresnel = 0.04 + 0.96 * pow(1.0 - facing, 5.0);
        float mirror_weight = saturate(fresnel * 1.5 + 0.12) * mirrored.a;
        color = lerp(color, mirrored.rgb, mirror_weight);
        surface_alpha = lerp(surface_alpha, 1.0, mirror_weight);
    }
#endif
#ifdef AETHER_HDR_TARGET
    // Linear light into the HDR scene colour target; the post pass applies exposure, the tone mapper and the sRGB encoding.
    return float4(max(color, 0.0), surface_alpha);
#else
    color = color / (color + 1.0);
    color = LinearToSRGB(color);
    return float4(color, surface_alpha);
#endif
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
Texture2D g_Textures[128] : register(t0, space0);
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
    if (g_PC.g_UseTexture == 2) {
        // A picture (UI image): its colour times the tint, its alpha times the tint's.
        float4 picture = g_Textures[g_PC.g_TextureIndex].Sample(g_Sampler, input.uv);
        return float4(g_PC.g_Color.rgb * picture.rgb, alpha * picture.a);
    }
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
        u32 vertex_count = 0;
        AccelerationStructureHandle blas; // ray-traced shadows: this rigid primitive's bottom-level structure, made when first needed
        bool blas_failed = false;
        std::vector<BufferHandle> frame_vertex_buffers;
        // For a skinned primitive drawn more than once in a frame (entities sharing the model): the buffers of the second draw on, per
        // frame in flight, made when first needed. draws counts this frame's draws (draws_frame: which frame).
        std::vector<std::vector<BufferHandle>> extra_vertex_buffers;
        u32 draws = 0;
        u64 draws_frame = 0;
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
        bool double_sided = false; // glTF doubleSided: drawn without culling, the back face lit from behind
        bool water = false;        // AETHER_materials_water: the normal map scrolls with the game's time
        u32 detail_level = 0;      // AETHER_materials_detail: 0 to 7 (the micro-surface's strength, in sevenths)
        u32 wind_level = 0;        // AETHER_materials_wind: 0 to 15, 0.02 m of sway a step (the vertex shader sways the vegetation)
        SampledTextureHandle base_color_texture;
        SampledTextureHandle normal_texture;
        SampledTextureHandle metallic_roughness_texture;
        SampledTextureHandle occlusion_texture;
        SampledTextureHandle emissive_texture;
        bool has_base_color_texture = false;
        bool has_normal_texture = false;
        bool has_metallic_roughness_texture = false;
        bool has_occlusion_texture = false;
        bool has_emissive_texture = false;
        // KHR_materials_clearcoat, sheen and anisotropy: a 2x1 texture of the packed factors (assets::PackSurfaceExtension), read by texel fetch.
        SampledTextureHandle extension_texture;
        bool has_extension_texture = false;
        // A bounding sphere of the bind pose in model space, for culling shadow casters; radius < 0 for skinned primitives (always drawn).
        f32 bounds_center[3] = {0, 0, 0};
        f32 bounds_radius = -1.0f;
        f32 bounds_height = 0.0f; // the bounds' extent in y: a flat sheet of water has almost none
        // A skinned primitive's bind-pose bounding sphere, grown by half for its poses: what a virtual shadow page redraws when it moves.
        f32 skin_bounds_center[3] = {0, 0, 0};
        f32 skin_bounds_radius = -1.0f;
    };
    struct MaterialTextures {
        SampledTextureHandle base_color;
        SampledTextureHandle normal;
        SampledTextureHandle metallic_roughness;
        SampledTextureHandle occlusion;
        SampledTextureHandle emissive;
        bool has_base_color = false;
        bool has_normal = false;
        bool has_metallic_roughness = false;
        bool has_occlusion = false;
        bool has_emissive = false;
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
    PipelineHandle culled_pipeline;             // `pipeline` with back faces culled (single-sided materials)
    // The water's reflection (AETHER_RENDER_REFLECTIONS=0 turns it off): the scene drawn mirrored about the water's plane into a render texture with
    // depth, the size of the swap chain, which the water shader samples. Where the device has no such texture (D3D12 for now) there is none.
    bool reflections = true;
    PipelineDesc reflection_desc;               // the scene pipeline for that pass, set where `pipeline` is made
    PipelineHandle reflection_pipeline;         // made when the first water draws
    bool reflection_failed = false;
    SampledTextureHandle reflection_texture;
    std::vector<BufferHandle> reflection_frame_buffers; // the frame constants again for the mirror pass (reflection.y = 1), one per frame in flight
    u32 reflection_width = 0, reflection_height = 0;
    PipelineHandle transparent_culled_pipeline; // likewise for `transparent_pipeline`
    PipelineHandle hud_pipeline;
    // The particle pass (M8.7), drawn after the scene's surfaces.
    std::unique_ptr<ParticlePass> particles;
    std::unique_ptr<DecalPass> decals;
    // The HDR path (render roadmap step 1): with a scene colour target the scene, decals and particles draw in linear light and the post
    // pass tone maps into the back buffer; without one (hdr false) they draw straight into the back buffer as before.
    std::unique_ptr<PostPass> post;
    std::unique_ptr<ShadowPass> shadows; // where the device has frame constants and depth textures (D3D12)
    // Virtual shadow maps (virtual_shadow.h, vsm_pages.h, vsm_pass.h): pages of a much finer sun shadow near the camera, over the cascades, which
    // stay as the fallback. Where the device has frame constants, depth textures, readback and the HDR path. AETHER_RENDER_VSM=0 turns them off.
    std::unique_ptr<VsmFeedback> vsm_feedback;
    std::unique_ptr<VsmPass> vsm_pass;
    std::unique_ptr<VsmPageAllocator> vsm_pages;
    // Ray-traced sun shadows (AETHER_RENDER_RT_SHADOWS=1): the scene shader traces one ray per pixel against rt_tlas, rebuilt (synchronously) when
    // the rigid casters' placement changes. rt_dummy_* is a far-away triangle that keeps the structure valid when nothing casts.
    bool rt_shadows = false;
    AccelerationStructureHandle rt_tlas;
    AccelerationStructureHandle rt_dummy_blas;
    BufferHandle rt_dummy_vertices, rt_dummy_indices;
    std::vector<f32> rt_signature;
    u32 rt_instances = 0;
    u32 rt_rebuilds = 0;
    VsmLight vsm_light;
    bool vsm_have_light = false;
    u64 vsm_frame = 0;
    u32 vsm_pool = 24;         // pages per side of the pool (24 x 24 pages of 128 x 128 texels: 36 MB)
    u32 vsm_draw_budget = 64;  // pages drawn per frame
    struct VsmCasterState {
        Mat4 model;
        Vec3 center{0, 0, 0};
        f32 radius = -1.0f;
        u64 frame = 0;
        bool dynamic = false; // deforming: in the cascades, in no page
    };
    SceneRenderer::VirtualShadowStats vsm_stats;
    // GPU timing: a timer of four timestamps (start, before the scene pass, after it, end) per frame in flight, read when its frame has finished.
    static constexpr usize kGpuTimers = 4;
    std::vector<GpuTimerHandle> gpu_timers;
    bool gpu_timer_pending[kGpuTimers] = {};
    u64 gpu_timer_frame[kGpuTimers] = {};
    i32 gpu_timer_slot = -1; // this frame's, -1: not timed (still waiting to be read, or no timing)
    SceneRenderer::GpuTimings gpu_timings;
    u64 gpu_timings_frame = 0;
    void GpuTimingBegin(ICommandList& commands, u64 frame) {
        for (usize slot = 0; slot < gpu_timers.size(); ++slot) {
            if (!gpu_timer_pending[slot]) continue;
            f64 t[4] = {};
            if (!device.ReadGpuTimer(gpu_timers[slot], t)) continue;
            gpu_timer_pending[slot] = false;
            if (gpu_timer_frame[slot] >= gpu_timings_frame) {
                gpu_timings = {true, static_cast<f32>(t[3]), static_cast<f32>(t[1]), static_cast<f32>(t[2] - t[1]), static_cast<f32>(t[3] - t[2])};
                gpu_timings.frame_ms = gpu_timings.shadows_ms + gpu_timings.scene_ms + gpu_timings.post_ms;
                gpu_timings_frame = gpu_timer_frame[slot];
            }
        }
        gpu_timer_slot = -1;
        if (gpu_timers.empty()) return;
        const usize slot = static_cast<usize>(frame % gpu_timers.size());
        if (gpu_timer_pending[slot]) return; // its last frame has not finished: this one is not timed
        gpu_timer_slot = static_cast<i32>(slot);
        commands.WriteGpuTimestamp(gpu_timers[slot], 0);
    }
    void GpuStamp(ICommandList& commands, u32 index) {
        if (gpu_timer_slot >= 0) commands.WriteGpuTimestamp(gpu_timers[static_cast<usize>(gpu_timer_slot)], index);
    }
    void GpuTimingEnd(ICommandList& commands, u64 frame) {
        if (gpu_timer_slot < 0) return;
        const usize slot = static_cast<usize>(gpu_timer_slot);
        commands.WriteGpuTimestamp(gpu_timers[slot], 3);
        if (commands.ResolveGpuTimer(gpu_timers[slot])) {
            gpu_timer_pending[slot] = true;
            gpu_timer_frame[slot] = frame;
        }
        gpu_timer_slot = -1;
    }
    std::unique_ptr<TaaPass> taa;         // temporal anti-aliasing, with the HDR target (AETHER_RENDER_TAA=0 turns it off)
    // Each drawn node's model transform as of the frame it was last drawn, for TAA's motion pass: key (entity index, generation, node).
    struct PreviousTransform {
        Mat4 model;
        u64 frame = 0;
    };
    struct TransformKey {
        u32 index = 0, generation = 0, node = 0, primitive = 0; // primitive: 0 for transforms, which belong to the node
        bool operator==(const TransformKey&) const = default;
    };
    struct TransformKeyHash {
        usize operator()(const TransformKey& key) const {
            return std::hash<u64>()((static_cast<u64>(key.index) << 32 | key.generation) * 0x9E3779B97F4A7C15ull ^
                                    (static_cast<u64>(key.node) << 20 | key.primitive));
        }
    };
    std::unordered_map<TransformKey, PreviousTransform, TransformKeyHash> previous_transforms;
    // Each skinned primitive's deformed positions (three floats a vertex) as of the frame it was last drawn, per entity: last frame's
    // pose for the motion pass.
    struct PreviousPose {
        std::vector<f32> positions;
        u64 frame = 0;
    };
    std::unordered_map<TransformKey, PreviousPose, TransformKeyHash> previous_poses;
    // The sun's shadow casters as of the frame they were last drawn (per entity, node and primitive), for the pages that need redrawing.
    std::unordered_map<TransformKey, VsmCasterState, TransformKeyHash> vsm_casters;
    // This frame's packed previous poses, uploaded into one of motion_position_buffers (one per frame in flight, grown as needed).
    std::vector<f32> motion_positions;
    std::vector<BufferHandle> motion_position_buffers;
    std::vector<u64> motion_position_capacity;
    u64 transform_frame = 0;
    // Screen-space ambient occlusion in the post pass (AETHER_RENDER_AO=0 turns it off; AETHER_RENDER_AO_STRENGTH and _RADIUS tune it).
    bool ssao = false;
    f32 fog_sun_scatter = 0.1f; // the sun's light the fog takes toward the sun, as a share of its colour (AETHER_RENDER_FOG_SUN tunes it)
    f32 ao_strength = 0.9f, ao_radius = 2.5f;
    bool taa_motion = false; // the motion pass (AETHER_RENDER_TAA_MOTION=0 turns it off)
    std::unique_ptr<SkyPass> sky;        // where the device has frame constants and half-float textures (D3D12)
    bool hdr = false;
    vfx::ParticleRenderData particle_data;
    // Mesh particles' geometry, flattened once from the package's models (empty: the model couldn't be loaded).
    std::unordered_map<std::string, ParticleMesh> particle_meshes;
    bool draw_particles = true;
    bool bloom = true;
    // The scene's lights, as frame constants (one buffer per frame in flight) where the device has them; else the shader's fixed light.
    static constexpr usize kFrameBuffers = 8;
    std::vector<BufferHandle> frame_buffers;
    // The clustered lights (light_clusters.h), one frame read buffer per frame in flight beside frame_buffers. `clustered` false
    // (AETHER_RENDER_CLUSTERED=0) keeps the nearest 8 point and 4 spot lights of the frame constants; the buffers are bound either way.
    std::vector<BufferHandle> light_buffers;
    bool clustered = true;
    LightClusters clusters;              // this frame's, reused
    std::vector<ClusterLight> cluster_lights;
    // Skins big meshes in parallel (kParallelSkinVertices); made on first use by the rendering thread.
    std::unique_ptr<JobSystem> skin_jobs;
    bool legacy_hud = false; // the AETHER-01 HUD, drawn when the game has no UI of its own
    SampledTextureHandle white_texture;
    SampledTextureHandle font_texture_handle;
    std::unique_ptr<ui::SdfFont> hud_font;
    std::unordered_map<std::string, Model> models;
    std::unordered_map<std::string, sprite2d::SpriteAtlas> sprite_atlases;
    std::unordered_map<std::string, sprite2d::Tileset> sprite_tilesets;
    std::unordered_map<std::string, sprite2d::TilemapData> sprite_tilemaps;
    std::unordered_map<std::string, SampledTextureHandle> textures;
    u32 sampled_texture_count = 0;
    u32 seen_critical_pressure = 0;
    f32 view_distance = 0.0f;
    std::function<bool(Entity, Transform&)> render_pose_sampler;
    // Each entity's local matrix and parent, read once a frame instead of once for every drawn entity under it.
    HierarchySnapshot hierarchy;
    Entity animated_player;
    Vec3 previous_player_position{};
    f64 previous_animation_time = 0.0;
    bool has_previous_player_position = false;

    // Releases every GPU object the renderer made: pipelines, frame constants, each model's vertex/index buffers and the textures it
    // loaded. The device retires them once any frame still drawing with them has finished, so this is safe right after the last Draw.
    ~Impl() {
        auto free_buffer = [this](BufferHandle& buffer) {
            if (buffer.IsValid()) device.DestroyBuffer(buffer);
            buffer = {};
        };
        std::unordered_set<u32> freed_textures;
        auto free_texture = [&](SampledTextureHandle& texture) {
            if (texture.IsValid() && freed_textures.insert(texture.index).second) device.DestroyTexture(texture);
            texture = {};
        };
        for (auto& [path, model] : models) {
            for (auto& primitives : model.meshes) {
                for (Primitive& primitive : primitives) {
                    if (primitive.frame_vertex_buffers.empty()) {
                        free_buffer(primitive.vertices);
                    } else {
                        for (BufferHandle& buffer : primitive.frame_vertex_buffers) free_buffer(buffer);
                        for (std::vector<BufferHandle>& frame : primitive.extra_vertex_buffers) {
                            for (BufferHandle& buffer : frame) free_buffer(buffer);
                        }
                        primitive.vertices = {};
                    }
                    free_buffer(primitive.indices);
                    free_texture(primitive.extension_texture);
                }
            }
        }
        for (const GpuTimerHandle& timer : gpu_timers) device.DestroyGpuTimer(timer);
        if (rt_tlas.IsValid()) device.DestroyAccelerationStructure(rt_tlas);
        for (auto& [path, model] : models) {
            for (auto& primitives : model.meshes) {
                for (Primitive& primitive : primitives) {
                    if (primitive.blas.IsValid()) device.DestroyAccelerationStructure(primitive.blas);
                }
            }
        }
        if (rt_dummy_blas.IsValid()) device.DestroyAccelerationStructure(rt_dummy_blas);
        free_buffer(rt_dummy_vertices);
        free_buffer(rt_dummy_indices);
        for (BufferHandle& buffer : frame_buffers) free_buffer(buffer);
        for (BufferHandle& buffer : reflection_frame_buffers) free_buffer(buffer);
        for (BufferHandle& buffer : light_buffers) free_buffer(buffer);
        for (BufferHandle& buffer : motion_position_buffers) free_buffer(buffer);
        for (auto& [path, texture] : textures) free_texture(texture);
        free_texture(font_texture_handle);
        free_texture(white_texture);
        if (reflection_texture.IsValid()) device.DestroyTexture(reflection_texture);
        for (PipelineHandle* handle : {&pipeline, &transparent_pipeline, &culled_pipeline, &transparent_culled_pipeline, &hud_pipeline, &reflection_pipeline}) {
            if (handle->IsValid()) device.DestroyPipeline(*handle);
            *handle = {};
        }
    }

    Impl(IDevice& d, ISwapChain& s, const GamePackage& p) : device(d), swap_chain(s), package(p) {
        // AETHER_RENDER_HDR=0 keeps the old direct-to-back-buffer path (for comparisons, and as a way out if a driver mishandles it).
        const char* hdr_setting = std::getenv("AETHER_RENDER_HDR");
        if (hdr_setting == nullptr || std::strcmp(hdr_setting, "0") != 0) {
            post = std::make_unique<PostPass>(device, swap_chain);
            hdr = post->Valid();
        }
        const char* taa_setting = std::getenv("AETHER_RENDER_TAA");
        if (hdr && (taa_setting == nullptr || std::strcmp(taa_setting, "0") != 0)) {
            taa = std::make_unique<TaaPass>(device, swap_chain);
            if (!taa->Valid()) taa.reset();
            const char* motion_setting = std::getenv("AETHER_RENDER_TAA_MOTION"); // 0: the camera's reprojection alone, for comparison
            taa_motion = taa && (motion_setting == nullptr || std::strcmp(motion_setting, "0") != 0);
        }
        const char* ao_setting = std::getenv("AETHER_RENDER_AO");
        ssao = hdr && (ao_setting == nullptr || std::strcmp(ao_setting, "0") != 0);
        if (const char* v = std::getenv("AETHER_RENDER_FOG_SUN")) fog_sun_scatter = std::clamp(static_cast<f32>(std::atof(v)), 0.0f, 2.0f);
        if (const char* v = std::getenv("AETHER_RENDER_AO_STRENGTH")) ao_strength = std::clamp(static_cast<f32>(std::atof(v)), 0.0f, 1.0f);
        if (const char* v = std::getenv("AETHER_RENDER_AO_RADIUS")) ao_radius = std::clamp(static_cast<f32>(std::atof(v)), 0.05f, 20.0f);
        PipelineDesc desc;
        desc.hlsl_source = SceneShaderSource(false, hdr);
        desc.scene_color_target = hdr;
        if (device.SupportsFrameConstants()) {
            desc.hlsl_source = SceneShaderSource(true, hdr);
            desc.frame_constants_size_bytes = sizeof(FrameData);
            for (usize i = 0; i < kFrameBuffers; ++i) frame_buffers.push_back(device.CreateFrameConstantBuffer(sizeof(FrameData)));
            desc.frame_read_buffer = true;
            for (usize i = 0; i < kFrameBuffers; ++i) light_buffers.push_back(device.CreateFrameConstantBuffer(kLightBufferBytes));
            const char* clustered_setting = std::getenv("AETHER_RENDER_CLUSTERED");
            clustered = clustered_setting == nullptr || std::strcmp(clustered_setting, "0") != 0;
            // AETHER_RENDER_SHADOWS=0 turns sun shadows off (for comparisons).
            const char* shadow_setting = std::getenv("AETHER_RENDER_SHADOWS");
            if (shadow_setting == nullptr || std::strcmp(shadow_setting, "0") != 0) {
                shadows = std::make_unique<ShadowPass>(device, swap_chain);
                if (!shadows->Valid()) shadows.reset();
            }
            // AETHER_RENDER_RT_SHADOWS=1: sun shadows by ray query instead of the cascades and virtual pages (needs ray tracing; Vulkan also needs
            // AETHER_VK_RAY_TRACING=1 as the device is made, which the player sets).
            const char* rt_setting = std::getenv("AETHER_RENDER_RT_SHADOWS");
            if (shadows && device.SupportsRayTracing() && rt_setting != nullptr && std::strcmp(rt_setting, "1") == 0) {
                rt_shadows = true;
                desc.hlsl_source = SceneShaderSource(true, hdr, true);
                desc.shader_model_6 = true;
                desc.acceleration_structure = true;
            }
            const char* vsm_setting = std::getenv("AETHER_RENDER_VSM");
            if (shadows && !rt_shadows && hdr && device.SupportsReadback() && (vsm_setting == nullptr || std::strcmp(vsm_setting, "0") != 0)) {
                if (const char* pool = std::getenv("AETHER_VSM_POOL")) vsm_pool = std::clamp<u32>(static_cast<u32>(std::atoi(pool)), 2u, 64u);
                vsm_feedback = std::make_unique<VsmFeedback>(device, swap_chain);
                vsm_pass = std::make_unique<VsmPass>(device, swap_chain);
                // The scene depth copy the feedback reads (TAA asks for it too).
                (void)swap_chain.SceneColorTexture();
                const bool depth_ok = swap_chain.SceneDepthTexture().IsValid();
                if (!vsm_feedback->Valid() || !vsm_pass->Valid() || !depth_ok) {
                    vsm_feedback.reset();
                    vsm_pass.reset();
                } else {
                    vsm_pages = std::make_unique<VsmPageAllocator>(vsm_pool);
                    vsm_stats.enabled = true;
                }
            }
            // AETHER_RENDER_IBL=0 keeps the flat ambient term and background (for comparisons).
            const char* ibl_setting = std::getenv("AETHER_RENDER_IBL");
            if (ibl_setting == nullptr || std::strcmp(ibl_setting, "0") != 0) {
                sky = std::make_unique<SkyPass>(device, swap_chain, hdr);
                if (!sky->Valid()) sky.reset();
            }
        }
        desc.push_constant_size_bytes = sizeof(DrawConstants);
        desc.use_vertex_buffer = true;
        desc.vertex_layout = VertexLayout::PositionNormalUvTangent;
        desc.enable_bindless_textures = true;
        desc.depth_test = true;
        if (device.SupportsGpuTiming()) {
            for (usize i = 0; i < kGpuTimers; ++i) {
                const GpuTimerHandle timer = device.CreateGpuTimer(4);
                if (timer.IsValid()) gpu_timers.push_back(timer);
            }
        }
        if (rt_shadows) {
            // The scene shader through DXC at shader model 6.5; if it will not build, the cascades stay.
            try {
                pipeline = device.CreatePipeline(desc, swap_chain);
                std::vector<SceneVertex> dummy(3);
                dummy[0].position[1] = dummy[1].position[1] = dummy[2].position[1] = -100000.0f;
                dummy[1].position[0] = 1.0f;
                dummy[2].position[2] = 1.0f;
                const u32 dummy_indices[3] = {0, 1, 2};
                rt_dummy_vertices = device.CreateVertexBuffer(dummy.data(), dummy.size() * sizeof(SceneVertex));
                rt_dummy_indices = device.CreateIndexBuffer(dummy_indices, sizeof(dummy_indices), IndexFormat::UInt32);
                rt_dummy_blas = device.CreateBottomLevelAS(rt_dummy_vertices, 3, sizeof(SceneVertex), rt_dummy_indices, 3);
                if (!rt_dummy_blas.IsValid()) throw std::runtime_error("no acceleration structure");
            } catch (const std::exception& error) {
                AETHER_LOG_WARN("PlayerRenderer", "Ray-traced shadows unavailable (%s); the cascades stay", error.what());
                rt_shadows = false;
                if (pipeline.IsValid()) device.DestroyPipeline(pipeline);
                pipeline = {};
                desc.hlsl_source = SceneShaderSource(true, hdr);
                desc.shader_model_6 = false;
                desc.acceleration_structure = false;
            }
        }
        if (!pipeline.IsValid()) pipeline = device.CreatePipeline(desc, swap_chain);

        PipelineDesc transparent_desc = desc;
        transparent_desc.enable_blending = true;
        transparent_desc.depth_write = false;
        transparent_pipeline = device.CreatePipeline(transparent_desc, swap_chain);
        PipelineDesc culled_desc = desc;
        culled_desc.cull_back_face = true;
        culled_pipeline = device.CreatePipeline(culled_desc, swap_chain);
        transparent_desc.cull_back_face = true;
        transparent_culled_pipeline = device.CreatePipeline(transparent_desc, swap_chain);
        if (const char* v = std::getenv("AETHER_RENDER_REFLECTIONS")) reflections = std::strcmp(v, "0") != 0;
        if (hdr && !rt_shadows && !frame_buffers.empty()) {
            reflection_desc = desc;
            reflection_desc.scene_color_target = false;
            reflection_desc.render_texture_target = true;
            reflection_desc.render_texture_depth = true;
        } else {
            reflections = false;
        }
        if (!transparent_pipeline.IsValid()) {
            AETHER_LOG_WARN("PlayerRenderer", "Transparent material pipeline unavailable; blended materials draw opaque");
        }

        constexpr std::array<u8, 4> white{255, 255, 255, 255};
        white_texture = device.CreateTexture(1, 1, white.data());
        if (white_texture.IsValid()) {
            ++sampled_texture_count;
            textures.emplace("", white_texture);
        }
        // The built-in HUD is specific to the AETHER-01 content contract; every game can show its own UI (WidgetComponent layouts)
        // with the same pipeline and font.
        legacy_hud = package.Manifest().project == "AETHER-01";

        particles = std::make_unique<ParticlePass>(device, swap_chain, hdr);
        decals = std::make_unique<DecalPass>(device, swap_chain, hdr);
        // Particle materials are textures in the package, through the same cache as the models' (0: untextured, drawn procedurally).
        particles->SetTextureResolver([this](const std::string& material) -> u32 {
            if (material == "@aether-white") return white_texture.index;
            SampledTextureHandle texture;
            return LoadTexture(material, texture) ? texture.index : 0u;
        });
        particles->SetMeshResolver([this](const std::string& path) -> const ParticleMesh* { return FindParticleMesh(path); });

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
                // Printable ASCII: the built-in HUD uses capitals and digits, and a game's own UI layouts any text. (The atlas is
                // uploaded once, here; glyphs outside this set would draw as blanks.)
                hud_font->Prewarm(" !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~");
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
                    font_texture_handle = font_texture;
                    hud_font->SetTexture(font_texture.index);
                } else {
                    AETHER_LOG_WARN("PlayerRenderer", "Couldn't upload the AETHER-01 HUD font atlas");
                    hud_font.reset();
                }
            } else {
                AETHER_LOG_WARN("PlayerRenderer", "Couldn't load the AETHER-01 HUD font: %s", error.c_str());
            }
        } else {
            if (legacy_hud) AETHER_LOG_WARN("PlayerRenderer", "Cooked package has no fonts/Roboto-Medium.ttf; HUD text is unavailable");
            else AETHER_LOG_INFO("PlayerRenderer", "Cooked package has no fonts/Roboto-Medium.ttf; UI text is unavailable");
        }
    }

    // The frame's decals: every enabled Decal component on an entity with a Transform, as ready-to-draw items (visible ones, in draw order).
    std::vector<DecalPassItem> GatherDecals(const World& world, const GuidIndex& guids, const Mat4& view_projection, const Vec3& camera_position) {
        std::vector<mat::DecalInput> inputs;
        std::vector<const Decal*> found;
        world.ForEachArchetype([&](const Archetype& archetype) {
            if (!archetype.Mask().test(GetComponentId<Decal>())) return;
            for (usize chunk = 0; chunk < archetype.ChunkCount(); ++chunk) {
                const Entity* entities = archetype.EntityArray(chunk);
                for (usize i = 0; i < archetype.ChunkEntityCount(chunk); ++i) {
                    const Decal* decal = world.GetComponent<Decal>(entities[i]);
                    if (decal == nullptr || !decal->enabled || !world.HasComponent<Transform>(entities[i])) continue;
                    inputs.push_back({ComputeWorldTransform(world, guids, entities[i]), decal->size, decal->sort_order, static_cast<u32>(found.size())});
                    found.push_back(decal);
                }
            }
        });
        std::vector<DecalPassItem> items;
        if (inputs.empty()) return items;
        std::vector<mat::DecalDraw> draws;
        mat::BuildDecalDraws(inputs, view_projection, camera_position, draws);
        for (const mat::DecalDraw& draw : draws) {
            const Decal& decal = *found[draw.id];
            DecalPassItem item;
            item.draw = draw;
            SampledTextureHandle texture;
            if (!decal.texture.empty() && LoadTexture(decal.texture, texture)) item.texture = texture.index;
            if (decals->HdrTarget()) {
                item.color[0] = std::max(decal.color.x, 0.0f), item.color[1] = std::max(decal.color.y, 0.0f), item.color[2] = std::max(decal.color.z, 0.0f);
            } else {
                item.color[0] = DecalTint(decal.color.x), item.color[1] = DecalTint(decal.color.y), item.color[2] = DecalTint(decal.color.z);
            }
            item.color[3] = std::clamp(decal.opacity, 0.0f, 1.0f);
            item.fade_start = decal.angle_fade_start, item.fade_end = decal.angle_fade_end;
            items.push_back(item);
        }
        return items;
    }

    void DrawHud(Game& game, ICommandList& commands) {
        if (!hud_font) return;
        const World& world = game.GetWorld();
        const f32 width = static_cast<f32>(swap_chain.Width());
        const f32 height = static_cast<f32>(swap_chain.Height());
        if (width <= 0 || height <= 0) return;

        // A game with its own UI (WidgetComponent layouts) shows that, not the built-in HUD, which belongs to AETHER-01.
        if (ui::Viewport* viewport = game.Ui(); viewport != nullptr && viewport->LayerCount() > 0) {
            viewport->SetSize({width, height});
            viewport->Layout(*hud_font);
            ui::DrawList list = viewport->Paint(*hud_font);
            // Image quads carry the game's image id; make each a texture (text quads are distance-field ones and keep the font's).
            for (ui::DrawQuad& quad : list.quads) {
                if (quad.texture == 0 || quad.sdf_range > 0.0f) continue;
                SampledTextureHandle texture;
                quad.texture = LoadTexture(game.UiImagePath(quad.texture), texture) ? texture.index : 0; // 0: drawn as its tint
            }
            AddSubtitle(game, list, width, height);
            SubmitHudQuads(commands, list, width, height);
            return;
        }
        if (!legacy_hud) {
            ui::DrawList list;
            AddSubtitle(game, list, width, height);
            if (!list.quads.empty()) SubmitHudQuads(commands, list, width, height);
            return;
        }

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
            } else if (menu_state == 3) {
                const std::vector<Game::RebindableInput> bindings = game.RebindableInputs();
                const i32 binding_count = static_cast<i32>(bindings.size());
                const i32 item_count = binding_count + 2; // reset all, back
                const i32 first = std::min(std::max(0, menu_row - 5), std::max(0, item_count - 6));
                for (i32 visible = 0; visible < 6 && first + visible < item_count; ++visible) {
                    const i32 index = first + visible;
                    const f32 y = my + 66 + visible * 36.0f;
                    if (index == menu_row) solid(mx + 20, y, 440, 32, dim);
                    if (index < binding_count) {
                        const Game::RebindableInput& binding = bindings[static_cast<usize>(index)];
                        std::string label = binding.context + " / " + binding.action;
                        if (binding.slot > 0) label += " [" + std::to_string(binding.slot + 1) + "]";
                        text(label, mx + 30, y + 7, 14, index == menu_row ? cyan : pale, 285);
                        std::string key = input::KeyName(binding.current_key);
                        if (binding.current_key != binding.default_key) key += " *";
                        text(key, mx + 330, y + 7, 14, cyan, 112);
                    } else {
                        const char* label = index == binding_count ? "RESET ALL TO DEFAULTS" : "BACK";
                        text(label, mx + 30, y + 7, 14, index == menu_row ? cyan : pale, 400);
                    }
                }
            } else {
                const char* rows[] = {"WASD / LEFT STICK: MOVE", "MOUSE / RIGHT STICK: LOOK", "LEFT CLICK / RT: FIRE",
                    campaign_act > 0 ? "1-5 WEAPONS / SHIFT DASH / Q PULSE" : "1 2 3 / D-PAD: WEAPONS", "R / X: RELOAD    SPACE / A: JUMP", "E / B: INTERACT    F5: CHECKPOINT"};
                for (i32 i = 0; i < 6; ++i) text(rows[i], mx + 24, my + 73 + i * 35.0f, 15, pale, 432);
            }
            text(settings_status == 3 ? "PRESS A KEY OR PAD BUTTON  ·  ESC CANCEL" :
                 settings_status == 2 ? "SHARED BINDING SAVED" :
                 settings_status == -1 ? "SETTINGS SAVE FAILED" : settings_status == -2 ? "NO CHECKPOINT YET" :
                 settings_status == 1 ? "SETTINGS SAVED" : "SIMULATION PAUSED",
                 mx + 24, my + 310, 13, settings_status < 0 ? red : muted, 430);
            text(menu_state == 2 ? "LEFT/RIGHT ADJUST   ESC BACK" : menu_state == 3 ?
                 "UP/DOWN SELECT  ENTER REBIND  DEL RESET  ESC BACK" : "ARROWS SELECT   ENTER CONFIRM   ESC BACK",
                 mx + 24, my + 344, 12, muted, 435);
        }
        AddSubtitle(game, list, width, height);
        SubmitHudQuads(commands, list, width, height);
    }

    // The dialogue line a sequence's Subtitle track shows (Game::Subtitle, already
    // translated and empty when the setting hides subtitles), low and centred.
    void AddSubtitle(const Game& game, ui::DrawList& list, f32 width, f32 height) {
        const std::string& line = game.Subtitle();
        if (line.empty()) return;
        const f32 box = std::min(720.0f, width - 48.0f);
        const f32 x = (width - box) * 0.5f, y = height * 0.88f;
        list.AddQuad({x, y, box, 54}, {0.0f, 0.0f, 0.0f, 0.72f});
        const std::string& speaker = game.SubtitleSpeaker();
        if (!speaker.empty()) list.AddText(*hud_font, speaker, 12, {x + 16, y + 6, box - 32, 18}, {0.18f, 0.82f, 0.95f, 1.0f});
        list.AddText(*hud_font, line, 17, {x + 16, y + (speaker.empty() ? 16.0f : 24.0f), box - 32, 23}, {1.0f, 1.0f, 1.0f, 1.0f});
    }

    // Draws a UI draw list (quads in pixels, each clipped) with the HUD pipeline.
    void SubmitHudQuads(ICommandList& commands, const ui::DrawList& list, f32 width, f32 height) {
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
            constants.use_texture = quad.texture == 0 ? 0u : (quad.sdf_range > 0.0f ? 1u : 2u); // 1: a font atlas (one channel), 2: a picture
            constants.use_sdf = quad.sdf_range > 0.0f ? 1u : 0u;
            commands.SetPushConstants(&constants, sizeof(constants));
            commands.Draw(6);
        }
    }

    // The SkyLight's environment, rebuilt when its image or rotation changes: the image from the package (a .hdr as it is, an LDR image
    // decoded from sRGB), or the procedural sky when it has none or the image can't be read (said once per image).
    // The cooked environment of an .hdr (Cooked/<guid>.aenv, built by the cooker at rotation 0 with the default settings). False when the pak has
    // none, it is damaged, or it was built with other settings than this player uses; the caller then prefilters the source itself.
    bool LoadCookedEnvironment(const std::string& path, PrefilteredEnvironment& out) {
        const GameManifest::Asset* asset = package.FindAsset(path);
        if (asset == nullptr || asset->cooked.size() < 5 || asset->cooked.compare(asset->cooked.size() - 5, 5, ".aenv") != 0) return false;
        std::vector<u8> bytes;
        std::string why;
        if (!package.Files().Read(asset->cooked, bytes, &why)) {
            AETHER_LOG_WARN("PlayerRenderer", "Can't read %s: %s; prefiltering '%s' here", asset->cooked.c_str(), why.c_str(), path.c_str());
            return false;
        }
        PrefilterSettings built;
        if (!assets::LoadEnvironment(bytes, out, &built, &why)) {
            AETHER_LOG_WARN("PlayerRenderer", "%s: %s; prefiltering '%s' here", asset->cooked.c_str(), why.c_str(), path.c_str());
            return false;
        }
        const PrefilterSettings wanted;
        return built.max_width == wanted.max_width && built.smallest_width == wanted.smallest_width && built.samples == wanted.samples;
    }

    void UpdateSky(const SkyLight& light) {
        const f32 rotation = std::isfinite(light.environment_rotation_degrees) ? light.environment_rotation_degrees : 0.0f;
        const std::string key = light.environment + "@" + std::to_string(rotation);
        if (sky->Key() == key && sky->HasEnvironment()) return;
        if (sky->Key() == key) return; // tried and failed: don't retry every frame
        // A sky that is not turned and has a cooked environment skips the prefilter (the rotation is baked into the cooked data, so a turned
        // sky is prefiltered here). AETHER_RENDER_COOKED_SKY=0 ignores the cooked data, for comparison.
        const char* cooked_setting = std::getenv("AETHER_RENDER_COOKED_SKY");
        if (rotation == 0.0f && !light.environment.empty() && (cooked_setting == nullptr || std::strcmp(cooked_setting, "0") != 0)) {
            const auto start = std::chrono::steady_clock::now();
            PrefilteredEnvironment cooked;
            if (LoadCookedEnvironment(light.environment, cooked) && sky->SetPrefilteredEnvironment(key, std::move(cooked))) {
                AETHER_LOG_INFO("PlayerRenderer", "Sky environment '%s': %ux%u, %u levels, cooked, loaded in %.1f ms", light.environment.c_str(),
                                sky->Environment().width, sky->Environment().height, sky->Environment().mip_count,
                                std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count());
                return;
            }
        }
        EnvironmentImage image;
        if (!light.environment.empty()) {
            std::vector<u8> bytes;
            if (!package.ReadContent(light.environment, bytes)) {
                AETHER_LOG_WARN("PlayerRenderer", "SkyLight environment '%s' is not in the package; using the procedural sky", light.environment.c_str());
            } else if (assets::IsHdrImage(bytes)) {
                assets::HdrImageData hdr_image;
                if (assets::DecodeHdrImageFromMemory(bytes, hdr_image)) image = MakeEnvironment(hdr_image.width, hdr_image.height, hdr_image.rgb, rotation);
            } else {
                assets::ImageData ldr_image;
                if (assets::DecodeImageFromMemory(bytes, ldr_image)) image = MakeEnvironmentFromSrgb(ldr_image.width, ldr_image.height, ldr_image.pixels, rotation);
            }
            if (image.width == 0 && !bytes.empty()) {
                AETHER_LOG_WARN("PlayerRenderer", "SkyLight environment '%s' can't be decoded; using the procedural sky", light.environment.c_str());
            }
        }
        if (sky->SetEnvironment(key, image)) {
            AETHER_LOG_INFO("PlayerRenderer", "Sky environment '%s': %ux%u, %u levels, prefiltered in %.1f ms",
                            light.environment.empty() ? "(procedural)" : light.environment.c_str(), sky->Environment().width,
                            sky->Environment().height, sky->Environment().mip_count, sky->LastBuildMilliseconds());
        }
    }

    // `filter` says how the mip chain averages it (colour by default; a material's normal and data maps say so). A texture used both
    // ways is cached once per way.
    bool LoadTexture(const std::string& path, SampledTextureHandle& out, assets::MipFilter filter = assets::MipFilter::Srgb) {
        if (path.empty()) {
            out = white_texture;
            return false;
        }
        const std::string key = filter == assets::MipFilter::Srgb ? path : path + (filter == assets::MipFilter::NormalMap ? "#normal" : "#linear");
        if (const auto found = textures.find(key); found != textures.end()) {
            out = found->second;
            return out.IsValid() && out.index != white_texture.index;
        }
        if (sampled_texture_count >= kMaxUserBindlessTextures) {
            AETHER_LOG_WARN("PlayerRenderer", "Bindless texture limit reached; using the white fallback for '%s'", path.c_str());
            out = white_texture;
            return false;
        }
        // A package whose sources were stripped: BC and ASTC colour textures stay compressed on the GPU as cooked (a quarter of the memory or
        // less, with their mips), where the device can; everything else is decoded to RGBA8 below.
        if (!PackageHasTextureSource(package, path)) {
            cook::CookedTexture cooked;
            if (LoadPackageCookedTexture(package, path, cooked) && !cooked.normal_map && !cooked.mips.empty()) {
                // BC1, BC3, BC7 and ASTC colour textures map to the device's formats; BC4 and BC5 (one and two channels) and normal maps do not.
                CompressedTextureFormat format = CompressedTextureFormat::BC7;
                bool direct = true;
                switch (cooked.format) {
                case cook::TextureFormat::BC1: format = CompressedTextureFormat::BC1; break;
                case cook::TextureFormat::BC3: format = CompressedTextureFormat::BC3; break;
                case cook::TextureFormat::BC7: format = CompressedTextureFormat::BC7; break;
                case cook::TextureFormat::ASTC_4x4: format = CompressedTextureFormat::ASTC_4x4; break;
                case cook::TextureFormat::ASTC_6x6: format = CompressedTextureFormat::ASTC_6x6; break;
                case cook::TextureFormat::ASTC_8x8: format = CompressedTextureFormat::ASTC_8x8; break;
                default: direct = false; break;
                }
                if (direct) {
                    if (device.SupportsCompressedTexture(format)) {
                        std::vector<CompressedMip> mips;
                        for (const cook::CookedMip& mip : cooked.mips) mips.push_back({mip.data.data(), mip.width, mip.height, mip.data.size()});
                        out = device.CreateCompressedTexture(format, cooked.width, cooked.height, mips);
                        if (out.IsValid()) {
                            ++sampled_texture_count;
                            textures.emplace(key, out);
                            return out.index != white_texture.index;
                        }
                    }
                }
            }
        }
        assets::ImageData image;
        std::string why;
        if (!LoadPackageTexture(package, path, image, &why)) {
            AETHER_LOG_WARN("PlayerRenderer", "Couldn't load model texture '%s' (%s); using the white fallback", path.c_str(), why.c_str());
            out = white_texture;
            textures.emplace(key, white_texture); // remembered, so a missing UI image isn't read and decoded again every frame
            return false;
        }
        // With its whole mip chain, so minified surfaces filter instead of shimmering.
        u32 mip_count = 0;
        const std::vector<u8> levels = assets::BuildMipChain(image.width, image.height, image.pixels, filter, mip_count);
        out = mip_count > 0 ? device.CreateTextureWithMips(image.width, image.height, mip_count, levels.data())
                            : device.CreateTexture(image.width, image.height, image.pixels.data());
        if (out.IsValid()) {
            ++sampled_texture_count;
            textures.emplace(key, out);
        }
        return out.IsValid() && out.index != white_texture.index;
    }

    // An image embedded in a model's buffer (images[i].bufferView), decoded and uploaded with its mips like LoadTexture's, cached
    // under `key` (the model's path and the image's index).
    bool LoadEmbeddedTexture(const std::string& key, const json& document, usize view_index, const std::vector<std::vector<u8>>& buffers,
                             SampledTextureHandle& out, assets::MipFilter filter) {
        const std::string cache_key = key + (filter == assets::MipFilter::Srgb ? "" : filter == assets::MipFilter::NormalMap ? "#normal" : "#linear");
        if (const auto found = textures.find(cache_key); found != textures.end()) {
            out = found->second;
            return out.IsValid() && out.index != white_texture.index;
        }
        out = white_texture;
        if (sampled_texture_count >= kMaxUserBindlessTextures) {
            AETHER_LOG_WARN("PlayerRenderer", "Bindless texture limit reached; using the white fallback for '%s'", key.c_str());
            return false;
        }
        if (!document.contains("bufferViews") || !document["bufferViews"].is_array() || view_index >= document["bufferViews"].size()) return false;
        const json& view = document["bufferViews"][view_index];
        const usize buffer_index = view.value("buffer", usize(-1));
        const usize offset = view.value("byteOffset", usize(0));
        const usize length = view.value("byteLength", usize(0));
        assets::ImageData image;
        if (buffer_index >= buffers.size() || length == 0 || offset > buffers[buffer_index].size() ||
            length > buffers[buffer_index].size() - offset ||
            !assets::DecodeImageFromMemory(std::span<const u8>(buffers[buffer_index].data() + offset, length), image)) {
            AETHER_LOG_WARN("PlayerRenderer", "Couldn't decode embedded texture '%s'; using the white fallback", key.c_str());
            textures.emplace(cache_key, white_texture);
            return false;
        }
        u32 mip_count = 0;
        const std::vector<u8> levels = assets::BuildMipChain(image.width, image.height, image.pixels, filter, mip_count);
        out = mip_count > 0 ? device.CreateTextureWithMips(image.width, image.height, mip_count, levels.data())
                            : device.CreateTexture(image.width, image.height, image.pixels.data());
        if (!out.IsValid()) {
            out = white_texture;
            return false;
        }
        ++sampled_texture_count;
        textures.emplace(cache_key, out);
        return true;
    }

    bool LoadModel(const std::string& path, Model& out) {
        std::vector<u8> source;
        if (!package.ReadContent(path, source)) {
            AETHER_LOG_WARN("PlayerRenderer", "Couldn't read cooked model '%s'", path.c_str());
            return false;
        }
        // A .glb: its JSON chunk is the document and its BIN chunk becomes buffer 0's data URI (the parser below reads only
        // self-contained documents).
        std::string glb_json;
        std::vector<u8> glb_bin;
        const bool glb = source.size() >= 4 && std::memcmp(source.data(), "glTF", 4) == 0;
        if (glb && !assets::ReadGlbChunks(source, glb_json, glb_bin)) {
            AETHER_LOG_WARN("PlayerRenderer", "Cooked model '%s' isn't a valid GLB", path.c_str());
            return false;
        }
        json document = glb ? json::parse(glb_json, nullptr, false) : json::parse(source.begin(), source.end(), nullptr, false);
        // Each buffer's bytes, kept for images stored in a bufferView (a .glb's usual way of embedding them).
        std::vector<std::vector<u8>> buffer_bytes;
        bool parse_glb = false;          // the parser reads `source` itself
        std::vector<u8> built_glb;       // or this: a one-buffer .gltf with its buffer as the BIN chunk
        if (!document.is_discarded() && document.is_object() && document.contains("buffers") && document["buffers"].is_array()) {
            buffer_bytes.resize(document["buffers"].size());
        }
        if (glb && !document.is_discarded() && document.is_object() && document.contains("buffers") && document["buffers"].is_array() &&
            !document["buffers"].empty() && document["buffers"][0].is_object() && !document["buffers"][0].contains("uri")) {
            json& buffer = document["buffers"][0];
            // The data URI holds byteLength exactly (the chunk's alignment padding is not the buffer's).
            if (buffer.contains("byteLength") && buffer["byteLength"].is_number_unsigned()) {
                const usize declared = buffer["byteLength"].get<usize>();
                if (declared <= glb_bin.size()) glb_bin.resize(declared);
            }
            parse_glb = true; // the file as it is: buffer 0 is its BIN chunk
            buffer_bytes[0] = std::move(glb_bin);
        }
        if (document.is_discarded() || !document.is_object()) {
            AETHER_LOG_WARN("PlayerRenderer", "Cooked model '%s' isn't valid glTF JSON", path.c_str());
            return false;
        }

        // A cooked package is a VFS, not a filesystem. Embed the already
        // cooked buffer bytes into the in-memory glTF document so the shared
        // parser can safely load it without opening arbitrary paths.
        // A .gltf with one external buffer becomes a GLB in memory (see MakeGlb).
        if (!glb && document.contains("buffers") && document["buffers"].is_array() && document["buffers"].size() == 1 &&
            document["buffers"][0].is_object() && document["buffers"][0].contains("uri") && document["buffers"][0]["uri"].is_string() &&
            document["buffers"][0]["uri"].get<std::string>().rfind("data:", 0) != 0) {
            const std::string buffer_path = ResolveContentPath(path, document["buffers"][0]["uri"].get<std::string>());
            std::vector<u8> bytes;
            if (!package.ReadContent(buffer_path, bytes)) {
                AETHER_LOG_WARN("PlayerRenderer", "Model '%s' references missing buffer '%s'", path.c_str(), buffer_path.c_str());
                return false;
            }
            json without_uri = document;
            without_uri["buffers"][0].erase("uri");
            if (without_uri["buffers"][0].contains("byteLength") && without_uri["buffers"][0]["byteLength"].is_number_unsigned()) {
                const usize declared = without_uri["buffers"][0]["byteLength"].get<usize>();
                if (declared <= bytes.size()) bytes.resize(declared);
            }
            built_glb = MakeGlb(without_uri.dump(), bytes);
            buffer_bytes[0] = std::move(bytes);
        } else if (!parse_glb && document.contains("buffers") && document["buffers"].is_array()) {
            for (usize buffer_index = 0; buffer_index < document["buffers"].size(); ++buffer_index) {
                json& buffer = document["buffers"][buffer_index];
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
                buffer_bytes[buffer_index] = std::move(bytes);
            }
        }
        std::string cooked_json;
        std::span<const u8> parser_input;
        if (parse_glb) {
            parser_input = source;
        } else if (!built_glb.empty()) {
            parser_input = built_glb;
        } else {
            cooked_json = document.dump(); // buffers embedded as data URIs: several, or none
            parser_input = std::span<const u8>(reinterpret_cast<const u8*>(cooked_json.data()), cooked_json.size());
        }
        if (!assets::LoadGltfFromMemory(parser_input, out.scene)) {
            AETHER_LOG_WARN("PlayerRenderer", "Couldn't parse cooked model '%s'", path.c_str());
            return false;
        }

        auto upload_texture_reference = [&](const json& texture_reference, SampledTextureHandle& texture,
                                            assets::MipFilter filter = assets::MipFilter::Srgb) {
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
            if (image.contains("bufferView") && image["bufferView"].is_number_unsigned()) {
                return LoadEmbeddedTexture(path + "#image" + std::to_string(image_index), document, image["bufferView"].get<usize>(),
                                           buffer_bytes, texture, filter);
            }
            if (!image.contains("uri") || !image["uri"].is_string()) return false;
            const std::string uri = image["uri"].get<std::string>();
            if (uri.rfind("data:", 0) == 0) return false;
            return LoadTexture(ResolveContentPath(path, uri), texture, filter);
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
                                                     textures_for_material.metallic_roughness, assets::MipFilter::Linear);
                    }
                }
                if (material_json.contains("normalTexture")) {
                    textures_for_material.has_normal =
                        upload_texture_reference(material_json["normalTexture"], textures_for_material.normal, assets::MipFilter::NormalMap);
                }
                if (material_json.contains("occlusionTexture")) {
                    textures_for_material.has_occlusion =
                        upload_texture_reference(material_json["occlusionTexture"], textures_for_material.occlusion, assets::MipFilter::Linear);
                }
                if (material_json.contains("emissiveTexture")) {
                    textures_for_material.has_emissive =
                        upload_texture_reference(material_json["emissiveTexture"], textures_for_material.emissive);
                }
            }
        }

        std::vector<SampledTextureHandle> extension_textures;
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
                    {
                        Vec3 lo(vertices[0].position[0], vertices[0].position[1], vertices[0].position[2]), hi = lo;
                        for (const SceneVertex& v : vertices) {
                            lo = Vec3(std::min(lo.x, v.position[0]), std::min(lo.y, v.position[1]), std::min(lo.z, v.position[2]));
                            hi = Vec3(std::max(hi.x, v.position[0]), std::max(hi.y, v.position[1]), std::max(hi.z, v.position[2]));
                        }
                        const Vec3 center = (lo + hi) * 0.5f;
                        f32 radius = 0.0f;
                        for (const SceneVertex& v : vertices) {
                            radius = std::max(radius, (Vec3(v.position[0], v.position[1], v.position[2]) - center).Length());
                        }
                        draw.skin_bounds_center[0] = center.x, draw.skin_bounds_center[1] = center.y, draw.skin_bounds_center[2] = center.z;
                        draw.skin_bounds_radius = radius * 1.5f;
                    }
                    AETHER_LOG_INFO("PlayerRenderer", "Skinned primitive: %zu vertices, per-frame vertex buffers %u..%u",
                                    vertices.size(), draw.frame_vertex_buffers.front().index,
                                    draw.frame_vertex_buffers.back().index);
                } else {
                    draw.vertices = device.CreateVertexBuffer(vertices.data(), vertices.size() * sizeof(SceneVertex));
                    draw.vertex_count = static_cast<u32>(vertices.size());
                    if (!vertices.empty()) {
                        Vec3 lo(vertices[0].position[0], vertices[0].position[1], vertices[0].position[2]), hi = lo;
                        for (const SceneVertex& v : vertices) {
                            lo = Vec3(std::min(lo.x, v.position[0]), std::min(lo.y, v.position[1]), std::min(lo.z, v.position[2]));
                            hi = Vec3(std::max(hi.x, v.position[0]), std::max(hi.y, v.position[1]), std::max(hi.z, v.position[2]));
                        }
                        const Vec3 center = (lo + hi) * 0.5f;
                        f32 radius = 0.0f;
                        for (const SceneVertex& v : vertices) {
                            radius = std::max(radius, (Vec3(v.position[0], v.position[1], v.position[2]) - center).Length());
                        }
                        draw.bounds_center[0] = center.x, draw.bounds_center[1] = center.y, draw.bounds_center[2] = center.z;
                        draw.bounds_radius = radius;
                        draw.bounds_height = hi.y - lo.y;
                    }
                }
                draw.indices = device.CreateIndexBuffer(primitive.indices.data(), primitive.indices.size() * sizeof(u32),
                                                        IndexFormat::UInt32);
                draw.index_count = static_cast<u32>(primitive.indices.size());
                if (primitive.material_index >= 0 && static_cast<usize>(primitive.material_index) < out.scene.materials.size()) {
                    const auto& material = out.scene.materials[static_cast<usize>(primitive.material_index)];
                    std::copy(std::begin(material.base_color), std::end(material.base_color), draw.color);
                    draw.metallic = material.metallic;
                    draw.roughness = material.roughness;
                    for (usize c = 0; c < 3; ++c) draw.emissive[c] = material.emissive_factor[c] * material.emissive_strength;
                    draw.normal_scale = material.normal_scale;
                    draw.occlusion_strength = material.occlusion_strength;
                    draw.alpha_cutoff = material.alpha_cutoff;
                    draw.alpha_mode = material.alpha_mode;
                    draw.double_sided = material.double_sided;
                    draw.water = material.water;
                    draw.detail_level = static_cast<u32>(std::clamp(std::lround(material.detail * 7.0f), 0l, 7l));
                    draw.wind_level = static_cast<u32>(std::clamp(std::lround(material.wind / 0.02f), 0l, 15l));
                    const usize material_index = static_cast<usize>(primitive.material_index);
                    if (material.HasSurfaceExtension()) {
                        if (extension_textures.size() < out.scene.materials.size()) extension_textures.resize(out.scene.materials.size());
                        SampledTextureHandle& cached = extension_textures[material_index];
                        if (!cached.IsValid()) {
                            u8 texels[16] = {};
                            assets::PackSurfaceExtension(material, texels);
                            // A hair material adds two texels, which the shader finds by the texture's width.
                            if (material.hair) assets::PackHairExtension(material, texels + 8);
                            cached = device.CreateTexture(material.hair ? 4 : 2, 1, texels);
                        }
                        // A full bindless table leaves the material plain rather than failing the model.
                        draw.extension_texture = cached;
                        draw.has_extension_texture = cached.IsValid();
                    }
                    if (material_index < material_textures.size()) {
                        const MaterialTextures& textures_for_material = material_textures[material_index];
                        draw.base_color_texture = textures_for_material.base_color;
                        draw.normal_texture = textures_for_material.normal;
                        draw.metallic_roughness_texture = textures_for_material.metallic_roughness;
                        draw.occlusion_texture = textures_for_material.occlusion;
                        draw.emissive_texture = textures_for_material.emissive;
                        draw.has_emissive_texture = textures_for_material.has_emissive;
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

    // Critical memory pressure: frees every GPU texture but the white fallback (the device waits for the GPU first, so this runs between
    // frames). Models keep their meshes and lose their textures (drawn white, as if untextured); UI images are loaded again on demand.
    u32 TrimTextures() {
        u32 freed = 0;
        for (auto it = textures.begin(); it != textures.end();) {
            if (it->second.index != white_texture.index && device.DestroyTexture(it->second)) ++freed;
            it = it->second.index != white_texture.index ? textures.erase(it) : std::next(it);
        }
        sampled_texture_count -= std::min(sampled_texture_count, freed); // the font atlas and the white texture stay
        for (auto& [path, model] : models) {
            for (auto& primitives : model.meshes) {
                for (Primitive& primitive : primitives) {
                    primitive.base_color_texture = primitive.normal_texture = primitive.metallic_roughness_texture = primitive.occlusion_texture =
                        primitive.emissive_texture = white_texture;
                    primitive.has_base_color_texture = primitive.has_normal_texture = primitive.has_metallic_roughness_texture =
                        primitive.has_occlusion_texture = primitive.has_emissive_texture = false;
                }
            }
        }
        if (freed > 0) AETHER_LOG_INFO("PlayerRenderer", "Memory pressure: freed %u GPU texture(s)", freed);
        return freed;
    }

    // A model as one triangle list in its own space (every node's mesh with its world transform baked in), for mesh particles.
    const ParticleMesh* FindParticleMesh(const std::string& path) {
        auto [it, inserted] = particle_meshes.try_emplace(path);
        if (inserted) {
            if (const Model* model = FindModel(path)) {
                for (const assets::GltfNodeInstance& node : model->scene.node_instances) {
                    if (node.mesh_index >= model->scene.meshes.size()) continue;
                    for (const assets::GltfPrimitive& primitive : model->scene.meshes[node.mesh_index].primitives) {
                        const auto append = [&](u32 index) {
                            const assets::GltfVertex& v = primitive.vertices[index];
                            const Vec4 p = node.world_transform * Vec4(v.position[0], v.position[1], v.position[2], 1.0f);
                            it->second.positions.push_back(Vec3(p.x, p.y, p.z));
                            it->second.uvs.push_back(v.uv[0]);
                            it->second.uvs.push_back(v.uv[1]);
                            // The node's normal matrix (inverse transpose) baked in too, for lighting.
                            Vec4 n = node.world_transform * Vec4(v.normal[0], v.normal[1], v.normal[2], 0.0f);
                            Mat4 inverse;
                            if (node.world_transform.TryInverse(inverse)) {
                                const Vec4 c0 = inverse.cols[0], c1 = inverse.cols[1], c2 = inverse.cols[2];
                                n = Vec4(c0.x * v.normal[0] + c0.y * v.normal[1] + c0.z * v.normal[2],
                                         c1.x * v.normal[0] + c1.y * v.normal[1] + c1.z * v.normal[2],
                                         c2.x * v.normal[0] + c2.y * v.normal[1] + c2.z * v.normal[2], 0.0f);
                            }
                            it->second.normals.push_back(Vec3(n.x, n.y, n.z).Normalized());
                        };
                        if (primitive.indices.empty()) {
                            for (usize i = 0; i + 3 <= primitive.vertices.size(); ++i) append(static_cast<u32>(i));
                        } else {
                            for (usize i = 0; i + 3 <= primitive.indices.size(); i += 3) {
                                if (primitive.indices[i] >= primitive.vertices.size() || primitive.indices[i + 1] >= primitive.vertices.size() ||
                                    primitive.indices[i + 2] >= primitive.vertices.size()) {
                                    continue;
                                }
                                for (usize k = 0; k < 3; ++k) append(primitive.indices[i + k]);
                            }
                        }
                    }
                }
            }
            if (it->second.positions.empty()) AETHER_LOG_WARN("PlayerRenderer", "Mesh particles' mesh '%s' has no triangles; not drawn", path.c_str());
        }
        return it->second.positions.empty() ? nullptr : &it->second;
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

void SceneRenderer::SetViewDistance(f32 metres) { impl_->view_distance = metres; }
void SceneRenderer::SetDrawParticles(bool draw) { impl_->draw_particles = draw; }
void SceneRenderer::SetShadows(u32 resolution, u32 cascades) {
    if (impl_->shadows) impl_->shadows->Configure(resolution, cascades);
    if (resolution == 0 || cascades == 0) { // no sun shadows: the pages and their atlas go too
        if (impl_->vsm_pass) impl_->vsm_pass->Release();
        if (impl_->vsm_pages) impl_->vsm_pages->Clear();
        impl_->vsm_casters.clear();
    }
}
SceneRenderer::VirtualShadowStats SceneRenderer::VirtualShadows() const { return impl_->vsm_stats; }
SceneRenderer::GpuTimings SceneRenderer::GpuFrameTimings() const { return impl_->gpu_timings; }
SceneRenderer::RayTracedShadowStats SceneRenderer::RayTracedShadows() const {
    return {impl_->rt_shadows, impl_->rt_instances, impl_->rt_rebuilds};
}
void SceneRenderer::SetBloom(bool enabled) {
    impl_->bloom = enabled;
    if (!enabled && impl_->post) impl_->post->ReleaseBloom(); // the chain's bindless slots go back to the table
}
void SceneRenderer::SetRenderPoseSampler(std::function<bool(Entity, Transform&)> sampler) {
    impl_->render_pose_sampler = std::move(sampler);
}

void SceneRenderer::Draw(Game& game, ICommandList& commands, u32 frame_index) {
    struct PendingTransparentDraw {
        BufferHandle vertices;
        BufferHandle indices;
        u32 index_count = 0;
        DrawConstants constants;
        f32 distance_squared = 0.0f;
        bool cull = false; // back faces culled: a single-sided material on a model that isn't mirrored
    };
    // Opaque and alpha-tested draws are gathered first and issued once the shadow atlas (drawn before the scene pass) is ready.
    struct PendingOpaqueDraw {
        BufferHandle vertices;
        BufferHandle indices;
        u32 index_count = 0;
        DrawConstants constants;
        bool cull = false;
        Mat4 model;           // for the water reflection's pass, which draws it again with another matrix
        bool reflect = false; // drawn into the water's reflection: still, single-sided, opaque surfaces (not plants, skinned meshes or water); double-sided ones too, with the mirror pass's own frame constants flagging it
    };

    Impl& renderer = *impl_;
    renderer.GpuTimingBegin(commands, renderer.transform_frame + 1);
    if (game.CriticalMemoryPressureEvents() != renderer.seen_critical_pressure) {
        renderer.seen_critical_pressure = game.CriticalMemoryPressureEvents();
        renderer.TrimTextures();
    }
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

    // The background: the same display colour on both paths. In the HDR target it is the linear value that ACES at exposure 1 maps back to
    // (solved numerically: the curve's toe crushes the plain sRGB decode of these values to black). Other tone mappers or exposures shift it.
    constexpr ClearColor kBackground{0.015f, 0.025f, 0.045f, 1.0f};
    constexpr ClearColor kBackgroundLinear{0.01037f, 0.01450f, 0.02188f, 1.0f};
    std::vector<PendingTransparentDraw> transparent_draws;
    std::vector<PendingOpaqueDraw> opaque_draws;
    std::vector<ShadowCaster> shadow_casters;
    std::vector<AccelerationInstance> rt_instances; // ray-traced shadows: the rigid opaque casters, placed
    std::vector<TaaMover> taa_movers; // opaque draws whose model transform or skinned pose changed since last frame
    struct DirtySphere {
        Vec3 center;
        f32 radius;
    };
    std::vector<DirtySphere> vsm_dirty; // where a caster is, and was, that is new or has moved: virtual shadow pages to redraw
    std::vector<VsmPageAllocator::Sphere> vsm_suppress; // where a deforming caster is: its pages are left out of the page table
    const u64 transform_frame = ++renderer.transform_frame;
    renderer.motion_positions.clear();
    FrameData frame_data;
    std::vector<WindZoneSite> wind_zones; // the world's wind zones (an object picks the one it stands in)
    View shadow_view;
    bool sun_shadows = false;
    bool draw_sky = false;
    BufferHandle frame_buffer; // this frame's lights, when the device has frame constants
    BufferHandle light_buffer; // and its clustered lights
    const Entity camera_entity = FindActiveCamera(world, guids);
    const Camera* camera = world.IsAlive(camera_entity) ? world.GetComponent<Camera>(camera_entity) : nullptr;
    Mat4 particle_view_projection, particle_projection;
    vfx::ParticleCamera particle_camera;
    bool draw_particles = false;
    bool draw_sprite2d = false;
    Mat4 decal_view_projection;
    Mat4 post_camera_view; // the camera's view matrix, for the fog's sun in-scatter
    Mat4 water_projection; // the camera's steady projection (the water reflection's pass)
    bool have_water = false;  // a water surface was drawn this frame, at its first one's height
    f32 water_level = 0.0f;
    f32 water_radius = 0.0f;
    Vec3 water_camera;
    f32 ao_proj_x = 1.0f, ao_proj_y = 1.0f, ao_near = 0.1f, ao_far = 1000.0f; // the camera's, for the post pass's occlusion
    Mat4 taa_view_projection; // without the jitter
    Vec3 decal_camera_position;
    bool have_camera = false;
    if (camera) {
        const f32 aspect = static_cast<f32>(renderer.swap_chain.Width()) /
                           static_cast<f32>(std::max(renderer.swap_chain.Height(), 1u));
        const Mat4 camera_view = CameraView(world, guids, camera_entity);
        post_camera_view = camera_view;
        water_projection = CameraProjection(LimitViewDistance(*camera, renderer.view_distance), aspect);
        const Mat4 steady_projection = CameraProjection(LimitViewDistance(*camera, renderer.view_distance), aspect);
        // Temporal anti-aliasing moves every draw by this frame's sub-pixel offset; its resolve reprojects without it.
        Mat4 projection = steady_projection;
        if (renderer.taa) {
            f32 jitter_x = 0.0f, jitter_y = 0.0f;
            renderer.taa->NextJitter(jitter_x, jitter_y);
            projection = JitterProjection(steady_projection, jitter_x, jitter_y, renderer.swap_chain.Width(), renderer.swap_chain.Height());
        }
        taa_view_projection = steady_projection * camera_view;
        ao_proj_x = steady_projection.cols[0].x;
        ao_proj_y = steady_projection.cols[1].y;
        ao_near = std::max(LimitViewDistance(*camera, renderer.view_distance).near_plane, 1e-4f);
        ao_far = std::max(LimitViewDistance(*camera, renderer.view_distance).far_plane, ao_near * 1.001f);
        const Mat4 view_projection = projection * camera_view;
        particle_view_projection = view_projection;
        particle_projection = projection;
        // Sprite2D and tilemaps share the particle pass's textured-quad GPU path. Their CPU batch builder
        // handles sorting, atlas UVs, map layers and camera culling; this adapter preserves those batches
        // as camera-facing particle sprites so package assets use the same texture cache and device limits.
        {
            particle_camera = vfx::ParticleCamera::FromView(camera_view);
            renderer.particle_data.Clear();
            if (renderer.draw_particles && game.Particles())
                game.Particles()->BuildRenderData(particle_camera, renderer.particle_data, &view_projection);
            sprite2d::Resolvers2D resolvers;
            const auto read_json = [&renderer](const assets::AssetGuid& guid) -> nlohmann::json {
                const GameManifest::Asset* asset = renderer.package.FindAssetByGuid(assets::ToString(guid));
                std::vector<u8> bytes;
                if (!asset || !renderer.package.ReadContent(asset->path, bytes)) return {};
                return nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
            };
            resolvers.atlases = [&renderer, &read_json](const assets::AssetGuid& guid) -> const sprite2d::SpriteAtlas* {
                const std::string key = assets::ToString(guid);
                auto it = renderer.sprite_atlases.find(key);
                if (it == renderer.sprite_atlases.end()) {
                    sprite2d::SpriteAtlas atlas;
                    const nlohmann::json json = read_json(guid);
                    if (json.is_discarded() || json.is_null() || !sprite2d::AtlasFromJson(json, atlas)) return nullptr;
                    it = renderer.sprite_atlases.emplace(key, std::move(atlas)).first;
                }
                return &it->second;
            };
            resolvers.tilesets = [&renderer, &read_json](const assets::AssetGuid& guid) -> const sprite2d::Tileset* {
                const std::string key = assets::ToString(guid);
                auto it = renderer.sprite_tilesets.find(key);
                if (it == renderer.sprite_tilesets.end()) {
                    sprite2d::Tileset tileset;
                    const nlohmann::json json = read_json(guid);
                    if (json.is_discarded() || json.is_null() || !sprite2d::TilesetFromJson(json, tileset)) return nullptr;
                    it = renderer.sprite_tilesets.emplace(key, std::move(tileset)).first;
                }
                return &it->second;
            };
            resolvers.tilemaps = [&renderer, &read_json](const assets::AssetGuid& guid) -> const sprite2d::TilemapData* {
                const std::string key = assets::ToString(guid);
                auto it = renderer.sprite_tilemaps.find(key);
                if (it == renderer.sprite_tilemaps.end()) {
                    sprite2d::TilemapData map;
                    const nlohmann::json json = read_json(guid);
                    if (json.is_discarded() || json.is_null() || !sprite2d::TilemapFromJson(json, map)) return nullptr;
                    it = renderer.sprite_tilemaps.emplace(key, std::move(map)).first;
                }
                return &it->second;
            };
            std::optional<sprite2d::ViewRect> sprite_view;
            if (camera->projection == Projection::Orthographic) {
                const Mat4 camera_world = ComputeWorldTransform(world, guids, camera_entity);
                const f32 half_h = std::max(camera->ortho_height, 0.0f) * 0.5f;
                const f32 half_w = half_h * aspect;
                const f32 half_x = std::abs(camera_world.cols[0].x) * half_w + std::abs(camera_world.cols[1].x) * half_h;
                const f32 half_y = std::abs(camera_world.cols[0].y) * half_w + std::abs(camera_world.cols[1].y) * half_h;
                const Vec3 center{camera_world.cols[3].x, camera_world.cols[3].y, camera_world.cols[3].z};
                sprite_view = sprite2d::ViewRect{center.x - half_x, center.y - half_y, center.x + half_x, center.y + half_y};
            }
            std::optional<sprite2d::LightMap> sprite_lighting;
            bool has_sprite_lights = false;
            world.ForEach<Transform, sprite2d::Light2D>([&](Transform&, sprite2d::Light2D& light) {
                has_sprite_lights = has_sprite_lights || (light.enabled && std::isfinite(light.intensity) && light.intensity > 0.0f);
            });
            if (sprite_view && has_sprite_lights) {
                sprite_lighting = sprite2d::BuildLightMap(
                    world, resolvers, {sprite_view->min_x, sprite_view->min_y}, {sprite_view->max_x, sprite_view->max_y}, 0.5f);
            }
            for (const sprite2d::SpriteBatch& source :
                 sprite2d::BuildSpriteBatches(world, resolvers, sprite_view ? &*sprite_view : nullptr)) {
                vfx::SpriteBatch batch;
                const GameManifest::Asset* texture = renderer.package.FindAssetByGuid(assets::ToString(source.texture));
                batch.material = texture ? texture->path : "@aether-white";
                batch.blend = vfx::BlendMode::Alpha;
                for (usize i = 0; i + 3 < source.vertices.size(); i += 4) {
                    const auto& tl = source.vertices[i];
                    const auto& tr = source.vertices[i + 1];
                    const auto& br = source.vertices[i + 2];
                    const auto& bl = source.vertices[i + 3];
                    vfx::SpriteInstance sprite;
                    sprite.center = {(tl.x + tr.x + br.x + bl.x) * 0.25f, (tl.y + tr.y + br.y + bl.y) * 0.25f,
                                     (tl.z + tr.z + br.z + bl.z) * 0.25f};
                    sprite.right = {(tr.x - tl.x) * 0.5f, (tr.y - tl.y) * 0.5f, (tr.z - tl.z) * 0.5f};
                    sprite.up = {(tl.x - bl.x) * 0.5f, (tl.y - bl.y) * 0.5f, (tl.z - bl.z) * 0.5f};
                    sprite.uv = {tl.u, tl.v, tr.u - tl.u, bl.v - tl.v};
                    sprite.uv_next = sprite.uv;
                    sprite.color = {tl.r, tl.g, tl.b, tl.a};
                    if (sprite_lighting) {
                        const sprite2d::LightColor light = sprite_lighting->Sample(sprite.center.x, sprite.center.y);
                        sprite.color.r *= light.r;
                        sprite.color.g *= light.g;
                        sprite.color.b *= light.b;
                    }
                    batch.instances.push_back(sprite);
                }
                if (!batch.instances.empty()) renderer.particle_data.sprites.push_back(std::move(batch));
            }
            draw_particles = renderer.draw_particles && game.Particles() &&
                             (!renderer.particle_data.sprites.empty() || !renderer.particle_data.meshes.empty() ||
                              !renderer.particle_data.ribbons.empty());
            draw_sprite2d = !renderer.particle_data.sprites.empty() &&
                            std::any_of(renderer.particle_data.sprites.begin(), renderer.particle_data.sprites.end(),
                                        [](const vfx::SpriteBatch& batch) { return !batch.instances.empty(); });
        }
        const Mat4 camera_transform = ComputeWorldTransform(world, guids, camera_entity);
        const Vec3 camera_position{camera_transform.cols[3].x, camera_transform.cols[3].y, camera_transform.cols[3].z};
        decal_view_projection = view_projection;
        decal_camera_position = camera_position;
        have_camera = true;
        shadow_view = MakeView(LimitViewDistance(*camera, renderer.view_distance), camera_transform, aspect);
        // Mesh particles get the scene shader's sun and sky: the frame's lights where the device has frame constants, else the shader's
        // fixed light (ParticleLighting's defaults), through the pass's push constants on every backend.
        ParticleLighting particle_lighting;
        particle_lighting.lit = true;
        if (!renderer.frame_buffers.empty()) {
            // The sun, sky and nearby point and spot lights of this scene (the previous use of this slot's frame has finished).
            frame_buffer = renderer.frame_buffers[frame_index % renderer.frame_buffers.size()];
            frame_data = BuildFrameData(world, guids, camera_position);
            wind_zones = CollectWindZones(world, guids);
            frame_data.time[0] = static_cast<f32>(std::fmod(std::max(game_time, 0.0), 3600.0)); // wraps hourly: a float keeps its precision for the ripples' scroll
            if (draw_particles) AddParticleLights(frame_data, renderer.particle_data.lights, camera_position);
            if (!renderer.light_buffers.empty()) {
                light_buffer = renderer.light_buffers[frame_index % renderer.light_buffers.size()];
                if (renderer.clustered) {
                    // Every light in view, binned into the view grid; the frame constants' nearest lights stay for lit particles.
                    renderer.cluster_lights = GatherClusterLights(
                        world, guids, draw_particles ? std::span<const vfx::ParticleLight>(renderer.particle_data.lights)
                                                     : std::span<const vfx::ParticleLight>());
                    const Camera limited = LimitViewDistance(*camera, renderer.view_distance);
                    const ClusterView cluster_view{camera_view, projection, std::max(limited.near_plane, 1e-4f),
                                                   std::max(limited.far_plane, std::max(limited.near_plane, 1e-4f) * 1.001f)};
                    BuildLightClusters(renderer.cluster_lights, cluster_view, renderer.clusters);
                    FillClusterData(frame_data, renderer.clusters, cluster_view, renderer.swap_chain.Width(), renderer.swap_chain.Height());
                }
            }
            sun_shadows = renderer.shadows && frame_data.counts[2] != 0; // uploaded once the shadow pass has filled its part
            // The sky's image-based lighting and background, from a SkyLight (a world without one keeps the flat ambient term).
            if (renderer.sky) {
                if (const SkyLight* sky_light = FindSkyLight(world)) {
                    renderer.UpdateSky(*sky_light);
                    renderer.sky->Fill(frame_data);
                    draw_sky = renderer.hdr && renderer.sky->HasEnvironment();
                }
            }
            particle_lighting.sun_direction = Vec3(frame_data.sun_direction[0], frame_data.sun_direction[1], frame_data.sun_direction[2]);
            particle_lighting.sun_color = Vec3(frame_data.sun_color[0], frame_data.sun_color[1], frame_data.sun_color[2]);
            particle_lighting.sky_color = Vec3(frame_data.sky_color[0], frame_data.sky_color[1], frame_data.sky_color[2]);
            // Lit sprites take the frame's point lights too (the scene's and the particles' own).
            for (u32 i = 0; i < frame_data.counts[0] && i < kMaxPointLights; ++i) {
                particle_lighting.points.push_back({Vec3(frame_data.point_position_range[i][0], frame_data.point_position_range[i][1], frame_data.point_position_range[i][2]),
                                                    frame_data.point_position_range[i][3],
                                                    Vec3(frame_data.point_color[i][0], frame_data.point_color[i][1], frame_data.point_color[i][2])});
            }
        }
        if (renderer.particles) renderer.particles->SetLighting(particle_lighting);
        renderer.hierarchy.Reset(world, guids, &renderer.render_pose_sampler);
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
                    // What ComputeRenderWorldTransform(world, guids, entity, sampler) returns, bit for bit.
                    const Mat4 entity_transform = renderer.hierarchy.WorldTransform(entities[row]);
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
                        // Last frame's transform of this node, when it was drawn last frame, and whether it moved since (for TAA's
                        // motion pass; a skinned primitive whose pose changed is a mover even when the transform did not).
                        std::optional<Mat4> previous_model;
                        bool transform_moved = false;
                        if (renderer.taa_motion) {
                            Impl::PreviousTransform& previous = renderer.previous_transforms[{entities[row].index, entities[row].generation,
                                                                                              static_cast<u32>(node.node_index)}];
                            if (previous.frame + 1 == transform_frame) {
                                previous_model = previous.model;
                                transform_moved = TaaTransformMoved(previous.model, model_transform);
                            }
                            previous.model = model_transform;
                            previous.frame = transform_frame;
                        }
                        const Mat4 normal_transform = BuildNormalTransform(model_transform);
                        std::vector<Mat4> skin_normal_matrices;
                        if (has_skin && !model->skin_matrices.empty()) {
                            assets::ComputeSkinNormalMatrices(model->skin_matrices, skin_normal_matrices);
                        }
                        auto& draw_primitives = model->meshes[node.mesh_index];
                        const auto& source_primitives = model->scene.meshes[node.mesh_index].primitives;
                        for (usize primitive_index = 0; primitive_index < draw_primitives.size(); ++primitive_index) {
                            Impl::Primitive& primitive = draw_primitives[primitive_index];
                            // A skinned primitive's vertices are written for each draw, and the GPU reads them when the frame runs, after every
                            // draw is recorded: so each draw of it in a frame needs its own buffer, or the entities that share a model all
                            // show the pose written last. The first draw uses the primitive's per-frame buffer, the others buffers made on
                            // demand (kept for later frames).
                            BufferHandle frame_vertex_buffer = primitive.vertices;
                            if (!primitive.frame_vertex_buffers.empty()) {
                                if (primitive.draws_frame != transform_frame) {
                                    primitive.draws_frame = transform_frame;
                                    primitive.draws = 0;
                                }
                                const usize draw_slot = primitive.draws++;
                                const usize frame_slot = frame_index % primitive.frame_vertex_buffers.size();
                                frame_vertex_buffer = primitive.frame_vertex_buffers[frame_slot];
                                if (draw_slot > 0) {
                                    primitive.extra_vertex_buffers.resize(primitive.frame_vertex_buffers.size());
                                    std::vector<BufferHandle>& extra = primitive.extra_vertex_buffers[frame_slot];
                                    while (extra.size() < draw_slot) {
                                        const BufferHandle made = renderer.device.CreateVertexBuffer(
                                            primitive.bind_pose_vertices.data(), primitive.bind_pose_vertices.size() * sizeof(SceneVertex));
                                        if (!made.IsValid()) break;
                                        extra.push_back(made);
                                    }
                                    if (extra.size() >= draw_slot) frame_vertex_buffer = extra[draw_slot - 1]; // else: shares the first
                                }
                            }
                            bool uploaded_animation = false;
                            u32 previous_positions = kTaaNoPreviousPositions; // this draw's last-frame pose in motion_positions
                            if (has_skin && !model->skin_matrices.empty() && primitive_index < source_primitives.size()) {
                                const auto& source = source_primitives[primitive_index];
                                const auto& matrices = model->skin_matrices;
                                if (!source.joint_indices.empty() && source.joint_indices.size() == primitive.bind_pose_vertices.size() &&
                                    source.joint_weights.size() == primitive.bind_pose_vertices.size() && !matrices.empty()) {
                                    std::vector<SceneVertex>& deformed = primitive.animated_vertices;
                                    // Meshes big enough to be worth splitting go over the job system (made on first use, on this
                                    // thread, which is the one that renders); small ones are cheaper to skin in place.
                                    const bool split = deformed.size() >= kParallelSkinVertices;
                                    if (split && !renderer.skin_jobs) renderer.skin_jobs = std::make_unique<JobSystem>(2);
                                    assets::SkinVerticesParallel(
                                        split ? renderer.skin_jobs.get() : nullptr, primitive.bind_pose_vertices.data(),
                                        deformed.data(), source.joint_indices.data(), source.joint_weights.data(),
                                        deformed.size(), matrices, skin_normal_matrices);
                                    if (!renderer.device.WriteBufferUnsynchronized(frame_vertex_buffer, deformed.data(),
                                                                                   deformed.size() * sizeof(SceneVertex))) {
                                        renderer.device.UpdateVertexBuffer(frame_vertex_buffer, deformed.data(),
                                                                           deformed.size() * sizeof(SceneVertex));
                                    }
                                    uploaded_animation = true;
                                    if (renderer.taa_motion && renderer.taa->ReadsPreviousPositions() &&
                                        primitive.alpha_mode != assets::MaterialAlphaMode::Blend) {
                                        Impl::PreviousPose& pose_before = renderer.previous_poses[{
                                            entities[row].index, entities[row].generation, static_cast<u32>(node.node_index),
                                            static_cast<u32>(primitive_index) + 1}];
                                        const usize floats = deformed.size() * 3;
                                        bool pose_moved = false;
                                        if (pose_before.frame + 1 == transform_frame && pose_before.positions.size() == floats) {
                                            for (usize v = 0; v < deformed.size() && !pose_moved; ++v) {
                                                for (usize axis = 0; axis < 3; ++axis) {
                                                    if (pose_before.positions[v * 3 + axis] != deformed[v].position[axis]) pose_moved = true;
                                                }
                                            }
                                        }
                                        if (pose_moved) {
                                            previous_positions = static_cast<u32>(renderer.motion_positions.size() * sizeof(f32));
                                            renderer.motion_positions.insert(renderer.motion_positions.end(), pose_before.positions.begin(),
                                                                             pose_before.positions.end());
                                        }
                                        pose_before.positions.resize(floats);
                                        for (usize v = 0; v < deformed.size(); ++v) {
                                            for (usize axis = 0; axis < 3; ++axis) pose_before.positions[v * 3 + axis] = deformed[v].position[axis];
                                        }
                                        pose_before.frame = transform_frame;
                                    }
                                }
                            }
                            if (!primitive.frame_vertex_buffers.empty() && !uploaded_animation) {
                                if (!renderer.device.WriteBufferUnsynchronized(frame_vertex_buffer, primitive.bind_pose_vertices.data(),
                                                                               primitive.bind_pose_vertices.size() * sizeof(SceneVertex))) {
                                    renderer.device.UpdateVertexBuffer(frame_vertex_buffer, primitive.bind_pose_vertices.data(),
                                                                       primitive.bind_pose_vertices.size() * sizeof(SceneVertex));
                                }
                            }
                            DrawConstants constants;
                            constants.mvp = mvp;
                            constants.color_packed[0] = PackHalfPair(primitive.color[0], primitive.color[1]);
                            // A wind material that is opaque has no use for its alpha: it carries the instance's phase instead (from where it stands in the world,
                            // so the sway travels across a field like a gust and not in step), in radians.
                            f32 alpha_or_phase = primitive.color[3];
                            if (primitive.wind_level != 0 && primitive.alpha_mode == assets::MaterialAlphaMode::Opaque) {
                                alpha_or_phase = static_cast<f32>(std::fmod(static_cast<f64>(model_transform.cols[3].x) * 0.35 + static_cast<f64>(model_transform.cols[3].z) * 0.27, 6.283185307179586));
                                if (alpha_or_phase < 0.0f) alpha_or_phase += 6.2831853f;
                            }
                            constants.color_packed[1] = PackHalfPair(primitive.color[2], alpha_or_phase);
                            PackEmissive(primitive.emissive, primitive.emissive_texture, primitive.has_emissive_texture, constants.emissive_packed);
                            PackNormalMatrix(normal_transform, constants.normal_matrix_packed);
                            constants.material_textures = PackMaterialTextures(
                                primitive.base_color_texture, primitive.has_base_color_texture,
                                primitive.normal_texture, primitive.has_normal_texture,
                                primitive.metallic_roughness_texture, primitive.has_metallic_roughness_texture,
                                primitive.occlusion_texture, primitive.has_occlusion_texture);
                            constants.material_factors_0 = PackHalfPair(primitive.metallic, primitive.roughness);
                            constants.material_factors_1 = PackHalfPair(primitive.normal_scale, primitive.occlusion_strength);
                            if (primitive.water && primitive.bounds_radius >= 0.0f && primitive.bounds_height < 0.1f && (!have_water || primitive.bounds_radius > water_radius)) {
                                // A flat sheet: the world height of its bounds' centre is its level; the biggest sheet drawn is the one mirrored.
                                water_radius = primitive.bounds_radius;
                                const Vec4 centre = model_transform * Vec4(primitive.bounds_center[0], primitive.bounds_center[1], primitive.bounds_center[2], 1.0f);
                                have_water = true;
                                water_level = centre.y;
                            }
                            const u32 wind_zone_bits = primitive.wind_level != 0 && !wind_zones.empty()
                                                           ? WindZoneBitsAt(wind_zones, Vec3(model_transform.cols[3].x, model_transform.cols[3].y, model_transform.cols[3].z))
                                                           : 0u;
                            constants.alpha_options = PackMaterialOptions(primitive.alpha_mode, primitive.alpha_cutoff, primitive.extension_texture,
                                                                          primitive.has_extension_texture, primitive.double_sided, primitive.water, primitive.wind_level,
                                                                          wind_zone_bits, primitive.detail_level);
                            // Single-sided materials cull their back faces, except on a mirrored model (a negative scale turns the
                            // winding round): that draws both faces, as every model did before.
                            const bool cull = !primitive.double_sided && ModelDeterminant(model_transform) > 0.0f;
                            Mat4 inverse_model_transform;
                            const bool have_inverse = model_transform.TryInverse(inverse_model_transform);
                            if (have_inverse) {
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
                                                             constants, dx * dx + dy * dy + dz * dz, cull});
                            } else {
                                opaque_draws.push_back({frame_vertex_buffer, primitive.indices, primitive.index_count, constants, cull, model_transform,
                                                        primitive.wind_level == 0 && !primitive.water && !uploaded_animation});
                                if (transform_moved || previous_positions != kTaaNoPreviousPositions) {
                                    taa_movers.push_back({frame_vertex_buffer, primitive.indices, primitive.index_count,
                                                          static_cast<u32>(sizeof(SceneVertex)), model_transform,
                                                          previous_model.value_or(model_transform), previous_positions});
                                }
                                if (sun_shadows) {
                                    ShadowCaster caster;
                                    caster.vertices = frame_vertex_buffer;
                                    caster.indices = primitive.indices;
                                    caster.index_count = primitive.index_count;
                                    caster.model = model_transform;
                                    if (primitive.bounds_radius >= 0.0f && primitive.frame_vertex_buffers.empty()) {
                                        const Vec4 c = model_transform * Vec4(primitive.bounds_center[0], primitive.bounds_center[1],
                                                                              primitive.bounds_center[2], 1.0f);
                                        const f32 scale = std::max({Vec3(model_transform.cols[0].x, model_transform.cols[0].y, model_transform.cols[0].z).Length(),
                                                                    Vec3(model_transform.cols[1].x, model_transform.cols[1].y, model_transform.cols[1].z).Length(),
                                                                    Vec3(model_transform.cols[2].x, model_transform.cols[2].y, model_transform.cols[2].z).Length()});
                                        caster.bounds_center = Vec3(c.x, c.y, c.z);
                                        caster.bounds_radius = primitive.bounds_radius * scale;
                                    }
                                    caster.alpha_tested = primitive.alpha_mode == assets::MaterialAlphaMode::Mask;
                                    caster.base_color_texture = primitive.has_base_color_texture && primitive.base_color_texture.IsValid()
                                                                    ? primitive.base_color_texture.index
                                                                    : 255u;
                                    caster.alpha = primitive.color[3];
                                    caster.alpha_cutoff = primitive.alpha_cutoff;
                                    caster.deforming = uploaded_animation;
                                    // Vegetation that sways casts the shadow of where it is drawn: the sway vector in the model's space goes to the shadow shader, and the
                                    // caster is drawn into the cascades only (the pages hold what stands still), its bounds grown by the most it can move.
                                    if (primitive.wind_level != 0 && have_inverse && !renderer.frame_buffers.empty()) {
                                        const WindSample wind = WindForDraw(frame_data, wind_zone_bits);
                                        const f32 amplitude = static_cast<f32>(primitive.wind_level) * 0.02f * wind.strength;
                                        const Vec4 local = inverse_model_transform * Vec4(wind.direction[0] * amplitude, 0.0f, wind.direction[1] * amplitude, 0.0f);
                                        caster.sway[0] = local.x; caster.sway[1] = local.y; caster.sway[2] = local.z; caster.sway[3] = alpha_or_phase;
                                        caster.sway_time[0] = frame_data.time[0] * wind.gust_speed;
                                        caster.sway_time[1] = frame_data.wind_gusts[0];
                                        if (amplitude > 0.0f) {
                                            caster.deforming = true;
                                            if (caster.bounds_radius >= 0.0f) caster.bounds_radius += amplitude * 1.01f + 0.01f;
                                        }
                                    }
                                    shadow_casters.push_back(caster);
                                    if (renderer.rt_shadows && !caster.deforming && !caster.alpha_tested && primitive.frame_vertex_buffers.empty()) {
                                        if (!primitive.blas.IsValid() && !primitive.blas_failed) {
                                            try {
                                                primitive.blas = renderer.device.CreateBottomLevelAS(primitive.vertices, primitive.vertex_count,
                                                                                                    sizeof(SceneVertex), primitive.indices, primitive.index_count);
                                            } catch (const std::exception&) {
                                            }
                                            primitive.blas_failed = !primitive.blas.IsValid();
                                        }
                                        if (primitive.blas.IsValid()) {
                                            AccelerationInstance instance;
                                            instance.blas = primitive.blas;
                                            for (usize row = 0; row < 3; ++row) {
                                                for (usize column = 0; column < 4; ++column) {
                                                    const Vec4& c = model_transform.cols[column];
                                                    instance.transform[row * 4 + column] = row == 0 ? c.x : row == 1 ? c.y : c.z;
                                                }
                                            }
                                            rt_instances.push_back(instance);
                                        }
                                    }
                                    if (renderer.vsm_pages) {
                                        // The caster's world sphere (a skinned one's from its bind pose, grown): the pages it touches are redrawn when it is
                                        // new, has moved, or deforms, both where it is now and where it was.
                                        Vec3 sphere_center = caster.bounds_center;
                                        f32 sphere_radius = caster.bounds_radius;
                                        if (!(sphere_radius >= 0.0f) && primitive.skin_bounds_radius >= 0.0f) {
                                            const Vec4 c = model_transform * Vec4(primitive.skin_bounds_center[0], primitive.skin_bounds_center[1],
                                                                                  primitive.skin_bounds_center[2], 1.0f);
                                            const f32 scale = std::max({Vec3(model_transform.cols[0].x, model_transform.cols[0].y, model_transform.cols[0].z).Length(),
                                                                        Vec3(model_transform.cols[1].x, model_transform.cols[1].y, model_transform.cols[1].z).Length(),
                                                                        Vec3(model_transform.cols[2].x, model_transform.cols[2].y, model_transform.cols[2].z).Length()});
                                            sphere_center = Vec3(c.x, c.y, c.z);
                                            sphere_radius = primitive.skin_bounds_radius * scale;
                                        }
                                        Impl::VsmCasterState& state = renderer.vsm_casters[{entities[row].index, entities[row].generation,
                                                                                            static_cast<u32>(node.node_index),
                                                                                            static_cast<u32>(primitive_index) + 1}];
                                        const bool seen = state.frame + 1 == transform_frame;
                                        const bool dynamic = caster.deforming; // a posed skin, or vegetation swaying in the wind: the cascades have it, the pages do not
                                        if (sphere_radius >= 0.0f) {
                                            // A deforming caster is in no page (the cascades have it), and the pages under it are left out of the
                                            // table; one that starts or stops deforming changes what its pages hold, so they are redrawn.
                                            if (dynamic) vsm_suppress.push_back({sphere_center, sphere_radius});
                                            const bool changed_kind = seen && state.dynamic != dynamic;
                                            const bool moved = !dynamic && (!seen || TaaTransformMoved(state.model, model_transform));
                                            if (moved || changed_kind) {
                                                vsm_dirty.push_back({sphere_center, sphere_radius});
                                                if (seen && !state.dynamic && state.radius >= 0.0f) vsm_dirty.push_back({state.center, state.radius});
                                            }
                                        }
                                        state = {model_transform, sphere_center, sphere_radius, transform_frame, dynamic};
                                    }
                                }
                            }
                        }
                    }
                }
            }
        });
    }
    // Forget the nodes not drawn this frame (destroyed entities, culled models): they start over when they come back.
    std::erase_if(renderer.previous_transforms, [&](const auto& entry) { return entry.second.frame != transform_frame; });
    std::erase_if(renderer.previous_poses, [&](const auto& entry) { return entry.second.frame != transform_frame; });
    // The skinned movers' last-frame poses, in this frame's slot (the frame loop waited on its fence; a slot too small is replaced, the
    // old buffer retired once the GPU is done with it).
    BufferHandle motion_position_buffer;
    if (!renderer.motion_positions.empty()) {
        if (renderer.motion_position_buffers.empty()) {
            renderer.motion_position_buffers.resize(Impl::kFrameBuffers);
            renderer.motion_position_capacity.assign(Impl::kFrameBuffers, 0);
        }
        const usize slot = frame_index % Impl::kFrameBuffers;
        const u64 bytes = renderer.motion_positions.size() * sizeof(f32);
        if (renderer.motion_position_capacity[slot] < bytes) {
            if (renderer.motion_position_buffers[slot].IsValid()) renderer.device.DestroyBuffer(renderer.motion_position_buffers[slot]);
            const u64 capacity = std::max<u64>(bytes + bytes / 2, 64 * 1024);
            renderer.motion_position_buffers[slot] = renderer.device.CreateFrameConstantBuffer(capacity);
            renderer.motion_position_capacity[slot] = renderer.motion_position_buffers[slot].IsValid() ? capacity : 0;
        }
        motion_position_buffer = renderer.motion_position_buffers[slot];
        if (motion_position_buffer.IsValid() &&
            !renderer.device.WriteBufferUnsynchronized(motion_position_buffer, renderer.motion_positions.data(), bytes)) {
            renderer.device.UpdateFrameConstantBuffer(motion_position_buffer, renderer.motion_positions.data(), bytes);
        }
    }
    // The sun's shadow atlas, outside any pass; then this frame's constants (lights and cascades) and the scene pass itself.
    if (sun_shadows && renderer.rt_shadows) {
        // The structure of this frame's rigid casters, rebuilt only when one is new, gone or has moved. The shader reads shadow_info.x < 128 as
        // "shadows on"; no cascades are drawn.
        if (rt_instances.empty()) {
            AccelerationInstance instance;
            instance.blas = renderer.rt_dummy_blas;
            rt_instances.push_back(instance);
        }
        std::vector<f32> signature;
        signature.reserve(rt_instances.size() * 13);
        for (const AccelerationInstance& instance : rt_instances) {
            signature.push_back(static_cast<f32>(instance.blas.index));
            signature.insert(signature.end(), instance.transform, instance.transform + 12);
        }
        if (!renderer.rt_tlas.IsValid() || signature != renderer.rt_signature) {
            const AccelerationStructureHandle made = renderer.device.CreateTopLevelAS(rt_instances);
            if (made.IsValid()) {
                if (renderer.rt_tlas.IsValid()) renderer.device.DestroyAccelerationStructure(renderer.rt_tlas);
                renderer.rt_tlas = made;
                renderer.rt_signature = std::move(signature);
                ++renderer.rt_rebuilds;
            }
        }
        renderer.rt_instances = static_cast<u32>(rt_instances.size());
        frame_data.shadow_info[0] = 0u;
        frame_data.shadow_info[1] = frame_data.shadow_info[2] = frame_data.shadow_info[3] = 0u;
    } else if (sun_shadows) {
        const Vec3 light_travel(-frame_data.sun_direction[0], -frame_data.sun_direction[1], -frame_data.sun_direction[2]);
        const ShadowFrameData shadow = renderer.shadows->Render(commands, shadow_view, light_travel, shadow_casters);
        std::memcpy(frame_data.shadow_view_projection, shadow.view_projection, sizeof(shadow.view_projection));
        std::memcpy(frame_data.shadow_cascade, shadow.cascade, sizeof(shadow.cascade));
        std::memcpy(frame_data.shadow_info, shadow.info, sizeof(shadow.info));
    }
    // The sun's virtual shadow pages: what the feedback of a few frames ago asked for gets physical pages, the pages that need it are drawn, and
    // the page table goes to the light buffer for the scene shader. Without any of that the cascades alone shadow the sun.
    bool vsm_record = false;
    VsmFeedbackParams vsm_feedback_params;
    renderer.vsm_stats.drawn_pages = renderer.vsm_stats.caster_draws = renderer.vsm_stats.requests = renderer.vsm_stats.suppressed = 0;
    const auto vsm_start = std::chrono::steady_clock::now();
    if (renderer.vsm_pages) {
        VsmPageAllocator& pages = *renderer.vsm_pages;
        // Casters no longer drawn (destroyed, hidden, out of reach) leave their pages stale where they were.
        for (auto it = renderer.vsm_casters.begin(); it != renderer.vsm_casters.end();) {
            if (it->second.frame == transform_frame) {
                ++it;
                continue;
            }
            if (it->second.radius >= 0.0f && !it->second.dynamic) vsm_dirty.push_back({it->second.center, it->second.radius});
            it = renderer.vsm_casters.erase(it);
        }
        if (!sun_shadows || !have_camera || !light_buffer.IsValid()) {
            pages.InvalidateAll(); // nothing is kept up to date while the sun's shadows are off
            renderer.vsm_casters.clear();
        } else {
            const Vec3 light_travel(-frame_data.sun_direction[0], -frame_data.sun_direction[1], -frame_data.sun_direction[2]);
            const VsmLight light = VsmLightBasis(light_travel);
            if (!renderer.vsm_have_light || light.direction.Dot(renderer.vsm_light.direction) < 0.99999f) pages.InvalidateAll(); // the sun turned
            renderer.vsm_light = light;
            renderer.vsm_have_light = true;
            renderer.vsm_pass->EnsureAtlas(renderer.vsm_pool); // made before the first page is drawn, so the first frame can ask for pages
            pages.BeginFrame(++renderer.vsm_frame);
            const std::vector<VsmRequest> requests = renderer.vsm_feedback->Collect(frame_index);
            renderer.vsm_stats.requests = static_cast<u32>(requests.size());
            pages.Request(requests);
            for (const DirtySphere& sphere : vsm_dirty) pages.InvalidateSphere(light, sphere.center, sphere.radius);
            const std::vector<VsmPageDraw> draws = pages.TakeDraws(renderer.vsm_draw_budget);
            renderer.vsm_stats.drawn_pages = static_cast<u32>(draws.size());
            renderer.vsm_stats.caster_draws = renderer.vsm_pass->DrawPages(commands, light, draws, renderer.vsm_pool, shadow_casters);
            const auto origins = VsmWindowOrigins(light, decal_camera_position);
            u32 suppressed = 0;
            const std::vector<u32> table = pages.BuildPageTable(origins, light, vsm_suppress, &suppressed);
            renderer.vsm_stats.suppressed = suppressed;
            if (renderer.vsm_pass->Atlas().IsValid() &&
                renderer.device.WriteBufferUnsynchronized(light_buffer, table.data(), table.size() * sizeof(u32), kVsmTableOffset)) {
                const auto set = [](f32 (&out)[4], const Vec3& v, f32 w) { out[0] = v.x, out[1] = v.y, out[2] = v.z, out[3] = w; };
                const f32 height = static_cast<f32>(std::max(renderer.swap_chain.Height(), 1u));
                const bool perspective = camera != nullptr && camera->projection == Projection::Perspective;
                const f32 footprint_scale =
                    perspective ? 2.0f * std::tan(camera->fov_degrees * 0.5f * 3.14159265f / 180.0f) / height : 0.0f;
                const f32 footprint_constant = !perspective && camera != nullptr ? camera->ortho_height / height : 0.0f;
                set(frame_data.vsm_light_right, light.right, 0.0f);
                set(frame_data.vsm_light_up, light.up, 0.0f);
                set(frame_data.vsm_light_direction, light.direction, 0.0f);
                set(frame_data.vsm_camera, decal_camera_position, footprint_scale);
                frame_data.vsm_params[0] = footprint_constant;
                for (u32 level = 0; level < kVsmLevels; ++level) {
                    frame_data.vsm_window[level / 2][(level % 2) * 2] = origins[level][0];
                    frame_data.vsm_window[level / 2][(level % 2) * 2 + 1] = origins[level][1];
                }
                frame_data.vsm_info[0] = renderer.vsm_pass->Atlas().index;
                frame_data.vsm_info[1] = renderer.vsm_pool;
                frame_data.vsm_info[2] = static_cast<u32>(kVsmTableOffset);
                Mat4 inverse;
                if (taa_view_projection.TryInverse(inverse)) {
                    vsm_feedback_params.inverse_view_projection = inverse;
                    vsm_feedback_params.camera_position = decal_camera_position;
                    vsm_feedback_params.footprint_scale = footprint_scale;
                    vsm_feedback_params.footprint_constant = footprint_constant;
                    vsm_feedback_params.light = light;
                    vsm_feedback_params.depth_texture = renderer.swap_chain.SceneDepthTexture().index;
                    vsm_feedback_params.screen_width = renderer.swap_chain.Width();
                    vsm_feedback_params.screen_height = renderer.swap_chain.Height();
                    vsm_feedback_params.frame = static_cast<u32>(renderer.vsm_frame);
                    vsm_record = true;
                }
            }
            const VsmPageStats stats = pages.Stats();
            renderer.vsm_stats.resident = stats.resident;
            renderer.vsm_stats.pending = stats.pending;
        }
    }
    renderer.vsm_stats.cpu_ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - vsm_start).count();
    // The water's reflection: when water was drawn and the camera is above it, the texture exists (the swap chain's size) and the frame constants name it; the pass
    // that draws it is recorded below, before the scene's.
    bool draw_reflection = false;
    if (renderer.reflections && have_water && have_camera && frame_buffer.IsValid() && decal_camera_position.y > water_level + 0.05f &&
        std::any_of(opaque_draws.begin(), opaque_draws.end(), [](const PendingOpaqueDraw& draw) { return draw.reflect; })) {
        const u32 width = renderer.swap_chain.Width(), height = renderer.swap_chain.Height();
        if (!renderer.reflection_texture.IsValid() || renderer.reflection_width != width || renderer.reflection_height != height) {
            if (renderer.reflection_texture.IsValid()) renderer.device.DestroyTexture(renderer.reflection_texture);
            renderer.reflection_texture = renderer.device.CreateRenderTextureWithDepth(width, height);
            renderer.reflection_width = width;
            renderer.reflection_height = height;
            if (!renderer.reflection_texture.IsValid()) renderer.reflections = false; // this device cannot: no reflections, and no retry each frame
        }
        if (renderer.reflections && !renderer.reflection_pipeline.IsValid() && !renderer.reflection_failed) {
            try {
                renderer.reflection_pipeline = renderer.device.CreatePipeline(renderer.reflection_desc, renderer.swap_chain);
            } catch (const std::exception& error) {
                AETHER_LOG_WARN("PlayerRenderer", "Water reflections unavailable (%s)", error.what());
            }
            renderer.reflection_failed = !renderer.reflection_pipeline.IsValid();
        }
        if (renderer.reflections && renderer.reflection_frame_buffers.empty()) {
            for (usize i = 0; i < renderer.frame_buffers.size(); ++i) renderer.reflection_frame_buffers.push_back(renderer.device.CreateFrameConstantBuffer(sizeof(FrameData)));
        }
        draw_reflection = renderer.reflections && renderer.reflection_texture.IsValid() && renderer.reflection_pipeline.IsValid() &&
                          !renderer.reflection_frame_buffers.empty() && renderer.reflection_frame_buffers[0].IsValid();
        if (draw_reflection) {
            frame_data.reflection[0] = renderer.reflection_texture.index;
            frame_data.reflection[2] = width; // the texture is the swap chain's size: a pixel's position over it is its place in the texture
            frame_data.reflection[3] = height;
        }
    }
    // This frame's slot: the frame loop waited on its fence, so it is written without waiting for the GPU (WriteBufferUnsynchronized).
    if (frame_buffer.IsValid() && !renderer.device.WriteBufferUnsynchronized(frame_buffer, &frame_data, sizeof(frame_data))) {
        renderer.device.UpdateFrameConstantBuffer(frame_buffer, &frame_data, sizeof(frame_data));
    }
    BufferHandle reflection_frame_buffer;
    if (draw_reflection) {
        reflection_frame_buffer = renderer.reflection_frame_buffers[frame_index % renderer.reflection_frame_buffers.size()];
        FrameData mirror_frame = frame_data;
        mirror_frame.reflection[1] = 1u; // the mirror pass
        if (!renderer.device.WriteBufferUnsynchronized(reflection_frame_buffer, &mirror_frame, sizeof(mirror_frame))) {
            renderer.device.UpdateFrameConstantBuffer(reflection_frame_buffer, &mirror_frame, sizeof(mirror_frame));
        }
    }
    if (light_buffer.IsValid() && frame_data.cluster_info[3] != 0 && !renderer.clusters.bytes.empty() &&
        !renderer.device.WriteBufferUnsynchronized(light_buffer, renderer.clusters.bytes.data(), renderer.clusters.bytes.size())) {
        renderer.device.UpdateFrameConstantBuffer(light_buffer, renderer.clusters.bytes.data(), renderer.clusters.bytes.size());
    }
    if (draw_reflection) {
        // The world mirrored about y = water_level, seen from the real camera (so a water pixel finds its reflection at its own screen position), with the
        // projection's near plane tilted onto the water's plane so nothing below it is drawn (an oblique frustum, as for depth [0, 1]: the third row of the
        // projection is the clip plane, scaled so the far plane stays where it was). Lit as the real surfaces are: only where they are drawn is mirrored.
        const f32 level = water_level;
        Mat4 mirror;
        mirror.cols[0] = Vec4(1.0f, 0.0f, 0.0f, 0.0f);
        mirror.cols[2] = Vec4(0.0f, 0.0f, 1.0f, 0.0f);
        mirror.cols[1] = Vec4(0.0f, -1.0f, 0.0f, 0.0f);
        mirror.cols[3] = Vec4(0.0f, 2.0f * level, 0.0f, 1.0f);
        Mat4 inverse_view, inverse_projection;
        Mat4 projection = water_projection;
        if (post_camera_view.TryInverse(inverse_view) && water_projection.TryInverse(inverse_projection)) {
            // The kept side, in the mirrored world's coordinates: y' <= level (a little below the water too, so the shore has no gap).
            const Vec4 plane_world(0.0f, -1.0f, 0.0f, level + 0.05f);
            const Vec4 plane(Dot4(inverse_view.cols[0], plane_world), Dot4(inverse_view.cols[1], plane_world), Dot4(inverse_view.cols[2], plane_world),
                             Dot4(inverse_view.cols[3], plane_world));
            const Vec4 corner = inverse_projection * Vec4(plane.x >= 0.0f ? 1.0f : -1.0f, plane.y >= 0.0f ? 1.0f : -1.0f, 1.0f, 1.0f);
            const f32 scale = Dot4(plane, corner);
            if (std::abs(scale) > 1e-6f) {
                projection.cols[0].z = plane.x / scale;
                projection.cols[1].z = plane.y / scale;
                projection.cols[2].z = plane.z / scale;
                projection.cols[3].z = plane.w / scale;
            }
        }
        const Mat4 mirrored_view_projection = projection * post_camera_view * mirror;
        const ClearColor clear{0.0f, 0.0f, 0.0f, 0.0f};
        if (commands.BeginTexturePass(renderer.reflection_texture, &clear)) {
            commands.BindPipeline(renderer.reflection_pipeline);
            commands.BindFrameConstants(reflection_frame_buffer);
            if (light_buffer.IsValid()) commands.BindFrameReadBuffer(light_buffer);
            commands.BindBindlessTextures();
            for (const PendingOpaqueDraw& draw : opaque_draws) {
                if (!draw.reflect) continue;
                DrawConstants constants = draw.constants;
                constants.mvp = mirrored_view_projection * draw.model;
                commands.BindVertexBuffer(draw.vertices, sizeof(SceneVertex));
                commands.BindIndexBuffer(draw.indices, IndexFormat::UInt32);
                commands.SetPushConstants(&constants, sizeof(constants));
                commands.DrawIndexed(draw.index_count);
            }
            commands.EndRenderPass();
        }
    }
    renderer.GpuStamp(commands, 1);
    if (renderer.hdr) {
        if (!commands.BeginSceneColorPass(renderer.swap_chain, kBackgroundLinear)) return;
    } else {
        commands.BeginRenderPass(renderer.swap_chain, kBackground);
    }
    if (draw_sky) renderer.sky->Draw(commands, shadow_view, frame_buffer);
    if (!opaque_draws.empty()) {
        // Culled draws first, then the two-sided ones: one pipeline switch.
        std::stable_partition(opaque_draws.begin(), opaque_draws.end(), [](const PendingOpaqueDraw& draw) { return draw.cull; });
        bool bound = false, bound_cull = false;
        for (const PendingOpaqueDraw& draw : opaque_draws) {
            const bool cull = draw.cull && renderer.culled_pipeline.IsValid();
            if (!bound || cull != bound_cull) {
                commands.BindPipeline(cull ? renderer.culled_pipeline : renderer.pipeline);
                if (frame_buffer.IsValid()) commands.BindFrameConstants(frame_buffer);
                if (light_buffer.IsValid()) commands.BindFrameReadBuffer(light_buffer);
                if (renderer.rt_shadows && renderer.rt_tlas.IsValid()) commands.BindAccelerationStructure(renderer.rt_tlas);
                commands.BindBindlessTextures();
                bound = true;
                bound_cull = cull;
            }
            commands.BindVertexBuffer(draw.vertices, sizeof(SceneVertex));
            commands.BindIndexBuffer(draw.indices, IndexFormat::UInt32);
            commands.SetPushConstants(&draw.constants, sizeof(draw.constants));
            commands.DrawIndexed(draw.index_count);
        }
    }
    // Decals paint over the opaque surfaces' lit colour, before the transparent ones. The pass copies the scene depth first, so the transparent
    // draws below rebind their own pipeline, frame constants and texture table.
    if (have_camera && renderer.decals && renderer.decals->Valid()) {
        const std::vector<DecalPassItem> decal_items =
            renderer.GatherDecals(world, guids, decal_view_projection, decal_camera_position);
        renderer.decals->Draw(commands, decal_items);
    }
    if (!transparent_draws.empty()) {
        std::stable_sort(transparent_draws.begin(), transparent_draws.end(),
                         [](const PendingTransparentDraw& left, const PendingTransparentDraw& right) {
                             return left.distance_squared > right.distance_squared;
                         });
        const PipelineHandle transparent_pipeline = renderer.transparent_pipeline.IsValid()
                                                        ? renderer.transparent_pipeline
                                                        : renderer.pipeline;
        // Back to front, switching between the culled and two-sided pipelines as the draws need.
        bool bound = false, bound_cull = false;
        for (const PendingTransparentDraw& draw : transparent_draws) {
            const bool cull = draw.cull && renderer.transparent_culled_pipeline.IsValid() && renderer.transparent_pipeline.IsValid();
            if (!bound || cull != bound_cull) {
                commands.BindPipeline(cull ? renderer.transparent_culled_pipeline : transparent_pipeline);
                if (frame_buffer.IsValid()) commands.BindFrameConstants(frame_buffer);
                if (light_buffer.IsValid()) commands.BindFrameReadBuffer(light_buffer);
                if (renderer.rt_shadows && renderer.rt_tlas.IsValid()) commands.BindAccelerationStructure(renderer.rt_tlas);
                commands.BindBindlessTextures();
                bound = true;
                bound_cull = cull;
            }
            commands.BindVertexBuffer(draw.vertices, sizeof(SceneVertex));
            commands.BindIndexBuffer(draw.indices, IndexFormat::UInt32);
            commands.SetPushConstants(&draw.constants, sizeof(draw.constants));
            commands.DrawIndexed(draw.index_count);
        }
    }
    // Particles after every surface (they test against its depth without writing it). Soft sprites make the pass copy the depth
    // (ICommandList::CopySceneDepth): the transparent surfaces above don't write depth, so the copy is the opaque scene plus the mesh
    // particles; the HUD below binds its own pipeline.
    if (draw_particles || draw_sprite2d) {
        renderer.particles->Draw(commands, renderer.particle_data, particle_camera, particle_view_projection, particle_projection, frame_index);
    }
    if (renderer.hdr) {
        // The lit scene is in the HDR target: tone map it into the back buffer under the post volumes at the camera, then the HUD on top
        // in display space. Temporal anti-aliasing first blends it with the frames before (reading the depth copied here).
        RenderScene volumes;
        volumes.post_volumes = ExtractPostVolumes(world, guids);
        const PostProcessSettings post_settings = BlendPostProcess(volumes, have_camera ? decal_camera_position : Vec3(0, 0, 0));
        const bool resolve = (renderer.taa || vsm_record || renderer.ssao || post_settings.fog_density > 0.0f) && have_camera && commands.CopySceneDepth();
        commands.EndRenderPass();
        renderer.GpuStamp(commands, 2);
        if (resolve && vsm_record) renderer.vsm_feedback->Record(commands, vsm_feedback_params, frame_index);
        u32 resolved = 0;
        if (resolve && renderer.taa) {
            resolved = renderer.taa->Resolve(commands, taa_view_projection,
                                             (static_cast<u64>(camera_entity.index) << 32) | camera_entity.generation, taa_movers,
                                             motion_position_buffer);
        } else if (renderer.taa) {
            renderer.taa->Reset();
        }
        renderer.post->SetSource(resolved);
        if (resolve && (renderer.ssao || post_settings.fog_density > 0.0f) && renderer.swap_chain.SceneDepthTexture().IsValid()) {
            renderer.post->SetAmbientOcclusion({renderer.swap_chain.SceneDepthTexture().index, renderer.ssao ? renderer.ao_strength : 0.0f,
                                                renderer.ao_radius, ao_near, ao_far, ao_proj_x, ao_proj_y});
            renderer.post->SetFogColor(Vec3(frame_data.sky_color[0], frame_data.sky_color[1], frame_data.sky_color[2]));
            if (frame_data.sun_direction[3] > 0.0f) {
                const Vec4 to_sun = post_camera_view * Vec4(frame_data.sun_direction[0], frame_data.sun_direction[1], frame_data.sun_direction[2], 0.0f);
                const Vec3 direction = Vec3(to_sun.x, to_sun.y, to_sun.z).Normalized();
                renderer.post->SetFogSun(direction, Vec3(frame_data.sun_color[0], frame_data.sun_color[1], frame_data.sun_color[2]) * renderer.fog_sun_scatter);
            }
        }
        // Bloom reads the finished scene colour into its own chain of render textures, between the two passes.
        if (renderer.bloom) renderer.post->RenderBloom(commands, post_settings);
        commands.BeginRenderPass(renderer.swap_chain, kBackground);
        renderer.post->Draw(commands, post_settings);
    }
    if (!renderer.hdr) renderer.GpuStamp(commands, 2); // one pass: the scene and the HUD end together
    renderer.DrawHud(game, commands);
    commands.EndRenderPass();
    renderer.GpuTimingEnd(commands, renderer.transform_frame);
}

// The advanced PBR library (anisotropic GGX, sheen, clear coat) goes ahead of the scene shader.
std::string SceneShaderSource(bool with_frame_data, bool hdr_target, bool ray_traced_shadows) {
    return std::string(with_frame_data ? "#define AETHER_FRAME_DATA 1\n" : "") + (hdr_target ? "#define AETHER_HDR_TARGET 1\n" : "") +
           (with_frame_data && ray_traced_shadows ? "#define AETHER_RT_SHADOWS 1\n" : "") +
           mat::AdvancedPbrHlsl() + mat::ShadingModelsHlsl() + kSceneShader + (with_frame_data ? FrameDataHlsl() : "") + kSceneShaderVertex +
           (with_frame_data ? VsmShaderLibrary() : "") + (with_frame_data ? kSceneShaderVsm : "") + kSceneShaderFrame;
}

} // namespace aether::player
