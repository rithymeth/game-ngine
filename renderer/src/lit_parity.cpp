#include "aether/renderer/lit_parity.h"

#include "aether/renderer/material_codegen.h"

#include <algorithm>
#include <cmath>

namespace aether::mat {
namespace {

constexpr f32 kPi = 3.14159265359f;

// A C++ port of the HLSL, line for line, so the two can be compared. Float math, like the GPU's.
struct V3 {
    f32 x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, f32 s) { return {a.x * s, a.y * s, a.z * s}; }
V3 operator*(V3 a, V3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
V3 operator/(V3 a, f32 s) { return {a.x / s, a.y / s, a.z / s}; }
f32 Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Normalize(V3 a) { return a / std::sqrt(Dot(a, a)); }
f32 Saturate(f32 x) { return std::min(std::max(x, 0.0f), 1.0f); }
f32 Lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }
f32 SmoothStep(f32 lo, f32 hi, f32 x) {
    const f32 t = Saturate((x - lo) / (hi - lo));
    return t * t * (3.0f - 2.0f * t);
}
f32 Sq(f32 x) { return x * x; }

// --- AdvancedPbrHlsl() ---------------------------------------------------------------------------------------------------
void AnisotropicRoughness(f32 roughness, f32 anisotropy, f32& at, f32& ab) {
    const f32 a = roughness * roughness;
    at = std::max(a * (1.0f + anisotropy), 0.002f);
    ab = std::max(a * (1.0f - anisotropy), 0.002f);
}
void RotateTangentFrame(V3 T, V3 B, f32 rotation, V3& Tr, V3& Br) {
    const f32 s = std::sin(rotation * 2.0f * kPi), c = std::cos(rotation * 2.0f * kPi);
    Tr = T * c + B * s;
    Br = B * c - T * s;
}
f32 D_GGXAniso(f32 at, f32 ab, f32 NoH, f32 ToH, f32 BoH) {
    const f32 a2 = at * ab;
    const V3 d{ab * ToH, at * BoH, a2 * NoH};
    const f32 d2 = Dot(d, d);
    const f32 b2 = a2 / d2;
    return a2 * b2 * b2 * (1.0f / kPi);
}
f32 V_SmithGGXAniso(f32 at, f32 ab, f32 ToV, f32 BoV, f32 ToL, f32 BoL, f32 NoV, f32 NoL) {
    const f32 lambdaV = NoL * std::sqrt(Sq(at * ToV) + Sq(ab * BoV) + Sq(NoV));
    const f32 lambdaL = NoV * std::sqrt(Sq(at * ToL) + Sq(ab * BoL) + Sq(NoL));
    return 0.5f / std::max(lambdaV + lambdaL, 0.00001f);
}
V3 AnisotropicSpecular(V3 f0, f32 roughness, f32 anisotropy, V3 N, V3 T, V3 B, V3 V, V3 L, f32 rotation) {
    V3 Tr, Br;
    RotateTangentFrame(T, B, rotation, Tr, Br);
    const V3 H = Normalize(V + L);
    const f32 NoL = Saturate(Dot(N, L)), NoV = std::max(Dot(N, V), 0.0001f), NoH = Saturate(Dot(N, H));
    const f32 VoH = Saturate(Dot(V, H));
    f32 at, ab;
    AnisotropicRoughness(roughness, anisotropy, at, ab);
    const f32 D = D_GGXAniso(at, ab, NoH, Dot(Tr, H), Dot(Br, H));
    const f32 Vis = V_SmithGGXAniso(at, ab, Dot(Tr, V), Dot(Br, V), Dot(Tr, L), Dot(Br, L), NoV, NoL);
    const f32 fresnel = std::pow(1.0f - VoH, 5.0f);
    const V3 F = f0 + (V3{1, 1, 1} - f0) * fresnel;
    return F * (D * Vis * NoL);
}
f32 D_Charlie(f32 roughness, f32 NoH) {
    const f32 a = std::max(roughness * roughness, 0.002f);
    const f32 invA = 1.0f / a;
    const f32 cos2h = NoH * NoH;
    const f32 sin2h = std::max(1.0f - cos2h, 0.0078125f);
    return (2.0f + invA) * std::pow(sin2h, invA * 0.5f) / (2.0f * kPi);
}
f32 V_Neubelt(f32 NoV, f32 NoL) { return 1.0f / (4.0f * std::max(NoL + NoV - NoL * NoV, 0.00001f)); }
V3 Sheen(V3 color, f32 roughness, f32 NoV, f32 NoL, f32 NoH) { return color * (D_Charlie(roughness, NoH) * V_Neubelt(NoV, NoL) * NoL); }
f32 D_GGX(f32 a, f32 NoH) {
    const f32 a2 = a * a;
    const f32 d = NoH * NoH * (a2 - 1.0f) + 1.0f;
    return a2 / (kPi * d * d);
}
f32 F_Schlick(f32 f0, f32 VoH) { return f0 + (1.0f - f0) * std::pow(1.0f - VoH, 5.0f); }
struct Coat {
    f32 spec, attenuation;
};
Coat ClearCoat(f32 weight, f32 coat_roughness, f32 NoH, f32 LoH) {
    const f32 a = std::max(coat_roughness * coat_roughness, 0.002f);
    const f32 D = D_GGX(a, NoH);
    const f32 Vis = 0.25f / std::max(LoH * LoH, 0.0001f);
    const f32 F = F_Schlick(0.04f, LoH) * weight;
    return {D * Vis * F, 1.0f - F};
}

// --- ShadingModelsHlsl() -------------------------------------------------------------------------------------------------
V3 SubsurfaceDiffuse(V3 albedo, V3 sss, f32 thickness, V3 N, V3 L, f32 wrap) {
    const f32 ndl = Dot(N, L);
    const f32 lit = Saturate(ndl);
    const f32 wrapped = Saturate((ndl + wrap) / ((1.0f + wrap) * (1.0f + wrap)));
    const V3 scattered = sss * ((wrapped - lit) * (1.0f - Saturate(thickness)));
    return (albedo * lit + scattered) / kPi;
}
V3 SubsurfaceTransmission(V3 sss, f32 thickness, V3 N, V3 V, V3 L, f32 distortion, f32 power) {
    const V3 H = Normalize(L + N * distortion);
    const f32 back = std::pow(Saturate(Dot(V, H * -1.0f)), power);
    return sss * (back * (1.0f - Saturate(thickness)));
}
V3 ShiftTangent(V3 T, V3 N, f32 shift) { return Normalize(T + N * shift); }
f32 HairLobe(V3 T, V3 V, V3 L, f32 exponent) {
    const V3 H = Normalize(L + V);
    const f32 tdh = Dot(T, H);
    const f32 sin_th = std::sqrt(std::max(1.0f - tdh * tdh, 0.0f));
    return SmoothStep(-1.0f, 0.0f, tdh) * std::pow(sin_th, exponent);
}
V3 HairDiffuse(V3 albedo, V3 T, V3 L) {
    const f32 tdl = Dot(T, L);
    return albedo * (std::sqrt(std::max(1.0f - tdl * tdl, 0.0f)) / kPi);
}
V3 HairSpecular(V3 color, V3 tint, V3 T, V3 N, V3 V, V3 L, f32 shift, f32 secondary_shift, f32 roughness) {
    const f32 r = Saturate(roughness);
    const f32 primary = HairLobe(ShiftTangent(T, N, shift), V, L, Lerp(256.0f, 16.0f, r));
    const f32 secondary = HairLobe(ShiftTangent(T, N, secondary_shift), V, L, Lerp(64.0f, 8.0f, r));
    return color * (V3{1, 1, 1} * primary + tint * secondary);
}
f32 D_Ashikhmin(f32 roughness, f32 NoH) {
    const f32 a2 = std::max(roughness * roughness, 0.002f);
    const f32 cos2h = NoH * NoH;
    const f32 sin2h = std::max(1.0f - cos2h, 0.0078125f);
    const f32 sin4h = sin2h * sin2h;
    const f32 cot2 = -cos2h / (a2 * sin2h);
    return 1.0f / (kPi * (4.0f * a2 + 1.0f)) * (4.0f * std::exp(cot2) / sin4h + sin4h);
}
V3 ClothSpecular(V3 color, f32 roughness, f32 fuzz, f32 NoV, f32 NoL, f32 NoH) {
    const f32 vis = 1.0f / (4.0f * std::max(NoL + NoV - NoL * NoV, 0.00001f));
    const f32 lobe = D_Ashikhmin(roughness, NoH) * vis;
    const f32 rim = fuzz * std::pow(1.0f - Saturate(NoV), 3.0f);
    return color * ((lobe + rim) * NoL);
}
V3 ClothDiffuse(V3 albedo, f32 NoL, f32 wrap) { return albedo * (Saturate((NoL + wrap) / (1.0f + wrap)) / kPi); }

// --- The scenes ----------------------------------------------------------------------------------------------------------
struct Frame {
    V3 N{0, 0, 1}, T{1, 0, 0}, B{0, 1, 0}, V, L, H;
    f32 NoV = 0, NoL = 0, NoH = 0, LoH = 0, u = 0, v = 0;
};
Frame MakeFrame(u32 x, u32 y, u32 width, u32 height) {
    Frame f;
    f.u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(width);
    f.v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(height);
    const f32 theta = 0.15f + f.u * 1.2f;
    f.V = Normalize({0.3f, 0.2f, 1.0f});
    f.L = Normalize({std::sin(theta), 0.5f * std::sin(theta), std::cos(theta)});
    f.H = Normalize(f.V + f.L);
    f.NoV = Saturate(Dot(f.N, f.V));
    f.NoL = Saturate(Dot(f.N, f.L));
    f.NoH = Saturate(Dot(f.N, f.H));
    f.LoH = Saturate(Dot(f.L, f.H));
    return f;
}

struct LitScene {
    const char* name;
    const char* hlsl_body; // computes float3 c from u, v, N, T, B, V, L, H, NoV, NoL, NoH, LoH
    V3 (*eval)(const Frame&);
};

const LitScene kScenes[] = {
    {"lit_anisotropy", "    float3 c = 4.0 * AetherAnisotropicSpecular(float3(0.9, 0.9, 0.9), 0.5, v * 2.0 - 1.0, N, T, B, V, L, 0.0);\n",
     [](const Frame& f) { return AnisotropicSpecular({0.9f, 0.9f, 0.9f}, 0.5f, f.v * 2.0f - 1.0f, f.N, f.T, f.B, f.V, f.L, 0.0f) * 4.0f; }},
    {"lit_sheen", "    float3 c = 25.0 * AetherSheen(float3(1.0, 0.8, 0.6), 0.1 + 0.9 * v, NoV, NoL, NoH);\n",
     [](const Frame& f) { return Sheen({1.0f, 0.8f, 0.6f}, 0.1f + 0.9f * f.v, f.NoV, f.NoL, f.NoH) * 25.0f; }},
    {"lit_clear_coat",
     "    AetherCoat coat = AetherClearCoat(1.0, 0.05 + 0.5 * v, NoH, LoH);\n    float3 c = float3(coat.spec * NoL * 60.0, coat.attenuation, 0.0);\n",
     [](const Frame& f) {
         const Coat k = ClearCoat(1.0f, 0.05f + 0.5f * f.v, f.NoH, f.LoH);
         return V3{k.spec * f.NoL * 60.0f, k.attenuation, 0.0f};
     }},
    {"lit_hair",
     "    float3 c = AetherHairSpecular(float3(1.0, 1.0, 1.0), float3(1.0, 0.8, 0.6), T, N, V, L, (v - 0.5) * 0.4, -0.1, 0.3) +\n"
     "               AetherHairDiffuse(float3(0.2, 0.1, 0.05), T, L);\n",
     [](const Frame& f) {
         return HairSpecular({1, 1, 1}, {1.0f, 0.8f, 0.6f}, f.T, f.N, f.V, f.L, (f.v - 0.5f) * 0.4f, -0.1f, 0.3f) +
                HairDiffuse({0.2f, 0.1f, 0.05f}, f.T, f.L);
     }},
    {"lit_cloth",
     "    float3 c = 20.0 * AetherClothSpecular(float3(0.5, 0.5, 0.6), 0.2 + 0.8 * v, 0.5, NoV, NoL, NoH) +\n"
     "               AetherClothDiffuse(float3(0.3, 0.3, 0.4), NoL, 0.3);\n",
     [](const Frame& f) {
         return ClothSpecular({0.5f, 0.5f, 0.6f}, 0.2f + 0.8f * f.v, 0.5f, f.NoV, f.NoL, f.NoH) * 20.0f + ClothDiffuse({0.3f, 0.3f, 0.4f}, f.NoL, 0.3f);
     }},
    {"lit_subsurface",
     "    float3 c = 3.0 * (AetherSubsurfaceDiffuse(float3(0.8, 0.6, 0.5), float3(1.0, 0.3, 0.2), v, N, L, 0.5) +\n"
     "                      AetherSubsurfaceTransmission(float3(1.0, 0.3, 0.2), v, N, V, L, 0.2, 4.0));\n",
     [](const Frame& f) {
         return (SubsurfaceDiffuse({0.8f, 0.6f, 0.5f}, {1.0f, 0.3f, 0.2f}, f.v, f.N, f.L, 0.5f) +
                 SubsurfaceTransmission({1.0f, 0.3f, 0.2f}, f.v, f.N, f.V, f.L, 0.2f, 4.0f)) * 3.0f;
     }},
};

std::string Hlsl(const LitScene& scene, u32 width, u32 height) {
    std::string h = AdvancedPbrHlsl();
    h += ShadingModelsHlsl();
    h += "float4 VSMain(uint id : SV_VertexID) : SV_Position {\n"
         "    float2 uv = float2((id << 1) & 2, id & 2);\n"
         "    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
         "}\n"
         "float4 PSMain(float4 pos : SV_Position) : SV_Target {\n"
         "    float u = pos.x / " + std::to_string(width) + ".0;\n"
         "    float v = pos.y / " + std::to_string(height) + ".0;\n"
         "    float theta = 0.15 + u * 1.2;\n"
         "    float3 N = float3(0.0, 0.0, 1.0), T = float3(1.0, 0.0, 0.0), B = float3(0.0, 1.0, 0.0);\n"
         "    float3 V = normalize(float3(0.3, 0.2, 1.0));\n"
         "    float3 L = normalize(float3(sin(theta), 0.5 * sin(theta), cos(theta)));\n"
         "    float3 H = normalize(V + L);\n"
         "    float NoV = saturate(dot(N, V)), NoL = saturate(dot(N, L)), NoH = saturate(dot(N, H)), LoH = saturate(dot(L, H));\n";
    h += scene.hlsl_body;
    h += "    c = c / (1.0 + c);\n    return float4(saturate(c), 1.0);\n}\n";
    return h;
}

u8 ToByte(f32 v) { return static_cast<u8>(std::lround(std::min(std::max(v, 0.0f), 1.0f) * 255.0f)); }

std::vector<u8> Reference(const LitScene& scene, u32 width, u32 height) {
    std::vector<u8> out(static_cast<usize>(width) * height * 4);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const V3 c = scene.eval(MakeFrame(x, y, width, height));
            u8* p = &out[(static_cast<usize>(y) * width + x) * 4];
            p[0] = ToByte(c.x / (1.0f + c.x)), p[1] = ToByte(c.y / (1.0f + c.y)), p[2] = ToByte(c.z / (1.0f + c.z)), p[3] = 255;
        }
    }
    return out;
}

} // namespace

const std::vector<gfx::parity::Scene>& LitParityScenes() {
    static const std::vector<gfx::parity::Scene> scenes = [] {
        std::vector<gfx::parity::Scene> out;
        for (const LitScene& s : kScenes) {
            gfx::parity::Scene scene;
            scene.kind = gfx::parity::SceneKind::Custom;
            scene.name = s.name;
            const LitScene* lit = &s;
            scene.hlsl = [lit](u32 w, u32 h) { return Hlsl(*lit, w, h); };
            scene.reference = [lit](u32 w, u32 h) { return Reference(*lit, w, h); };
            out.push_back(std::move(scene));
        }
        return out;
    }();
    return scenes;
}

} // namespace aether::mat
