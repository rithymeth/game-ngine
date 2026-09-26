// Roadmap item 2 (+ follow-ups: normal mapping, multiple/colored lights,
// full split-sum prefiltered specular IBL): a real PBR (physically-based)
// renderer — Cook-Torrance specular (GGX normal distribution, Smith geometry
// term, Schlick Fresnel approximation) plus a Lambertian diffuse term,
// metallic-roughness workflow. Textbook formulas (matching the widely-used
// LearnOpenGL/Sascha Willems reference derivations), not a simplified
// stand-in. Tangent-space normal mapping (procedural bump map, TBN built
// per-vertex from an analytic sphere tangent) perturbs the shading normal
// before the BRDF runs; four point lights with distinct colors and
// inverse-square falloff replace the original single directional light,
// summed per-pixel; ambient specular uses the full split-sum approximation
// (Karis 2013) — a prefiltered environment mip chain plus a BRDF LUT, both
// precomputed CPU-side at startup — rather than a roughness-faded direct
// reflection (see the "Follow-up: full split-sum ... IBL" README section).
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

#include <algorithm>
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

// A large flat quad (XZ plane, +Y up) — the shadow-mapping follow-up's
// receiver surface. The sphere grid alone has nothing for a shadow to
// visibly fall onto (neighboring spheres barely occlude each other at this
// grid spacing), so this ground plane, sitting below the grid, is what
// actually makes the shadow-mapping pass demonstrable in a screenshot.
void GenerateGroundPlane(f32 half_size, std::vector<PBRVertex>& out_vertices, std::vector<u32>& out_indices) {
    PBRVertex v[4]{};
    f32 positions[4][2] = {{-half_size, -half_size}, {half_size, -half_size}, {half_size, half_size},
                            {-half_size, half_size}};
    f32 uvs[4][2] = {{0, 0}, {4, 0}, {4, 4}, {0, 4}};
    for (u32 i = 0; i < 4; ++i) {
        v[i].pos[0] = positions[i][0];
        v[i].pos[1] = 0.0f;
        v[i].pos[2] = positions[i][1];
        v[i].normal[0] = 0.0f;
        v[i].normal[1] = 1.0f;
        v[i].normal[2] = 0.0f;
        v[i].uv[0] = uvs[i][0];
        v[i].uv[1] = uvs[i][1];
        v[i].tangent[0] = 1.0f;
        v[i].tangent[1] = 0.0f;
        v[i].tangent[2] = 0.0f;
        out_vertices.push_back(v[i]);
    }
    u32 indices[6] = {0, 1, 2, 0, 2, 3};
    out_indices.assign(indices, indices + 6);
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

// --------------------------------------------------------------------------
// Split-sum prefiltered specular IBL (Karis, "Real Shading in Unreal Engine
// 4", 2013): a prefiltered environment mip chain (each mip pre-convolved
// against the GGX lobe for that mip's roughness, so the runtime cost is one
// trilinear cubemap sample) plus a 2D BRDF LUT (indexed by NdotV and
// roughness, storing the two scalars the specular integral factors into:
// see IntegrateBRDF below). Both are precomputed here on the CPU, same as
// the diffuse-irradiance convolution above — this is a demo, not a runtime
// asset pipeline, so "precompute once at startup with plain C++ loops"
// beats standing up a compute-shader pass for what's fundamentally the same
// one-time cost either way.
// --------------------------------------------------------------------------

// Van der Corput radical inverse (base 2) — paired with i/N to form the
// Hammersley low-discrepancy sequence GGX importance sampling is built on
// (same Karis 2013 derivation as the rest of this section).
f32 RadicalInverseVdC(u32 bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<f32>(bits) * 2.3283064365386963e-10f; // / 2^32
}

// GGX importance-sampled half-vector, in the *tangent space of N* (N along
// +Z) — i.e. this is deliberately basis-free; callers transform the result
// into whatever space they need (world space for the mip prefilter, or used
// directly when N is already (0,0,1) as in IntegrateBRDF below). Same
// distribution DistributionGGX evaluates in the pixel shader, sampled here
// instead of evaluated.
Vec3 ImportanceSampleGGXTangent(f32 xi_x, f32 xi_y, f32 roughness) {
    f32 a = roughness * roughness;
    f32 phi = 2.0f * kPi * xi_x;
    f32 cos_theta = std::sqrt((1.0f - xi_y) / (1.0f + (a * a - 1.0f) * xi_y));
    f32 sin_theta = std::sqrt(std::max(0.0f, 1.0f - cos_theta * cos_theta));
    return Vec3(sin_theta * std::cos(phi), sin_theta * std::sin(phi), cos_theta);
}

// One mip level of the prefiltered environment cubemap, for a given
// roughness: for each texel's direction N, assume N == V == R (the standard
// split-sum simplifying assumption — see Karis 2013 section 3), importance-
// sample the GGX lobe around N, and accumulate SkyColor(L) weighted by
// NdotL. roughness == 0 skips the GGX sampling entirely (a mirror lobe is a
// single direction, not a distribution to sample) and just mirrors SkyColor
// directly — this is mip 0. Evaluated directly against the analytic
// SkyColor() function for the same reason GenerateIrradianceCubeFaces is:
// SkyColor *is* the environment radiance, no texture-sampling round trip
// needed.
std::vector<std::vector<u8>> GeneratePrefilteredEnvironmentMip(u32 size, f32 roughness, u32 sample_count) {
    std::vector<std::vector<u8>> faces(6);
    for (u32 face = 0; face < 6; ++face) {
        std::vector<u8>& pixels = faces[face];
        pixels.resize(static_cast<usize>(size) * size * 4);
        for (u32 y = 0; y < size; ++y) {
            for (u32 x = 0; x < size; ++x) {
                f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(size);
                f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(size);
                Vec3 N = CubeFaceDirection(face, u, v).Normalized();

                Vec3 color;
                if (roughness <= 1e-4f) {
                    color = SkyColor(N);
                } else {
                    Vec3 V = N;
                    Vec3 up = std::abs(N.y) < 0.999f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
                    Vec3 tangent_x = up.Cross(N).Normalized();
                    Vec3 tangent_y = N.Cross(tangent_x);

                    Vec3 prefiltered(0.0f, 0.0f, 0.0f);
                    f32 total_weight = 0.0f;
                    for (u32 i = 0; i < sample_count; ++i) {
                        f32 xi_x = static_cast<f32>(i) / static_cast<f32>(sample_count);
                        f32 xi_y = RadicalInverseVdC(i);
                        Vec3 h_tangent = ImportanceSampleGGXTangent(xi_x, xi_y, roughness);
                        Vec3 H = tangent_x * h_tangent.x + tangent_y * h_tangent.y + N * h_tangent.z;
                        Vec3 L = (H * (2.0f * V.Dot(H)) - V).Normalized();
                        f32 NdotL = std::max(N.Dot(L), 0.0f);
                        if (NdotL > 0.0f) {
                            prefiltered = prefiltered + SkyColor(L) * NdotL;
                            total_weight += NdotL;
                        }
                    }
                    color = total_weight > 0.0f ? prefiltered * (1.0f / total_weight) : SkyColor(N);
                }

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

// IBL variant of the Smith geometry term: k = roughness^2/2, distinct from
// the direct-lighting k = (roughness+1)^2/8 used in the pixel shader's
// GeometrySchlickGGX — same split between "direct" and "IBL" k as Karis
// 2013 / the LearnOpenGL IBL chapter (the IBL k comes from matching Smith's
// term to the GGX importance-sampling PDF, so it's not the same
// approximation the direct-lighting k is).
f32 GeometrySchlickGGXIBL(f32 NdotV, f32 roughness) {
    f32 k = (roughness * roughness) / 2.0f;
    return NdotV / (NdotV * (1.0f - k) + k);
}

f32 GeometrySmithIBL(f32 NdotV, f32 NdotL, f32 roughness) {
    return GeometrySchlickGGXIBL(NdotV, roughness) * GeometrySchlickGGXIBL(NdotL, roughness);
}

// The split-sum's second factor: integrates the BRDF (minus F0, which
// factors out as F0*scale + bias — the whole point of the split) over the
// GGX-importance-sampled hemisphere, for a given (NdotV, roughness) pair.
// This is the exact derivation the runtime g_BRDFLUT texture bakes: the
// pixel shader just looks up (scale, bias) instead of running this loop
// per-pixel per-frame.
void IntegrateBRDF(f32 NdotV, f32 roughness, u32 sample_count, f32& out_scale, f32& out_bias) {
    Vec3 V(std::sqrt(std::max(0.0f, 1.0f - NdotV * NdotV)), 0.0f, NdotV);
    f32 A = 0.0f;
    f32 B = 0.0f;

    for (u32 i = 0; i < sample_count; ++i) {
        f32 xi_x = static_cast<f32>(i) / static_cast<f32>(sample_count);
        f32 xi_y = RadicalInverseVdC(i);
        // N == (0,0,1) here, so tangent space *is* world space — the H
        // ImportanceSampleGGXTangent returns needs no basis transform.
        Vec3 H = ImportanceSampleGGXTangent(xi_x, xi_y, roughness);
        Vec3 L = (H * (2.0f * V.Dot(H)) - V).Normalized();

        f32 NdotL = std::max(L.z, 0.0f);
        f32 NdotH = std::max(H.z, 0.0f);
        f32 VdotH = std::max(V.Dot(H), 0.0f);

        if (NdotL > 0.0f) {
            f32 G = GeometrySmithIBL(NdotV, NdotL, roughness);
            f32 G_Vis = (G * VdotH) / std::max(NdotH * NdotV, 1e-5f);
            f32 Fc = std::pow(1.0f - VdotH, 5.0f);
            A += (1.0f - Fc) * G_Vis;
            B += Fc * G_Vis;
        }
    }
    out_scale = A / static_cast<f32>(sample_count);
    out_bias = B / static_cast<f32>(sample_count);
}

// x = NdotV, y = roughness (matching the shader's g_BRDFLUT.Sample(uv =
// float2(NdotV, roughness)) lookup) — scale packed into R, bias into G;
// B/A unused (0/255) since Texture only supports RGBA8.
std::vector<u8> GenerateBRDFLUT(u32 size, u32 sample_count) {
    std::vector<u8> pixels(static_cast<usize>(size) * size * 4);
    for (u32 y = 0; y < size; ++y) {
        f32 roughness = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(size);
        for (u32 x = 0; x < size; ++x) {
            f32 NdotV = std::max((static_cast<f32>(x) + 0.5f) / static_cast<f32>(size), 1e-3f);
            f32 scale, bias;
            IntegrateBRDF(NdotV, roughness, sample_count, scale, bias);
            u8* p = &pixels[(static_cast<usize>(y) * size + x) * 4];
            p[0] = static_cast<u8>(std::clamp(scale, 0.0f, 1.0f) * 255.0f);
            p[1] = static_cast<u8>(std::clamp(bias, 0.0f, 1.0f) * 255.0f);
            p[2] = 0;
            p[3] = 255;
        }
    }
    return pixels;
}

struct CubemapTexture {
    ComPtr<ID3D12Resource> resource;
    // Kept alive for the CubemapTexture's own lifetime rather than freed
    // once the copy completes — same demo-scale trade-off gfx::Texture's
    // upload_staging_ member documents (engine/include/aether/gfx/texture.h).
    ComPtr<ID3D12Resource> upload_staging;
    u32 srv_index = DescriptorHeap::kInvalidIndex;
};

// Uploads a 6-face RGBA8 cubemap, optionally with multiple mip levels —
// DescriptorHeap-allocated SRV, one DEFAULT-heap resource with 6 array
// slices * mip_faces.size() mip levels, one UPLOAD-heap staging buffer sized
// for every subresource at once. Mirrors gfx::Texture's single-face upload
// pattern (engine/src/gfx/texture.cpp), generalized to 6*mip_count
// subresources. `mip_faces[mip][face]` must be sized for
// `base_size >> mip` (tightly packed RGBA8) — the irradiance map (one mip)
// and the prefiltered environment map (kEnvMapMipCount mips) both go through
// this one function; a 1-element `mip_faces` is exactly the old
// single-mip-cubemap case.
CubemapTexture CreateCubemapTexture(Device& device, DescriptorHeap& heap, ID3D12GraphicsCommandList* upload_cmd,
                                     u32 base_size, const std::vector<std::vector<std::vector<u8>>>& mip_faces) {
    CubemapTexture result;
    u32 mip_count = static_cast<u32>(mip_faces.size());
    u32 num_subresources = 6 * mip_count;

    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC tex_desc{};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width = base_size;
    tex_desc.Height = base_size;
    tex_desc.DepthOrArraySize = 6;
    tex_desc.MipLevels = static_cast<UINT16>(mip_count);
    tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    AETHER_D3D_CHECK(device.Handle()->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
                                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                               IID_PPV_ARGS(&result.resource)));

    // Subresource index = mip + face * mip_count (D3D12's standard
    // MipSlice + ArraySlice * MipLevels indexing for a single-plane
    // resource) — GetCopyableFootprints fills footprints[subresource] using
    // that exact same indexing, so no separate mapping table is needed.
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(num_subresources);
    std::vector<UINT> num_rows(num_subresources);
    std::vector<UINT64> row_sizes(num_subresources);
    UINT64 total_bytes = 0;
    device.Handle()->GetCopyableFootprints(&tex_desc, 0, num_subresources, 0, footprints.data(), num_rows.data(),
                                            row_sizes.data(), &total_bytes);

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
        for (u32 mip = 0; mip < mip_count; ++mip) {
            u32 subresource = mip + face * mip_count;
            u32 mip_size = base_size >> mip;
            const std::vector<u8>& pixels = mip_faces[mip][face];
            for (u32 row = 0; row < mip_size; ++row) {
                std::memcpy(mapped + footprints[subresource].Offset +
                                static_cast<u64>(row) * footprints[subresource].Footprint.RowPitch,
                            pixels.data() + static_cast<u64>(row) * mip_size * 4, static_cast<usize>(mip_size) * 4);
            }
        }
    }
    staging->Unmap(0, nullptr);

    for (u32 face = 0; face < 6; ++face) {
        for (u32 mip = 0; mip < mip_count; ++mip) {
            u32 subresource = mip + face * mip_count;
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = result.resource.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = subresource;

            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = staging;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = footprints[subresource];

            upload_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
    }

    D3D12_RESOURCE_BARRIER barrier = TransitionBarrier(result.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    upload_cmd->ResourceBarrier(1, &barrier);

    result.srv_index = heap.Allocate();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = tex_desc.Format;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.TextureCube.MipLevels = mip_count;
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
    // Shadow mapping follow-up: g_Lights[0] is the sole shadow-casting
    // light (see the README's shadow-mapping section for why one light,
    // not all four — point-light shadows need a cubemap per light, real
    // future work). g_LightViewProj projects world space into that light's
    // shadow-map clip space; g_ShadowTexelSize is 1/shadow-map-resolution,
    // used to offset the 3x3 PCF taps below by exactly one texel.
    float4x4 g_LightViewProj;
    float g_ShadowTexelSize;
    float3 _Pad2;
};

Texture2D g_NormalMap : register(t0);
TextureCube g_PrefilteredEnvMap : register(t1);
TextureCube g_IrradianceMap : register(t2);
Texture2D g_BRDFLUT : register(t3);
Texture2D g_ShadowMap : register(t4);
SamplerState g_Sampler : register(s0);
SamplerComparisonState g_ShadowSampler : register(s1);

// Must match kEnvMapMipCount on the C++ side (the number of mips baked into
// g_PrefilteredEnvMap) so roughness->mip mapping lines up with what was
// actually prefiltered at each level.
static const float kEnvMapMaxMipIndex = 4.0;

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

// 3x3 PCF (percentage-closer filtering): 9 hardware comparison samples
// (SampleCmpLevelZero — the comparison itself, current-depth-vs-stored-depth
// per tap, happens in the texture unit, not as a manual branch) averaged
// into a soft 0..1 shadow factor, rather than one hard in/out-of-shadow
// sample — visibly softens shadow edges, at 9x the texture traffic of a
// single tap. A small constant depth bias (subtracted from the fragment's
// own depth before comparing) counteracts shadow acne (self-shadowing false
// positives from the shadow map's own finite resolution) — the standard
// fix, tuned by eye for this scene's light distance/shadow-map resolution
// rather than derived analytically (slope-scaled bias would adapt better
// across very different geometry, but is more machinery than this demo's
// single-light, mostly-flat-relative-to-the-light-direction spheres need).
float ComputeShadow(float3 worldPos) {
    float4 lightClip = mul(g_LightViewProj, float4(worldPos, 1.0));
    float3 ndc = lightClip.xyz / lightClip.w;
    float2 shadowUV = ndc.xy * 0.5 + 0.5;
    shadowUV.y = 1.0 - shadowUV.y; // NDC +Y is up; texture space +V is down
    float currentDepth = ndc.z;

    if (shadowUV.x < 0.0 || shadowUV.x > 1.0 || shadowUV.y < 0.0 || shadowUV.y > 1.0 || currentDepth > 1.0) {
        return 1.0; // outside the shadow-casting light's frustum: fully lit
    }

    const float kDepthBias = 0.0015;
    float shadow = 0.0;
    [unroll]
    for (int dx = -1; dx <= 1; ++dx) {
        [unroll]
        for (int dy = -1; dy <= 1; ++dy) {
            float2 offset = float2(dx, dy) * g_ShadowTexelSize;
            shadow += g_ShadowMap.SampleCmpLevelZero(g_ShadowSampler, shadowUV + offset, currentDepth - kDepthBias);
        }
    }
    return shadow / 9.0;
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
        // Only g_Lights[0] casts a shadow — see FrameConstants' comment.
        float shadowFactor = (i == 0) ? ComputeShadow(input.worldPos) : 1.0;
        Lo += (kD * g_Albedo / kPi + specular) * radiance * NdotL * shadowFactor;
    }

    // Image-based lighting: diffuse from the precomputed irradiance
    // convolution (GenerateIrradianceCubeFaces on the CPU, see the comment
    // there); specular via the full split-sum approximation (Karis, "Real
    // Shading in Unreal Engine 4", 2013) — g_PrefilteredEnvMap's mip chain
    // was pre-convolved against the GGX lobe for each mip's roughness (see
    // GeneratePrefilteredEnvironmentMip), so a single roughness-selected
    // trilinear sample stands in for what would otherwise be a per-pixel
    // importance-sampling loop; g_BRDFLUT bakes the second split-sum factor
    // (IntegrateBRDF, also precomputed CPU-side) so the runtime cost is just
    // two texture samples, not an integral.
    float NdotV = max(dot(N, V), 0.0);
    float3 ambientFresnel = FresnelSchlick(NdotV, F0);
    float3 ambientKd = (1.0 - ambientFresnel) * (1.0 - g_Metallic);
    float3 irradiance = g_IrradianceMap.Sample(g_Sampler, N).rgb;
    float3 diffuseIBL = ambientKd * irradiance * g_Albedo;

    float3 R = reflect(-V, N);
    float3 prefilteredColor = g_PrefilteredEnvMap.SampleLevel(g_Sampler, R, g_Roughness * kEnvMapMaxMipIndex).rgb;
    float2 brdf = g_BRDFLUT.Sample(g_Sampler, float2(NdotV, g_Roughness)).rg;
    float3 specularIBL = prefilteredColor * (F0 * brdf.x + brdf.y);

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
    Mat4 light_view_proj;
    f32 shadow_texel_size;
    f32 pad2[3];
};

ComPtr<ID3D12RootSignature> CreatePBRRootSignature(Device& device) {
    // One contiguous range covering t0 (normal map), t1 (prefiltered
    // environment cubemap), t2 (irradiance cubemap), t3 (BRDF LUT), t4
    // (shadow map) — a TextureCube SRV uses the same SRV descriptor range
    // type as a Texture2D one, they only differ in the
    // D3D12_SHADER_RESOURCE_VIEW_DESC used when the view itself is created.
    D3D12_DESCRIPTOR_RANGE normal_map_range{};
    normal_map_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    normal_map_range.NumDescriptors = 5;
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

    // A comparison sampler (s1): the GPU compares the fragment's own depth
    // against the shadow map's stored depth per tap as part of the texture
    // fetch itself (SampleCmpLevelZero in the shader), rather than the
    // shader sampling a raw depth value and comparing manually — this is
    // what makes hardware bilinear PCF possible at all. CLAMP addressing +
    // an opaque-white border color means a shadow-map lookup that lands
    // outside [0,1] UV (already handled explicitly in ComputeShadow, but
    // belt-and-suspenders for the PCF taps' small offset) reads back
    // "farthest possible depth", comparing as unoccluded rather than
    // sampling undefined memory.
    D3D12_STATIC_SAMPLER_DESC shadow_sampler{};
    shadow_sampler.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    shadow_sampler.AddressU = shadow_sampler.AddressV = shadow_sampler.AddressW =
        D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadow_sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    shadow_sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    shadow_sampler.ShaderRegister = 1;
    shadow_sampler.RegisterSpace = 0;
    shadow_sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samplers[2] = {sampler, shadow_sampler};

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = _countof(params);
    desc.pParameters = params;
    desc.NumStaticSamplers = 2;
    desc.pStaticSamplers = samplers;
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

// --------------------------------------------------------------------------
// Shadow mapping: a depth-only pass from g_Lights[0]'s point of view into a
// dedicated shadow map, sampled back (with PCF, see ComputeShadow above) in
// the main PBR pass. Deliberately scoped to ONE shadow-casting light, not
// all four: a point light's shadow genuinely needs 6 shadow maps (one per
// cube face) to cover the full sphere of directions it illuminates from,
// which is real future work (see the README) — this follow-up proves the
// depth-pass/sample-back mechanics with a single 2D shadow map, treating
// light 0 as if it only needed to illuminate the hemisphere facing the
// grid (true for where it's actually positioned in this scene).
// --------------------------------------------------------------------------

constexpr const char* kShadowShaderSource = R"(
cbuffer ShadowConstants : register(b0) {
    float4x4 g_MVP;
};

struct VSInput {
    float3 position : POSITION;
};

float4 VSMain(VSInput input) : SV_POSITION {
    return mul(g_MVP, float4(input.position, 1.0));
}
)";

struct ShadowConstants {
    Mat4 mvp;
};

ComPtr<ID3D12RootSignature> CreateShadowRootSignature(Device& device) {
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants = {/*ShaderRegister=*/0, /*RegisterSpace=*/0, /*Num32BitValues=*/sizeof(ShadowConstants) / 4};
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 1;
    desc.pParameters = &param;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature, error;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) {
        const char* message = error ? static_cast<const char*>(error->GetBufferPointer()) : "(no error blob)";
        AETHER_LOG_FATAL("PBRDemo", "Shadow root signature serialization failed: %s", message);
        throw std::runtime_error("shadow root signature serialization failed");
    }
    ComPtr<ID3D12RootSignature> root_signature;
    AETHER_D3D_CHECK(device.Handle()->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                                           IID_PPV_ARGS(&root_signature)));
    return root_signature;
}

// Depth-only: no pixel shader, no render targets — only the shadow map's
// own depth buffer is written. Reuses the main pass's PBRVertex vertex
// buffer unchanged (same VBV, same stride) but declares only the POSITION
// element in its input layout, since a depth pass never reads
// normal/uv/tangent.
ComPtr<ID3D12PipelineState> CreateShadowPSO(Device& device, ID3D12RootSignature* root_signature) {
    ShaderBytecode vs = CompileHLSL(kShadowShaderSource, "VSMain", "vs_5_1", "shadow_vs");

    D3D12_INPUT_ELEMENT_DESC input_elements[1] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(PBRVertex, pos),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root_signature;
    desc.InputLayout = {input_elements, 1};
    desc.VS = {vs.Data(), vs.Size()};
    desc.PS = {nullptr, 0};

    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    desc.RasterizerState.DepthClipEnable = TRUE;

    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    desc.DepthStencilState.StencilEnable = FALSE;

    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 0;
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

// Dumps the shadow map's raw depth values (R32_FLOAT, 0=near/light-facing
// surface, 1=cleared/no geometry) to a grayscale PNG — a direct diagnostic
// for the shadow-mapping follow-up, independent of the much harder to
// visually judge "is the final lit image actually darker under a sphere"
// question a full composite screenshot poses. `shadow_map` must currently
// be in D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE (its steady-state between
// frames — see the main loop's shadow_map_used_before bookkeeping).
void SaveShadowMapScreenshot(Device& device, ID3D12Resource* shadow_map, u32 size, const std::string& path) {
    D3D12_RESOURCE_DESC desc = shadow_map->GetDesc();
    D3D12_RESOURCE_DESC readback_view_desc = desc;
    readback_view_desc.Format = DXGI_FORMAT_R32_FLOAT;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    u64 total_bytes = 0;
    device.Handle()->GetCopyableFootprints(&readback_view_desc, 0, 1, 0, &footprint, nullptr, nullptr, &total_bytes);

    Buffer readback(device, total_bytes, BufferKind::Readback);

    CommandList cmd(device);
    cmd.Reset();

    D3D12_RESOURCE_BARRIER to_copy_src =
        TransitionBarrier(shadow_map, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->ResourceBarrier(1, &to_copy_src);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = shadow_map;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback.Handle();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;

    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER to_read =
        TransitionBarrier(shadow_map, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &to_read);

    cmd.Close();
    ID3D12CommandList* lists[] = {cmd.Get()};
    device.WaitForFence(device.Submit(lists, 1));

    std::vector<u8> raw(total_bytes);
    readback.Read(raw.data(), total_bytes);

    std::vector<u8> gray(static_cast<usize>(size) * size);
    for (u32 y = 0; y < size; ++y) {
        const f32* row = reinterpret_cast<const f32*>(&raw[static_cast<usize>(y) * footprint.Footprint.RowPitch]);
        for (u32 x = 0; x < size; ++x) {
            gray[static_cast<usize>(y) * size + x] = static_cast<u8>(std::clamp(row[x], 0.0f, 1.0f) * 255.0f);
        }
    }

    stbi_write_png(path.c_str(), static_cast<int>(size), static_cast<int>(size), 1, gray.data(),
                   static_cast<int>(size));
    AETHER_LOG_INFO("PBRDemo", "Wrote shadow map screenshot to \"%s\" (%ux%u)", path.c_str(), size, size);
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

        std::vector<PBRVertex> ground_vertices;
        std::vector<u32> ground_indices;
        GenerateGroundPlane(8.0f, ground_vertices, ground_indices);

        Buffer ground_vertex_buffer(device, ground_vertices.size() * sizeof(PBRVertex), BufferKind::Upload);
        ground_vertex_buffer.Update(ground_vertices.data(), ground_vertices.size() * sizeof(PBRVertex));
        Buffer ground_index_buffer(device, ground_indices.size() * sizeof(u32), BufferKind::Upload);
        ground_index_buffer.Update(ground_indices.data(), ground_indices.size() * sizeof(u32));

        D3D12_VERTEX_BUFFER_VIEW ground_vbv{};
        ground_vbv.BufferLocation = ground_vertex_buffer.GPUAddress();
        ground_vbv.SizeInBytes = static_cast<UINT>(ground_vertex_buffer.Size());
        ground_vbv.StrideInBytes = sizeof(PBRVertex);

        D3D12_INDEX_BUFFER_VIEW ground_ibv{};
        ground_ibv.BufferLocation = ground_index_buffer.GPUAddress();
        ground_ibv.SizeInBytes = static_cast<UINT>(ground_index_buffer.Size());
        ground_ibv.Format = DXGI_FORMAT_R32_UINT;

        constexpr u32 kGridSize = 7;
        Buffer frame_constants_buffer(device, sizeof(FrameConstants), BufferKind::Upload);

        // A single non-bindless SRV heap for this demo's five textures —
        // normal map (t0), prefiltered environment cubemap (t1), irradiance
        // cubemap (t2), BRDF LUT (t3), shadow map (t4). Allocation order
        // matters: the root signature binds all five as one contiguous
        // descriptor-table range starting at this heap's index 0, so they
        // must land at heap indices 0/1/2/3/4 in exactly that order (not
        // sandbox's bindless heap — one texture per fixed slot).
        DescriptorHeap texture_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, /*capacity=*/5,
                                     /*shader_visible=*/true);

        std::vector<u8> normal_map_pixels = GenerateBumpNormalMap(256, /*frequency=*/6.0f, /*strength=*/2.5f);
        std::vector<std::vector<u8>> irradiance_faces = GenerateIrradianceCubeFaces(16);

        // Split-sum specular IBL setup: kEnvMapMipCount mips, roughness 0
        // (mirror, mip 0) through 1 (fully rough, mip kEnvMapMipCount-1),
        // each mip's resolution halving as roughness increases (a rougher
        // reflection is a lower-frequency function of direction, so it needs
        // less resolution — same reasoning as the irradiance map's low
        // resolution). Sample counts scale up per mip as resolution drops,
        // since fewer, larger texels can afford more samples each within the
        // same startup-time budget. Must match kEnvMapMaxMipIndex in the
        // shader above.
        constexpr u32 kEnvMapMipCount = 5;
        constexpr u32 kEnvMapBaseSize = 128;
        constexpr u32 kEnvMapSampleCounts[kEnvMapMipCount] = {1, 32, 64, 128, 256};
        std::vector<std::vector<std::vector<u8>>> prefiltered_env_mips;
        for (u32 mip = 0; mip < kEnvMapMipCount; ++mip) {
            u32 mip_size = kEnvMapBaseSize >> mip;
            f32 roughness = static_cast<f32>(mip) / static_cast<f32>(kEnvMapMipCount - 1);
            prefiltered_env_mips.push_back(
                GeneratePrefilteredEnvironmentMip(mip_size, roughness, kEnvMapSampleCounts[mip]));
        }

        constexpr u32 kBRDFLUTSize = 128;
        std::vector<u8> brdf_lut_pixels = GenerateBRDFLUT(kBRDFLUTSize, /*sample_count=*/512);

        // Shadow mapping setup: a dedicated depth buffer, sampled as a
        // regular Texture2D (t4 in texture_heap) by the main pass — hence
        // the TYPELESS resource format (DXGI doesn't allow a DEPTH_STENCIL-
        // flagged resource to also declare a depth-specific format like
        // D32_FLOAT if it's going to be read through an SRV with a
        // different, non-depth view; TYPELESS + separate DSV/SRV view
        // formats is the standard way to get both). Not managed through
        // RenderGraph like the main color/depth targets: it's read (SRV) in
        // the very same frame it's written (DSV), by a different pipeline,
        // which is exactly the multi-pass, cross-usage case this file's
        // RenderGraph wasn't set up to track — manual barriers instead, same
        // as the cubemap/BRDF-LUT setup above already does for other
        // resources this file manages by hand.
        constexpr u32 kShadowMapSize = 1024;
        ComPtr<ID3D12RootSignature> shadow_root_signature = CreateShadowRootSignature(device);
        ComPtr<ID3D12PipelineState> shadow_pso = CreateShadowPSO(device, shadow_root_signature.Get());

        ComPtr<ID3D12Resource> shadow_map_resource;
        {
            D3D12_HEAP_PROPERTIES default_heap{};
            default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_DESC shadow_desc{};
            shadow_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            shadow_desc.Width = kShadowMapSize;
            shadow_desc.Height = kShadowMapSize;
            shadow_desc.DepthOrArraySize = 1;
            shadow_desc.MipLevels = 1;
            shadow_desc.Format = DXGI_FORMAT_R32_TYPELESS;
            shadow_desc.SampleDesc.Count = 1;
            shadow_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

            D3D12_CLEAR_VALUE clear_value{};
            clear_value.Format = DXGI_FORMAT_D32_FLOAT;
            clear_value.DepthStencil.Depth = 1.0f;

            AETHER_D3D_CHECK(device.Handle()->CreateCommittedResource(
                &default_heap, D3D12_HEAP_FLAG_NONE, &shadow_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear_value,
                IID_PPV_ARGS(&shadow_map_resource)));
        }

        DescriptorHeap shadow_dsv_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, /*capacity=*/1,
                                        /*shader_visible=*/false);
        u32 shadow_dsv_index = shadow_dsv_heap.Allocate();
        {
            D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
            dsv_desc.Format = DXGI_FORMAT_D32_FLOAT;
            dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            device.Handle()->CreateDepthStencilView(shadow_map_resource.Get(), &dsv_desc,
                                                     shadow_dsv_heap.CPUHandle(shadow_dsv_index));
        }

        std::vector<std::unique_ptr<CommandList>> command_lists;
        std::vector<u64> frame_fences(swap_chain.BufferCount(), 0);
        for (u32 i = 0; i < swap_chain.BufferCount(); ++i) {
            command_lists.push_back(std::make_unique<CommandList>(device));
        }

        // One-time upload of all four textures, on its own short-lived
        // command list submitted and waited on before the main loop starts.
        CommandList setup_cmd(device);
        setup_cmd.Reset();
        Texture normal_map(device, texture_heap, setup_cmd.Get(), 256, 256, normal_map_pixels.data());
        CubemapTexture environment_map =
            CreateCubemapTexture(device, texture_heap, setup_cmd.Get(), kEnvMapBaseSize, prefiltered_env_mips);
        CubemapTexture irradiance_map =
            CreateCubemapTexture(device, texture_heap, setup_cmd.Get(), 16, {irradiance_faces});
        Texture brdf_lut(device, texture_heap, setup_cmd.Get(), kBRDFLUTSize, kBRDFLUTSize, brdf_lut_pixels.data());
        AETHER_ASSERT(normal_map.BindlessIndex() == 0);
        AETHER_ASSERT(environment_map.srv_index == 1);
        AETHER_ASSERT(irradiance_map.srv_index == 2);
        AETHER_ASSERT(brdf_lut.BindlessIndex() == 3);

        // The shadow map's SRV must be allocated after the four textures
        // above (whose constructors each call texture_heap.Allocate()
        // internally) so it lands at index 4, matching the root signature's
        // t0-t4 contiguous range.
        u32 shadow_srv_index = texture_heap.Allocate();
        AETHER_ASSERT(shadow_srv_index == 4);
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
            srv_desc.Format = DXGI_FORMAT_R32_FLOAT;
            srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv_desc.Texture2D.MipLevels = 1;
            device.Handle()->CreateShaderResourceView(shadow_map_resource.Get(), &srv_desc,
                                                       texture_heap.CPUHandle(shadow_srv_index));
        }

        setup_cmd.Close();
        ID3D12CommandList* setup_lists[] = {setup_cmd.Get()};
        device.WaitForFence(device.Submit(setup_lists, 1));

        AETHER_LOG_INFO("PBRDemo", "Entering main loop (%ux%u material grid)", kGridSize, kGridSize);

        // Shared by both the shadow pass and the main pass — every sphere's
        // model matrix must be identical in both, or the shadow a sphere
        // casts wouldn't line up with the sphere itself.
        f32 spacing = 1.1f;
        f32 offset = spacing * static_cast<f32>(kGridSize - 1) * 0.5f;
        auto instance_model = [&](u32 row, u32 col) {
            return Mat4::Translation(Vec3(col * spacing - offset, row * spacing - offset, 0.0f));
        };
        // Below and behind the grid, catching the shadows the sphere grid
        // casts from g_Lights[0] (above-ish, at y=4.5).
        constexpr f32 kGroundY = -4.3f;
        Mat4 ground_model = Mat4::Translation(Vec3(0.0f, kGroundY, 0.0f));

        i32 frame_index = 0;
        f32 t = 0.0f;
        u32 last_rendered_buffer_index = 0;
        bool shadow_map_used_before = false;
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
            Vec3 light0_pos(-4.5f, 4.5f, 6.0f);
            FrameConstants frame_constants{};
            frame_constants.view_proj = proj * view;
            frame_constants.camera_pos = camera_pos;
            frame_constants.albedo = Vec3(0.9f, 0.15f, 0.15f); // crimson dielectric/metal base color
            frame_constants.lights[0] = {light0_pos, 0.0f, Vec3(0.1f, 0.2f, 1.0f) * intensity, 0.0f};
            frame_constants.lights[1] = {Vec3(4.5f, 4.5f, 6.0f), 0.0f, Vec3(0.15f, 1.0f, 0.2f) * intensity, 0.0f};
            frame_constants.lights[2] = {Vec3(-4.5f, -4.5f, 6.0f), 0.0f, Vec3(1.0f, 0.15f, 0.9f) * intensity, 0.0f};
            frame_constants.lights[3] = {Vec3(4.5f, -4.5f, 6.0f), 0.0f, Vec3(1.0f, 0.85f, 0.1f) * intensity, 0.0f};

            // g_Lights[0] is the sole shadow-casting light (see
            // FrameConstants' HLSL-side comment) — a perspective projection
            // from its position toward the grid's center, wide enough
            // (60 degrees) to cover the whole 7x7 grid from ~8.75 units away.
            Mat4 light_view = Mat4::LookAtRH(light0_pos, Vec3(0, 0, 0), Vec3(0, 1, 0));
            Mat4 light_proj = Mat4::PerspectiveRH(Radians(60.0f), 1.0f, 1.0f, 20.0f);
            frame_constants.light_view_proj = light_proj * light_view;
            frame_constants.shadow_texel_size = 1.0f / static_cast<f32>(kShadowMapSize);
            frame_constants_buffer.Update(&frame_constants, sizeof(FrameConstants));

            // Shadow pass: depth-only render of every sphere from
            // g_Lights[0]'s point of view, recorded directly on `cmd`
            // (raw D3D12 calls, not through `graph`) before the main pass
            // below — see shadow_map_resource's setup comment for why this
            // multi-pass, cross-usage (DSV this pass, SRV the next) resource
            // isn't a RenderGraph-managed one.
            {
                D3D12_RESOURCE_STATES shadow_before =
                    shadow_map_used_before ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                            : D3D12_RESOURCE_STATE_DEPTH_WRITE;
                if (shadow_map_used_before) {
                    D3D12_RESOURCE_BARRIER to_write =
                        TransitionBarrier(shadow_map_resource.Get(), shadow_before, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                    cmd->ResourceBarrier(1, &to_write);
                }

                D3D12_CPU_DESCRIPTOR_HANDLE shadow_dsv = shadow_dsv_heap.CPUHandle(shadow_dsv_index);
                cmd->OMSetRenderTargets(0, nullptr, FALSE, &shadow_dsv);
                cmd->ClearDepthStencilView(shadow_dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

                D3D12_VIEWPORT shadow_viewport{0.0f, 0.0f, static_cast<f32>(kShadowMapSize),
                                                static_cast<f32>(kShadowMapSize), 0.0f, 1.0f};
                D3D12_RECT shadow_scissor{0, 0, static_cast<LONG>(kShadowMapSize), static_cast<LONG>(kShadowMapSize)};
                cmd->RSSetViewports(1, &shadow_viewport);
                cmd->RSSetScissorRects(1, &shadow_scissor);

                cmd->SetPipelineState(shadow_pso.Get());
                cmd->SetGraphicsRootSignature(shadow_root_signature.Get());
                cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                cmd->IASetVertexBuffers(0, 1, &vbv);
                cmd->IASetIndexBuffer(&ibv);

                for (u32 row = 0; row < kGridSize; ++row) {
                    for (u32 col = 0; col < kGridSize; ++col) {
                        ShadowConstants shadow_constants{frame_constants.light_view_proj * instance_model(row, col)};
                        cmd->SetGraphicsRoot32BitConstants(0, sizeof(ShadowConstants) / 4, &shadow_constants, 0);
                        cmd->DrawIndexedInstanced(static_cast<UINT>(sphere_indices.size()), 1, 0, 0, 0);
                    }
                }

                D3D12_RESOURCE_BARRIER to_read = TransitionBarrier(
                    shadow_map_resource.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                cmd->ResourceBarrier(1, &to_read);
                shadow_map_used_before = true;
            }

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

                    for (u32 row = 0; row < kGridSize; ++row) {     // roughness axis
                        for (u32 col = 0; col < kGridSize; ++col) { // metallic axis
                            InstanceConstants instance{};
                            instance.model = instance_model(row, col);
                            instance.metallic = static_cast<f32>(col) / static_cast<f32>(kGridSize - 1);
                            instance.roughness =
                                std::max(0.05f, static_cast<f32>(row) / static_cast<f32>(kGridSize - 1));

                            cl->SetGraphicsRoot32BitConstants(0, sizeof(InstanceConstants) / 4, &instance, 0);
                            cl->DrawIndexedInstanced(static_cast<UINT>(sphere_indices.size()), 1, 0, 0, 0);
                        }
                    }

                    // The shadow-receiving ground plane: fully rough,
                    // non-metallic, so its own IBL specular stays minimal
                    // and the shadow's contrast against direct lighting
                    // reads clearly.
                    InstanceConstants ground_instance{};
                    ground_instance.model = ground_model;
                    ground_instance.metallic = 0.0f;
                    ground_instance.roughness = 0.95f;
                    cl->SetGraphicsRoot32BitConstants(0, sizeof(InstanceConstants) / 4, &ground_instance, 0);
                    cl->IASetVertexBuffers(0, 1, &ground_vbv);
                    cl->IASetIndexBuffer(&ground_ibv);
                    cl->DrawIndexedInstanced(static_cast<UINT>(ground_indices.size()), 1, 0, 0, 0);
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
        if (const char* shadow_map_screenshot_path = std::getenv("AETHER_PBR_DEMO_SHADOW_MAP_SCREENSHOT")) {
            SaveShadowMapScreenshot(device, shadow_map_resource.Get(), kShadowMapSize, shadow_map_screenshot_path);
        }

        AETHER_LOG_INFO("PBRDemo", "Shutting down cleanly");
    } catch (const std::exception& e) {
        AETHER_LOG_FATAL("PBRDemo", "Unhandled exception: %s", e.what());
        return 1;
    }

    return 0;
}
