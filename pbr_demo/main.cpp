// Roadmap item 2 (+ follow-ups: normal mapping, multiple/colored lights): a
// real PBR (physically-based) renderer — Cook-Torrance specular (GGX normal
// distribution, Smith geometry term, Schlick Fresnel approximation) plus a
// Lambertian diffuse term, metallic-roughness workflow. Textbook formulas
// (matching the widely-used LearnOpenGL/Sascha Willems reference
// derivations), not a simplified stand-in. Tangent-space normal mapping
// (procedural bump map, TBN built per-vertex from an analytic sphere
// tangent) perturbs the shading normal before the BRDF runs; four point
// lights with distinct colors and inverse-square falloff replace the
// original single directional light, summed per-pixel.
//
// Scene: the classic "material ball grid" used to visually validate a PBR
// implementation — a 7x7 grid of spheres, metallic varying 0->1 along one
// axis and roughness varying 0.05->1 along the other. Seeing the expected
// qualitative trends (rough dielectrics look chalky/diffuse, smooth metals
// show a sharp, tinted specular highlight, low-roughness anything gets a
// tight bright highlight, and now: visible surface bumps from the normal
// map, and each light's color separately visible on the near side of the
// grid it illuminates) is the actual verification here — a mathematically
// wrong BRDF still "renders", it just looks wrong, so this was checked by
// actually looking at rendered frames, not just by not crashing (see the
// screenshot capture note below).
//
// This is intentionally a separate project from sandbox/ (which stays
// focused on bindless textures + GPU-driven culling) rather than a rewrite
// of it — same reasoning as rhi_demo/ being separate from sandbox.
//
// Set AETHER_PBR_DEMO_MAX_FRAMES=<N> to auto-close after N frames instead of
// waiting for the window to be closed, for scripted/automated verification.
// Set AETHER_PBR_DEMO_SCREENSHOT=<path> to dump the final frame's backbuffer
// to a PNG on exit, for visual verification without a human watching the
// window live.

#include "aether/core/log.h"
#include "aether/gfx/buffer.h"
#include "aether/gfx/command_list.h"
#include "aether/gfx/descriptor_heap.h"
#include "aether/gfx/device.h"
#include "aether/gfx/render_graph.h"
#include "aether/gfx/shader_compiler.h"
#include "aether/gfx/swap_chain.h"
#include "aether/gfx/texture.h"
#include "aether/math/mat4.h"
#include "aether/math/math.h"
#include "aether/platform/filesystem.h"
#include "aether/platform/window.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace aether;
using namespace aether::gfx;

namespace {

struct PBRVertex {
    f32 pos[3];
    f32 normal[3];
    f32 uv[2];
    f32 tangent[3];
};

// A unit UV sphere (radius baked in via `radius`), position == normalized
// normal since it's centered at the origin — no separate normal computation
// needed. The tangent is the analytic partial derivative of position with
// respect to theta (longitude) — d/dtheta (sin(phi)cos(theta), 0,
// sin(phi)sin(theta)) directionally reduces to (-sin(theta), 0, cos(theta)),
// independent of phi — i.e. "the direction u increases in", which is exactly
// what a tangent needs to be for the TBN basis normal mapping builds
// per-pixel. Degenerates at the poles (where the whole notion of "which way
// is east" is ill-defined) but stays a valid unit vector everywhere, which
// is enough for a demo.
void GenerateSphere(f32 radius, u32 stacks, u32 slices, std::vector<PBRVertex>& out_vertices,
                     std::vector<u32>& out_indices) {
    for (u32 i = 0; i <= stacks; ++i) {
        f32 v = static_cast<f32>(i) / static_cast<f32>(stacks);
        f32 phi = v * kPi;
        f32 sin_phi = std::sin(phi);
        f32 cos_phi = std::cos(phi);
        for (u32 j = 0; j <= slices; ++j) {
            f32 u = static_cast<f32>(j) / static_cast<f32>(slices);
            f32 theta = u * 2.0f * kPi;
            f32 cos_theta = std::cos(theta);
            f32 sin_theta = std::sin(theta);
            f32 x = sin_phi * cos_theta;
            f32 y = cos_phi;
            f32 z = sin_phi * sin_theta;
            PBRVertex vertex{};
            vertex.pos[0] = x * radius;
            vertex.pos[1] = y * radius;
            vertex.pos[2] = z * radius;
            vertex.normal[0] = x;
            vertex.normal[1] = y;
            vertex.normal[2] = z;
            vertex.uv[0] = u;
            vertex.uv[1] = v;
            vertex.tangent[0] = -sin_theta;
            vertex.tangent[1] = 0.0f;
            vertex.tangent[2] = cos_theta;
            out_vertices.push_back(vertex);
        }
    }
    for (u32 i = 0; i < stacks; ++i) {
        for (u32 j = 0; j < slices; ++j) {
            u32 a = i * (slices + 1) + j;
            u32 b = a + slices + 1;
            out_indices.push_back(a);
            out_indices.push_back(b);
            out_indices.push_back(a + 1);
            out_indices.push_back(b);
            out_indices.push_back(b + 1);
            out_indices.push_back(a + 1);
        }
    }
}

// A procedural tangent-space normal map (a grid of smooth wave bumps),
// generated analytically rather than loaded from a file: the height field is
// h(u,v) = sin(u)*cos(v), and the tangent-space normal at each texel is
// exactly (-dh/dx, -dh/dy, 1) normalized — this is the standard
// height-to-normal-map derivation, computed here in closed form instead of
// via a finite-difference Sobel pass since h is a known analytic function.
std::vector<u8> GenerateBumpNormalMap(u32 size, f32 frequency, f32 strength) {
    std::vector<u8> pixels(static_cast<usize>(size) * size * 4);
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            f32 u = static_cast<f32>(x) / static_cast<f32>(size) * frequency * 2.0f * kPi;
            f32 v = static_cast<f32>(y) / static_cast<f32>(size) * frequency * 2.0f * kPi;
            f32 scale = frequency * 2.0f * kPi / static_cast<f32>(size);
            f32 dh_du = std::cos(u) * std::cos(v) * scale;
            f32 dh_dv = -std::sin(u) * std::sin(v) * scale;

            Vec3 n = Vec3(-dh_du * strength, -dh_dv * strength, 1.0f).Normalized();
            u8* p = &pixels[(static_cast<usize>(y) * size + x) * 4];
            p[0] = static_cast<u8>((n.x * 0.5f + 0.5f) * 255.0f);
            p[1] = static_cast<u8>((n.y * 0.5f + 0.5f) * 255.0f);
            p[2] = static_cast<u8>((n.z * 0.5f + 0.5f) * 255.0f);
            p[3] = 255;
        }
    }
    return pixels;
}

// --------------------------------------------------------------------------
// Environment cubemap + diffuse-irradiance convolution (image-based
// lighting) and specular environment reflections.
// --------------------------------------------------------------------------

// Analytic sky: a horizon->zenith gradient plus a darker ground half,
// evaluated directly as a function of direction — no image asset needed, and
// (unlike the normal map's height field) there's no derivative to take here,
// just a direct color function of `dir`.
Vec3 SkyColor(Vec3 dir) {
    dir = dir.Normalized();
    f32 t = std::pow(std::max(dir.y, 0.0f), 0.5f);
    Vec3 horizon(0.85f, 0.80f, 0.70f);
    Vec3 zenith(0.15f, 0.35f, 0.75f);
    Vec3 sky = horizon + (zenith - horizon) * t;
    if (dir.y < 0.0f) {
        f32 g = std::min(-dir.y * 2.0f, 1.0f);
        Vec3 ground(0.20f, 0.18f, 0.15f);
        sky = horizon + (ground - horizon) * g;
    }
    return sky;
}

// Standard D3D cubemap face-to-direction mapping (array slice order +X, -X,
// +Y, -Y, +Z, -Z), (u,v) in [0,1) across each face.
Vec3 CubeFaceDirection(u32 face, f32 u, f32 v) {
    f32 s = 2.0f * u - 1.0f;
    f32 t = 2.0f * v - 1.0f;
    switch (face) {
        case 0: return Vec3(1.0f, -t, -s);
        case 1: return Vec3(-1.0f, -t, s);
        case 2: return Vec3(s, 1.0f, t);
        case 3: return Vec3(s, -1.0f, -t);
        case 4: return Vec3(s, -t, 1.0f);
        default: return Vec3(-s, -t, -1.0f);
    }
}

std::vector<std::vector<u8>> GenerateSkyCubeFaces(u32 size) {
    std::vector<std::vector<u8>> faces(6);
    for (u32 face = 0; face < 6; ++face) {
        std::vector<u8>& pixels = faces[face];
        pixels.resize(static_cast<usize>(size) * size * 4);
        for (u32 y = 0; y < size; ++y) {
            for (u32 x = 0; x < size; ++x) {
                f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(size);
                f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(size);
                Vec3 color = SkyColor(CubeFaceDirection(face, u, v));
                u8* p = &pixels[(static_cast<usize>(y) * size + x) * 4];
                p[0] = static_cast<u8>(std::min(color.x, 1.0f) * 255.0f);
                p[1] = static_cast<u8>(std::min(color.y, 1.0f) * 255.0f);
                p[2] = static_cast<u8>(std::min(color.z, 1.0f) * 255.0f);
                p[3] = 255;
            }
        }
    }
    return faces;
}

// Diffuse-irradiance convolution: for each output texel's direction N,
// integrates incoming radiance over the hemisphere around N, cosine-weighted
// (the standard diffuse-irradiance formula, matching the widely-used
// LearnOpenGL IBL derivation — same "textbook reference" approach as the
// BRDF and the normal-map derivative above). Evaluated directly against the
// analytic SkyColor() function rather than by texture-sampling the generated
// cube faces — mathematically the same integral, since SkyColor *is* the
// environment's radiance function; this just skips a redundant
// texture-into-CPU round trip. Deliberately low resolution (this is a
// low-frequency function by construction — convolving a whole hemisphere
// erases all high-frequency detail) and done once at startup, not per-frame.
std::vector<std::vector<u8>> GenerateIrradianceCubeFaces(u32 size) {
    constexpr u32 kPhiSteps = 64;
    constexpr u32 kThetaSteps = 16;

    std::vector<std::vector<u8>> faces(6);
    for (u32 face = 0; face < 6; ++face) {
        std::vector<u8>& pixels = faces[face];
        pixels.resize(static_cast<usize>(size) * size * 4);
        for (u32 y = 0; y < size; ++y) {
            for (u32 x = 0; x < size; ++x) {
                f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(size);
                f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(size);
                Vec3 N = CubeFaceDirection(face, u, v).Normalized();

                Vec3 up = std::abs(N.y) < 0.999f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
                Vec3 right = up.Cross(N).Normalized();
                up = N.Cross(right);

                Vec3 irradiance(0.0f, 0.0f, 0.0f);
                for (u32 pi = 0; pi < kPhiSteps; ++pi) {
                    f32 phi = (static_cast<f32>(pi) / static_cast<f32>(kPhiSteps)) * 2.0f * kPi;
                    for (u32 ti = 0; ti < kThetaSteps; ++ti) {
                        f32 theta = (static_cast<f32>(ti) / static_cast<f32>(kThetaSteps)) * (kPi * 0.5f);
                        f32 st = std::sin(theta);
                        f32 ct = std::cos(theta);
                        Vec3 tangent_sample(st * std::cos(phi), st * std::sin(phi), ct);
                        Vec3 sample_dir = right * tangent_sample.x + up * tangent_sample.y + N * tangent_sample.z;
                        irradiance = irradiance + SkyColor(sample_dir) * (ct * st);
                    }
                }
                irradiance = irradiance * (kPi / static_cast<f32>(kPhiSteps * kThetaSteps));

                u8* p = &pixels[(static_cast<usize>(y) * size + x) * 4];
                p[0] = static_cast<u8>(std::min(irradiance.x, 1.0f) * 255.0f);
                p[1] = static_cast<u8>(std::min(irradiance.y, 1.0f) * 255.0f);
                p[2] = static_cast<u8>(std::min(irradiance.z, 1.0f) * 255.0f);
                p[3] = 255;
            }
        }
    }
    return faces;
}

struct CubemapTexture {
    ComPtr<ID3D12Resource> resource;
    // Kept alive for the CubemapTexture's own lifetime rather than freed
    // once the copy completes — same demo-scale trade-off gfx::Texture's
    // upload_staging_ member documents (engine/include/aether/gfx/texture.h).
    ComPtr<ID3D12Resource> upload_staging;
    u32 srv_index = DescriptorHeap::kInvalidIndex;
};

// Uploads a 6-face RGBA8 cubemap — DescriptorHeap-allocated SRV, one
// DEFAULT-heap resource with 6 array slices (one subresource per face, since
// MipLevels=1), one UPLOAD-heap staging buffer sized for all 6 faces at
// once. Mirrors gfx::Texture's single-face upload pattern
// (engine/src/gfx/texture.cpp), generalized to 6 subresources.
CubemapTexture CreateCubemapTexture(Device& device, DescriptorHeap& heap, ID3D12GraphicsCommandList* upload_cmd,
                                     u32 face_size, const std::vector<std::vector<u8>>& faces) {
    CubemapTexture result;

    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC tex_desc{};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width = face_size;
    tex_desc.Height = face_size;
    tex_desc.DepthOrArraySize = 6;
    tex_desc.MipLevels = 1;
    tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    AETHER_D3D_CHECK(device.Handle()->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
                                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                               IID_PPV_ARGS(&result.resource)));

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[6]{};
    UINT num_rows[6]{};
    UINT64 row_sizes[6]{};
    UINT64 total_bytes = 0;
    device.Handle()->GetCopyableFootprints(&tex_desc, 0, 6, 0, footprints, num_rows, row_sizes, &total_bytes);

    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC upload_desc{};
    upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upload_desc.Width = total_bytes;
    upload_desc.Height = 1;
    upload_desc.DepthOrArraySize = 1;
    upload_desc.MipLevels = 1;
    upload_desc.Format = DXGI_FORMAT_UNKNOWN;
    upload_desc.SampleDesc.Count = 1;
    upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    AETHER_D3D_CHECK(device.Handle()->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                               IID_PPV_ARGS(&result.upload_staging)));
    ID3D12Resource* staging = result.upload_staging.Get();

    u8* mapped = nullptr;
    D3D12_RANGE no_read{0, 0};
    AETHER_D3D_CHECK(staging->Map(0, &no_read, reinterpret_cast<void**>(&mapped)));
    for (u32 face = 0; face < 6; ++face) {
        for (u32 row = 0; row < face_size; ++row) {
            std::memcpy(mapped + footprints[face].Offset + static_cast<u64>(row) * footprints[face].Footprint.RowPitch,
                        faces[face].data() + static_cast<u64>(row) * face_size * 4, static_cast<usize>(face_size) * 4);
        }
    }
    staging->Unmap(0, nullptr);

    for (u32 face = 0; face < 6; ++face) {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = result.resource.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = face;

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = staging;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprints[face];

        upload_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    D3D12_RESOURCE_BARRIER barrier = TransitionBarrier(result.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    upload_cmd->ResourceBarrier(1, &barrier);

    result.srv_index = heap.Allocate();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = tex_desc.Format;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.TextureCube.MipLevels = 1;
    device.Handle()->CreateShaderResourceView(result.resource.Get(), &srv_desc, heap.CPUHandle(result.srv_index));

    return result;
}

// Cook-Torrance GGX/Smith/Schlick BRDF, standard metallic-roughness
// workflow. Per-instance data (model matrix + material params) comes
// through root constants (one draw call per sphere — this demo's scope is
// the lighting model, not draw-call batching, which sandbox/ already covers
// via GPU-driven indirect draws); per-frame data (view-proj, camera, light,
// albedo) through a root CBV.
constexpr const char* kPBRShaderSource = R"(
cbuffer InstanceConstants : register(b0) {
    float4x4 g_Model;
    float g_Metallic;
    float g_Roughness;
};

struct PointLight {
    float3 position;
    float _pad0;
    float3 color;
    float _pad1;
};

#define kNumLights 4

cbuffer FrameConstants : register(b1) {
    float4x4 g_ViewProj;
    float3 g_CameraPos;
    float _Pad0;
    float3 g_Albedo;
    float _Pad1;
    PointLight g_Lights[kNumLights];
};

Texture2D g_NormalMap : register(t0);
TextureCube g_EnvironmentMap : register(t1);
TextureCube g_IrradianceMap : register(t2);
SamplerState g_Sampler : register(s0);

struct VSInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float3 tangent : TANGENT;
};

struct PSInput {
    float4 position : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD1;
    float3 tangent : TANGENT;
};

PSInput VSMain(VSInput input) {
    PSInput result;
    float4 worldPos = mul(g_Model, float4(input.position, 1.0));
    result.worldPos = worldPos.xyz;
    result.position = mul(g_ViewProj, worldPos);
    result.normal = mul((float3x3)g_Model, input.normal);
    result.tangent = mul((float3x3)g_Model, input.tangent);
    result.uv = input.uv;
    return result;
}

static const float kPi = 3.14159265359;

float DistributionGGX(float3 N, float3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = kPi * denom * denom;
    return a2 / max(denom, 1e-7);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

float3 FresnelSchlick(float cosTheta, float3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float4 PSMain(PSInput input) : SV_TARGET {
    float3 N_geom = normalize(input.normal);
    // Gram-Schmidt re-orthogonalize the tangent against the interpolated
    // normal (linear interpolation across a triangle doesn't preserve
    // perpendicularity), then derive the bitangent — the standard TBN setup.
    float3 T = normalize(input.tangent - N_geom * dot(N_geom, input.tangent));
    float3 B = cross(N_geom, T);
    float3x3 TBN = float3x3(T, B, N_geom);

    float3 tangentNormal = g_NormalMap.Sample(g_Sampler, input.uv).rgb * 2.0 - 1.0;
    // mul(vector, matrix) treats the vector as a row vector in HLSL, i.e.
    // result = sum_i vector[i] * matrix.row[i] — with TBN's rows literally
    // T, B, N_geom (float3x3(T,B,N) sets rows, not columns), this computes
    // tangentNormal.x*T + tangentNormal.y*B + tangentNormal.z*N_geom: exactly
    // the tangent-space-to-world-space basis transform.
    float3 N = normalize(mul(tangentNormal, TBN));

    float3 V = normalize(g_CameraPos - input.worldPos);
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), g_Albedo, g_Metallic);

    float3 Lo = float3(0.0, 0.0, 0.0);
    [unroll]
    for (int i = 0; i < kNumLights; ++i) {
        float3 lightVec = g_Lights[i].position - input.worldPos;
        float dist = length(lightVec);
        float3 L = lightVec / max(dist, 1e-4);
        float3 H = normalize(V + L);
        float attenuation = 1.0 / max(dist * dist, 1e-4);
        float3 radiance = g_Lights[i].color * attenuation;

        float NDF = DistributionGGX(N, H, g_Roughness);
        float G = GeometrySmith(N, V, L, g_Roughness);
        float3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);

        float3 kS = F;
        float3 kD = (1.0 - kS) * (1.0 - g_Metallic);

        float3 numerator = NDF * G * F;
        float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 1e-4;
        float3 specular = numerator / denominator;

        float NdotL = max(dot(N, L), 0.0);
        Lo += (kD * g_Albedo / kPi + specular) * radiance * NdotL;
    }

    // Image-based lighting: diffuse from the precomputed irradiance
    // convolution (aether::CreateCubemapTexture / GenerateIrradianceCubeFaces
    // on the CPU, see the comment there), specular from a direct environment
    // reflection off the base (non-convolved) cubemap. The roughness fade on
    // the specular term is a deliberately simple stand-in for full
    // split-sum/prefiltered-mip specular IBL (not implemented) — it fades
    // reflections out for rough surfaces without actually blurring them.
    float3 ambientFresnel = FresnelSchlick(max(dot(N, V), 0.0), F0);
    float3 ambientKd = (1.0 - ambientFresnel) * (1.0 - g_Metallic);
    float3 irradiance = g_IrradianceMap.Sample(g_Sampler, N).rgb;
    float3 diffuseIBL = ambientKd * irradiance * g_Albedo;

    float3 R = reflect(-V, N);
    float3 envColor = g_EnvironmentMap.Sample(g_Sampler, R).rgb;
    float3 specularIBL = envColor * ambientFresnel * (1.0 - g_Roughness * 0.9);

    float3 ambient = diffuseIBL + specularIBL;
    float3 color = ambient + Lo;

    color = color / (color + 1.0); // Reinhard tonemap
    color = pow(color, 1.0 / 2.2); // gamma correction

    return float4(color, 1.0);
}
)";

struct InstanceConstants {
    Mat4 model;
    f32 metallic;
    f32 roughness;
};

constexpr u32 kNumLights = 4;

// Layout must byte-for-byte match the HLSL PointLight struct (each field
// pair already fills a 16-byte cbuffer slot exactly, so the array packs
// contiguously with no inter-element padding).
struct PointLight {
    Vec3 position;
    f32 pad0;
    Vec3 color;
    f32 pad1;
};

struct FrameConstants {
    Mat4 view_proj;
    Vec3 camera_pos;
    f32 pad0;
    Vec3 albedo;
    f32 pad1;
    PointLight lights[kNumLights];
};

ComPtr<ID3D12RootSignature> CreatePBRRootSignature(Device& device) {
    // One contiguous range covering t0 (normal map), t1 (environment
    // cubemap), t2 (irradiance cubemap) — a TextureCube SRV uses the same
    // SRV descriptor range type as a Texture2D one, they only differ in the
    // D3D12_SHADER_RESOURCE_VIEW_DESC used when the view itself is created.
    D3D12_DESCRIPTOR_RANGE normal_map_range{};
    normal_map_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    normal_map_range.NumDescriptors = 3;
    normal_map_range.BaseShaderRegister = 0;
    normal_map_range.RegisterSpace = 0;
    normal_map_range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {/*ShaderRegister=*/0, /*RegisterSpace=*/0,
                            /*Num32BitValues=*/sizeof(InstanceConstants) / 4};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor = {/*ShaderRegister=*/1, /*RegisterSpace=*/0};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable = {1, &normal_map_range};
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = _countof(params);
    desc.pParameters = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature, error;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) {
        const char* message = error ? static_cast<const char*>(error->GetBufferPointer()) : "(no error blob)";
        AETHER_LOG_FATAL("PBRDemo", "Root signature serialization failed: %s", message);
        throw std::runtime_error("root signature serialization failed");
    }
    ComPtr<ID3D12RootSignature> root_signature;
    AETHER_D3D_CHECK(device.Handle()->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                                           IID_PPV_ARGS(&root_signature)));
    return root_signature;
}

ComPtr<ID3D12PipelineState> CreatePBRPSO(Device& device, ID3D12RootSignature* root_signature,
                                          DXGI_FORMAT rtv_format) {
    ShaderBytecode vs = CompileHLSL(kPBRShaderSource, "VSMain", "vs_5_1", "pbr_vs");
    ShaderBytecode ps = CompileHLSL(kPBRShaderSource, "PSMain", "ps_5_1", "pbr_ps");

    D3D12_INPUT_ELEMENT_DESC input_elements[4] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(PBRVertex, pos),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(PBRVertex, normal),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(PBRVertex, uv),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(PBRVertex, tangent),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root_signature;
    desc.InputLayout = {input_elements, 4};
    desc.VS = {vs.Data(), vs.Size()};
    desc.PS = {ps.Data(), ps.Size()};

    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    desc.RasterizerState.DepthClipEnable = TRUE;

    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    desc.DepthStencilState.StencilEnable = FALSE;

    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = rtv_format;
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> pso;
    AETHER_D3D_CHECK(device.Handle()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
    return pso;
}

// Reads back a specific backbuffer index into a PNG, for automated visual
// verification of the PBR output (rendered highlights, roughness/metallic
// trends) without a human watching the live window. Takes an explicit index
// — the *last rendered* one, tracked by the caller — rather than querying
// CurrentBackBuffer(): DXGI's flip model advances the "current" index as
// soon as Present() is called, so after the loop's final Present(),
// CurrentBackBuffer() already points at the *next*, never-rendered buffer,
// not the one just drawn (caught by actually capturing a screenshot and
// seeing blank/uninitialized memory instead of the rendered scene).
void SaveBackbufferScreenshot(Device& device, SwapChain& swap_chain, u32 buffer_index, const std::string& path) {
    ID3D12Resource* back_buffer = swap_chain.BackBuffer(buffer_index);
    D3D12_RESOURCE_DESC desc = back_buffer->GetDesc();

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    u64 total_bytes = 0;
    // The 7th output param (left null here) is the UNPADDED row size, not
    // the actual stride between rows in the copied buffer
    // (footprint.Footprint.RowPitch, 256-byte aligned) — see rhi_demo's
    // SaveD3D12Screenshot for the real bug this caused there (not visible at
    // this demo's 1280x720, where 1280*4=5120 already happens to be
    // 256-aligned).
    device.Handle()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total_bytes);

    Buffer readback(device, total_bytes, BufferKind::Readback);

    CommandList cmd(device);
    cmd.Reset();

    D3D12_RESOURCE_BARRIER to_copy_src = TransitionBarrier(back_buffer, D3D12_RESOURCE_STATE_PRESENT,
                                                            D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->ResourceBarrier(1, &to_copy_src);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = back_buffer;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback.Handle();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;

    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER to_present = TransitionBarrier(back_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                           D3D12_RESOURCE_STATE_PRESENT);
    cmd->ResourceBarrier(1, &to_present);

    cmd.Close();
    ID3D12CommandList* lists[] = {cmd.Get()};
    device.WaitForFence(device.Submit(lists, 1));

    std::vector<u8> raw(total_bytes);
    readback.Read(raw.data(), total_bytes);

    // Backbuffer format is R8G8B8A8_UNORM (see SwapChain); drop the row
    // padding GetCopyableFootprints may have inserted so stb_image_write
    // gets a tightly-packed buffer.
    u32 width = static_cast<u32>(desc.Width);
    u32 height = desc.Height;
    std::vector<u8> tight(static_cast<usize>(width) * height * 4);
    for (u32 y = 0; y < height; ++y) {
        std::memcpy(&tight[static_cast<usize>(y) * width * 4],
                    &raw[static_cast<usize>(y) * footprint.Footprint.RowPitch], static_cast<usize>(width) * 4);
    }

    stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 4, tight.data(),
                   static_cast<int>(width) * 4);
    AETHER_LOG_INFO("PBRDemo", "Wrote screenshot to \"%s\" (%ux%u)", path.c_str(), width, height);
}

} // namespace

int main() {
    i32 max_frames = -1;
    if (const char* env = std::getenv("AETHER_PBR_DEMO_MAX_FRAMES")) {
        max_frames = std::atoi(env);
    }
    const char* screenshot_path = std::getenv("AETHER_PBR_DEMO_SCREENSHOT");

    try {
        WindowDesc window_desc;
        window_desc.title = "Aether PBR Demo";
        window_desc.width = 1280;
        window_desc.height = 720;
        Window window(window_desc);

        Device device(/*enable_debug_layer=*/true);
        SwapChain swap_chain(device, window.NativeHandle(), window.Width(), window.Height());

        RenderGraph graph(device);

        auto create_depth_buffer = [&](u32 width, u32 height) {
            D3D12_RESOURCE_DESC depth_desc{};
            depth_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            depth_desc.Width = width;
            depth_desc.Height = height;
            depth_desc.DepthOrArraySize = 1;
            depth_desc.MipLevels = 1;
            depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
            depth_desc.SampleDesc.Count = 1;
            depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

            D3D12_CLEAR_VALUE clear_value{};
            clear_value.Format = DXGI_FORMAT_D32_FLOAT;
            clear_value.DepthStencil.Depth = 1.0f;

            return graph.CreateTransientTexture(depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear_value,
                                                 "PBRDepth");
        };
        RenderGraph::ResourceHandle depth_handle = create_depth_buffer(window.Width(), window.Height());

        window.on_resize = [&](u32 w, u32 h) {
            swap_chain.Resize(w, h);
            depth_handle = create_depth_buffer(w, h);
        };

        ComPtr<ID3D12RootSignature> root_signature = CreatePBRRootSignature(device);
        ComPtr<ID3D12PipelineState> pso = CreatePBRPSO(device, root_signature.Get(), swap_chain.Format());

        std::vector<PBRVertex> sphere_vertices;
        std::vector<u32> sphere_indices;
        GenerateSphere(0.4f, 24, 32, sphere_vertices, sphere_indices);

        Buffer vertex_buffer(device, sphere_vertices.size() * sizeof(PBRVertex), BufferKind::Upload);
        vertex_buffer.Update(sphere_vertices.data(), sphere_vertices.size() * sizeof(PBRVertex));
        Buffer index_buffer(device, sphere_indices.size() * sizeof(u32), BufferKind::Upload);
        index_buffer.Update(sphere_indices.data(), sphere_indices.size() * sizeof(u32));

        D3D12_VERTEX_BUFFER_VIEW vbv{};
        vbv.BufferLocation = vertex_buffer.GPUAddress();
        vbv.SizeInBytes = static_cast<UINT>(vertex_buffer.Size());
        vbv.StrideInBytes = sizeof(PBRVertex);

        D3D12_INDEX_BUFFER_VIEW ibv{};
        ibv.BufferLocation = index_buffer.GPUAddress();
        ibv.SizeInBytes = static_cast<UINT>(index_buffer.Size());
        ibv.Format = DXGI_FORMAT_R32_UINT;

        constexpr u32 kGridSize = 7;
        Buffer frame_constants_buffer(device, sizeof(FrameConstants), BufferKind::Upload);

        // A single non-bindless SRV heap for this demo's three textures —
        // normal map (t0), environment cubemap (t1), irradiance cubemap
        // (t2). Allocation order matters: the root signature binds all three
        // as one contiguous descriptor-table range starting at this heap's
        // index 0, so they must land at heap indices 0/1/2 in exactly that
        // order (not sandbox's bindless heap — one texture per fixed slot).
        DescriptorHeap texture_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, /*capacity=*/3,
                                     /*shader_visible=*/true);

        std::vector<u8> normal_map_pixels = GenerateBumpNormalMap(256, /*frequency=*/6.0f, /*strength=*/2.5f);
        std::vector<std::vector<u8>> sky_faces = GenerateSkyCubeFaces(128);
        std::vector<std::vector<u8>> irradiance_faces = GenerateIrradianceCubeFaces(16);

        std::vector<std::unique_ptr<CommandList>> command_lists;
        std::vector<u64> frame_fences(swap_chain.BufferCount(), 0);
        for (u32 i = 0; i < swap_chain.BufferCount(); ++i) {
            command_lists.push_back(std::make_unique<CommandList>(device));
        }

        // One-time upload of all three textures, on its own short-lived
        // command list submitted and waited on before the main loop starts.
        CommandList setup_cmd(device);
        setup_cmd.Reset();
        Texture normal_map(device, texture_heap, setup_cmd.Get(), 256, 256, normal_map_pixels.data());
        CubemapTexture environment_map = CreateCubemapTexture(device, texture_heap, setup_cmd.Get(), 128, sky_faces);
        CubemapTexture irradiance_map =
            CreateCubemapTexture(device, texture_heap, setup_cmd.Get(), 16, irradiance_faces);
        AETHER_ASSERT(normal_map.BindlessIndex() == 0);
        AETHER_ASSERT(environment_map.srv_index == 1);
        AETHER_ASSERT(irradiance_map.srv_index == 2);
        setup_cmd.Close();
        ID3D12CommandList* setup_lists[] = {setup_cmd.Get()};
        device.WaitForFence(device.Submit(setup_lists, 1));

        AETHER_LOG_INFO("PBRDemo", "Entering main loop (%ux%u material grid)", kGridSize, kGridSize);

        i32 frame_index = 0;
        f32 t = 0.0f;
        u32 last_rendered_buffer_index = 0;
        while (window.PumpMessages()) {
            if (window.IsMinimized()) {
                continue;
            }

            u32 buffer_index = swap_chain.CurrentBackBufferIndex();
            last_rendered_buffer_index = buffer_index;
            device.WaitForFence(frame_fences[buffer_index]);

            CommandList& cmd = *command_lists[buffer_index];
            cmd.Reset();

            t += 0.005f;
            Vec3 camera_pos(std::sin(t) * 9.0f, 3.0f, std::cos(t) * 9.0f);
            Mat4 view = Mat4::LookAtRH(camera_pos, Vec3(0, 0, 0), Vec3(0, 1, 0));
            Mat4 proj = Mat4::PerspectiveRH(
                Radians(45.0f), static_cast<f32>(swap_chain.Width()) / static_cast<f32>(swap_chain.Height()), 0.1f,
                100.0f);

            // Four colored point lights at the grid's corners (inverse-square
            // falloff, so `intensity` is tuned well above 1 to still read as
            // bright at ~7-8 units away) — this is what actually exercises
            // "multiple lights" and "colored lights": each corner of the
            // grid visibly picks up its nearest light's color/tint, summed
            // with the others in the shader's per-light loop.
            f32 intensity = 130.0f;
            FrameConstants frame_constants{};
            frame_constants.view_proj = proj * view;
            frame_constants.camera_pos = camera_pos;
            frame_constants.albedo = Vec3(0.9f, 0.15f, 0.15f); // crimson dielectric/metal base color
            frame_constants.lights[0] = {Vec3(-4.5f, 4.5f, 6.0f), 0.0f, Vec3(0.1f, 0.2f, 1.0f) * intensity, 0.0f};
            frame_constants.lights[1] = {Vec3(4.5f, 4.5f, 6.0f), 0.0f, Vec3(0.15f, 1.0f, 0.2f) * intensity, 0.0f};
            frame_constants.lights[2] = {Vec3(-4.5f, -4.5f, 6.0f), 0.0f, Vec3(1.0f, 0.15f, 0.9f) * intensity, 0.0f};
            frame_constants.lights[3] = {Vec3(4.5f, -4.5f, 6.0f), 0.0f, Vec3(1.0f, 0.85f, 0.1f) * intensity, 0.0f};
            frame_constants_buffer.Update(&frame_constants, sizeof(FrameConstants));

            ID3D12Resource* back_buffer = swap_chain.CurrentBackBuffer();
            RenderGraph::ResourceHandle backbuffer_handle =
                graph.ImportResource(back_buffer, D3D12_RESOURCE_STATE_PRESENT, "BackBuffer");

            graph.AddPass(
                "PBRForward",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Write(backbuffer_handle, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    builder.Write(depth_handle, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                },
                [&](ID3D12GraphicsCommandList* cl) {
                    D3D12_CPU_DESCRIPTOR_HANDLE rtv = swap_chain.CurrentBackBufferRTV();
                    D3D12_CPU_DESCRIPTOR_HANDLE dsv = graph.GetOrCreateDSV(depth_handle);
                    cl->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
                    const f32 clear_color[4] = {0.02f, 0.02f, 0.03f, 1.0f};
                    cl->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
                    cl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

                    D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<f32>(swap_chain.Width()),
                                             static_cast<f32>(swap_chain.Height()), 0.0f, 1.0f};
                    D3D12_RECT scissor{0, 0, static_cast<LONG>(swap_chain.Width()),
                                        static_cast<LONG>(swap_chain.Height())};
                    cl->RSSetViewports(1, &viewport);
                    cl->RSSetScissorRects(1, &scissor);

                    cl->SetPipelineState(pso.Get());
                    cl->SetGraphicsRootSignature(root_signature.Get());
                    cl->SetGraphicsRootConstantBufferView(1, frame_constants_buffer.GPUAddress());
                    ID3D12DescriptorHeap* heaps[] = {texture_heap.Heap()};
                    cl->SetDescriptorHeaps(1, heaps);
                    cl->SetGraphicsRootDescriptorTable(2, texture_heap.GPUHandle(0));
                    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    cl->IASetVertexBuffers(0, 1, &vbv);
                    cl->IASetIndexBuffer(&ibv);

                    f32 spacing = 1.1f;
                    f32 offset = spacing * static_cast<f32>(kGridSize - 1) * 0.5f;
                    for (u32 row = 0; row < kGridSize; ++row) {     // roughness axis
                        for (u32 col = 0; col < kGridSize; ++col) { // metallic axis
                            InstanceConstants instance{};
                            instance.model = Mat4::Translation(
                                Vec3(col * spacing - offset, row * spacing - offset, 0.0f));
                            instance.metallic = static_cast<f32>(col) / static_cast<f32>(kGridSize - 1);
                            instance.roughness =
                                std::max(0.05f, static_cast<f32>(row) / static_cast<f32>(kGridSize - 1));

                            cl->SetGraphicsRoot32BitConstants(0, sizeof(InstanceConstants) / 4, &instance, 0);
                            cl->DrawIndexedInstanced(static_cast<UINT>(sphere_indices.size()), 1, 0, 0, 0);
                        }
                    }
                });

            graph.Execute(cmd.Get(), nullptr);
            graph.Reset();

            cmd.Close();
            ID3D12CommandList* lists[] = {cmd.Get()};
            frame_fences[buffer_index] = device.Submit(lists, 1);

            swap_chain.Present(/*vsync=*/true);

            ++frame_index;
            if (max_frames >= 0 && frame_index >= max_frames) {
                AETHER_LOG_INFO("PBRDemo", "Reached AETHER_PBR_DEMO_MAX_FRAMES=%d, exiting", max_frames);
                break;
            }
        }

        for (u64 fence : frame_fences) {
            device.WaitForFence(fence);
        }

        if (screenshot_path) {
            SaveBackbufferScreenshot(device, swap_chain, last_rendered_buffer_index, screenshot_path);
        }

        AETHER_LOG_INFO("PBRDemo", "Shutting down cleanly");
    } catch (const std::exception& e) {
        AETHER_LOG_FATAL("PBRDemo", "Unhandled exception: %s", e.what());
        return 1;
    }

    return 0;
}
