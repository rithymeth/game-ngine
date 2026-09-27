#include "aether/vfx/gpu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace aether::vfx {

namespace {

// Builds the shader's module code and the Modules buffer together, so the
// two can't disagree about where a value lives.
struct Builder {
    std::string init, forces, post, looks;
    std::vector<f32> values;
    std::vector<std::string> names;
    bool uses_depth = false, uses_noise = false;

    u32 Put(const std::string& name, f32 x, f32 y = 0.0f, f32 z = 0.0f, f32 w = 0.0f) {
        const u32 slot = static_cast<u32>(values.size() / 4);
        values.insert(values.end(), {x, y, z, w});
        names.push_back(name);
        return slot;
    }
    u32 PutCurve(const std::string& name, const FloatCurve& c) {
        const std::vector<f32> t = BakeCurve(c, kCurveSamples);
        const u32 first = static_cast<u32>(values.size() / 4);
        for (u32 i = 0; i < kCurveSamples; i += 4) {
            (void)Put(name + "[" + std::to_string(i / 4) + "]", t[i], t[i + 1], t[i + 2], t[i + 3]);
        }
        return first;
    }
    u32 PutGradient(const std::string& name, const ColorGradient& g) {
        const std::vector<LinearColor> t = BakeGradient(g, kGradientSamples);
        const u32 first = static_cast<u32>(values.size() / 4);
        for (u32 i = 0; i < kGradientSamples; ++i) (void)Put(name + "[" + std::to_string(i) + "]", t[i].r, t[i].g, t[i].b, t[i].a);
        return first;
    }
};

std::string M(u32 slot) { return "M[" + std::to_string(slot) + "]"; }

// A module's point into simulation space: `world` says where it was given.
std::string PointIn(const std::string& p, bool world, bool local_sim) {
    if (world && local_sim) return "QRotateInv(EmitterRotation, " + p + " - EmitterPosition)";
    if (!world && !local_sim) return "(EmitterPosition + QRotate(EmitterRotation, " + p + "))";
    return p;
}
std::string DirectionIn(const std::string& d, bool world, bool local_sim) {
    if (world && local_sim) return "QRotateInv(EmitterRotation, " + d + ")";
    if (!world && !local_sim) return "QRotate(EmitterRotation, " + d + ")";
    return d;
}

void Build(const Emitter& e, Builder& b) {
    const bool local = e.settings.space == SimSpace::Local;
    auto name = [](const char* stage, usize i, const char* module, const char* field) {
        return std::string(stage) + "[" + std::to_string(i) + "] " + module + "." + field;
    };
    for (usize i = 0; i < e.init.size(); ++i) {
        std::visit(
            [&](const auto& m) {
                using T = std::decay_t<decltype(m)>;
                if (!m.enabled) return;
                std::string& c = b.init;
                if constexpr (std::is_same_v<T, InitLifetime>) {
                    const u32 a = b.Put(name("init", i, "InitLifetime", "seconds"), m.seconds.min, m.seconds.max);
                    c += "    p.lifetime = max(lerp(" + M(a) + ".x, " + M(a) + ".y, Rand(rng)), 1e-3);\n";
                } else if constexpr (std::is_same_v<T, InitShape>) {
                    const u32 a = b.Put(name("init", i, "InitShape", "radius, thickness"), m.radius, std::clamp(m.thickness, 0.0f, 1.0f));
                    const u32 h = b.Put(name("init", i, "InitShape", "half_extents"), m.half_extents.x, m.half_extents.y, m.half_extents.z);
                    const std::string r = M(a) + ".x", t = M(a) + ".y";
                    switch (m.shape) {
                    case ShapeKind::Point: c += "    local = 0;\n"; break;
                    case ShapeKind::Sphere:
                    case ShapeKind::Hemisphere:
                        c += "    { float3 d = OnSphere(rng);";
                        if (m.shape == ShapeKind::Hemisphere) c += " d.y = abs(d.y);";
                        c += " local = d * (" + r + " * (1.0 - " + t + " + " + t + " * pow(Rand(rng), 1.0 / 3.0))); }\n";
                        break;
                    case ShapeKind::Box:
                        c += "    { float3 h = " + M(h) + ".xyz; local = float3(lerp(-h.x, h.x, Rand(rng)), lerp(-h.y, h.y, Rand(rng)), lerp(-h.z, h.z, Rand(rng)));\n"
                             "      if (Rand(rng) >= " + t + ") { float ax = h.y * h.z, ay = h.x * h.z, az = h.x * h.y, pick = Rand(rng) * (ax + ay + az);\n"
                             "        float side = Rand(rng) < 0.5 ? -1.0 : 1.0;\n"
                             "        if (pick < ax) local.x = side * h.x; else if (pick < ax + ay) local.y = side * h.y; else local.z = side * h.z; } }\n";
                        break;
                    case ShapeKind::Cone:
                    case ShapeKind::Circle:
                        c += "    { float r = " + r + " * sqrt(1.0 - " + t + " + " + t + " * Rand(rng)), a = Rand(rng) * 6.28318530718; local = float3(r * cos(a), 0, r * sin(a)); }\n";
                        break;
                    case ShapeKind::Edge: c += "    local = float3(lerp(-" + r + ", " + r + ", Rand(rng)), 0, 0);\n"; break;
                    }
                } else if constexpr (std::is_same_v<T, InitVelocity>) {
                    const u32 a = b.Put(name("init", i, "InitVelocity", "direction, cone_angle"), m.direction.x, m.direction.y, m.direction.z, m.cone_angle);
                    const u32 s = b.Put(name("init", i, "InitVelocity", "speed"), m.speed.min, m.speed.max);
                    c += "    { float speed = lerp(" + M(s) + ".x, " + M(s) + ".y, Rand(rng)); float3 dir;\n";
                    switch (m.mode) {
                    case VelocityMode::Cone: c += "      dir = AlignY(InCone(rng, " + M(a) + ".w), " + M(a) + ".xyz);\n"; break;
                    case VelocityMode::Radial: c += "      dir = length(local) > 1e-6 ? normalize(local) : OnSphere(rng);\n"; break;
                    case VelocityMode::Direction: c += "      dir = SafeNormalize(" + M(a) + ".xyz, float3(0, 1, 0));\n"; break;
                    }
                    c += "      velocity = dir * speed; }\n";
                } else if constexpr (std::is_same_v<T, InitSize>) {
                    const u32 a = b.Put(name("init", i, "InitSize", "size"), m.size.min, m.size.max);
                    c += "    p.size = max(lerp(" + M(a) + ".x, " + M(a) + ".y, Rand(rng)), 0.0);\n";
                } else if constexpr (std::is_same_v<T, InitColor>) {
                    const u32 g = b.PutGradient(name("init", i, "InitColor", "color"), m.color);
                    c += "    p.color = SampleGradient(" + std::to_string(g) + ", Rand(rng));\n";
                } else if constexpr (std::is_same_v<T, InitRotation>) {
                    const u32 a = b.Put(name("init", i, "InitRotation", "angle, spin"), m.angle.min, m.angle.max, m.spin.min, m.spin.max);
                    c += "    angle = lerp(" + M(a) + ".x, " + M(a) + ".y, Rand(rng)); spin = lerp(" + M(a) + ".z, " + M(a) + ".w, Rand(rng));\n";
                } else if constexpr (std::is_same_v<T, InheritVelocity>) {
                    const u32 a = b.Put(name("init", i, "InheritVelocity", "amount"), m.amount);
                    c += "    inherited += EmitterVelocity * " + M(a) + ".x;\n";
                }
            },
            e.init[i]);
    }
    for (usize i = 0; i < e.update.size(); ++i) {
        std::visit(
            [&](const auto& m) {
                using T = std::decay_t<decltype(m)>;
                if (!m.enabled) return;
                if constexpr (std::is_same_v<T, Gravity>) {
                    const u32 a = b.Put(name("update", i, "Gravity", "acceleration"), m.acceleration.x, m.acceleration.y, m.acceleration.z);
                    b.forces += "    p.velocity += " + DirectionIn(M(a) + ".xyz", true, local) + " * dt;\n";
                } else if constexpr (std::is_same_v<T, Drag>) {
                    const u32 a = b.Put(name("update", i, "Drag", "coefficient"), m.coefficient);
                    b.forces += "    p.velocity *= exp(-" + M(a) + ".x * dt);\n";
                } else if constexpr (std::is_same_v<T, CurlNoiseForce>) {
                    b.uses_noise = true;
                    const u32 a = b.Put(name("update", i, "CurlNoiseForce", "strength, frequency, scroll"), m.strength, m.frequency, m.scroll);
                    b.forces += "    p.velocity += CurlNoise(p.position * " + M(a) + ".y + float3(0, Time * " + M(a) + ".z * " + M(a) + ".y, 0)) * (" + M(a) +
                                ".x * dt);\n";
                } else if constexpr (std::is_same_v<T, Vortex>) {
                    const u32 a = b.Put(name("update", i, "Vortex", "center, strength"), m.center.x, m.center.y, m.center.z, m.strength);
                    const u32 x = b.Put(name("update", i, "Vortex", "axis, pull"), m.axis.x, m.axis.y, m.axis.z, m.pull);
                    b.forces += "    { float3 c = " + PointIn(M(a) + ".xyz", m.world, local) + ", ax = SafeNormalize(" + DirectionIn(M(x) + ".xyz", m.world, local) +
                                ", float3(0, 1, 0));\n"
                                "      float3 r = p.position - c; float3 radial = r - ax * dot(r, ax); float d = length(radial);\n"
                                "      if (d >= 1e-5) p.velocity += (cross(ax, radial) / d * " + M(a) + ".w - radial * (" + M(x) + ".w / d)) * dt; }\n";
                } else if constexpr (std::is_same_v<T, PointAttractor>) {
                    const u32 a = b.Put(name("update", i, "PointAttractor", "position, strength"), m.position.x, m.position.y, m.position.z, m.strength);
                    const u32 r = b.Put(name("update", i, "PointAttractor", "radius, kill_radius"), m.radius, m.kill_radius);
                    b.forces += "    { float3 d = " + PointIn(M(a) + ".xyz", m.world, local) + " - p.position; float dist = length(d);\n"
                                "      if (" + M(r) + ".y > 0.0 && dist < " + M(r) + ".y) dead = true;\n"
                                "      if (dist >= 1e-5) { float falloff = " + M(r) + ".x > 0.0 ? max(0.0, 1.0 - dist / " + M(r) + ".x) : 1.0;\n"
                                "        p.velocity += d * (" + M(a) + ".w * falloff * dt / dist); } }\n";
                } else if constexpr (std::is_same_v<T, SpeedOverLife>) {
                    const u32 c = b.PutCurve(name("update", i, "SpeedOverLife", "curve"), m.curve);
                    b.forces += "    speed *= SampleCurve(" + std::to_string(c) + ", t);\n";
                } else if constexpr (std::is_same_v<T, CollisionPlane>) {
                    const Vec3 n = m.normal.Length() > 1e-8f ? m.normal.Normalized() : Vec3(0, 1, 0);
                    const u32 a = b.Put(name("update", i, "CollisionPlane", "normal, offset"), n.x, n.y, n.z, m.offset);
                    const u32 k = b.Put(name("update", i, "CollisionPlane", "bounce, friction, lifetime_loss, radius_scale"), m.bounce, m.friction, m.lifetime_loss,
                                        m.radius_scale);
                    b.post += "    { float3 n = SafeNormalize(" + DirectionIn(M(a) + ".xyz", m.world, local) + ", float3(0, 1, 0));\n"
                              "      float d = dot(" + PointIn(M(a) + ".xyz * " + M(a) + ".w", m.world, local) + ", n);\n"
                              "      float depth = dot(p.position, n) - d - p.size * " + M(k) + ".w;\n"
                              "      if (depth < 0.0) { p.position -= n * depth; Bounce(p, n, " + M(k) + ".xyz); } }\n";
                } else if constexpr (std::is_same_v<T, SceneCollision>) {
                    b.uses_depth = true;
                    const u32 a = b.Put(name("update", i, "SceneCollision", "bounce, friction, lifetime_loss, radius_scale"), m.bounce, m.friction, m.lifetime_loss,
                                        m.radius_scale);
                    const std::string to_world = local ? "EmitterPosition + QRotate(EmitterRotation, " : "(";
                    b.post += "    { float3 hit, n; float3 w = " + to_world + "p.position), from = " + to_world + "previous);\n"
                              "      if (DepthHit(w, from, hit, n)) { float3 at = hit + n * (p.size * " + M(a) + ".w);\n"
                              "        p.position = " + (local ? std::string("QRotateInv(EmitterRotation, at - EmitterPosition)") : std::string("at")) + ";\n"
                              "        Bounce(p, " + (local ? std::string("QRotateInv(EmitterRotation, n)") : std::string("n")) + ", " + M(a) + ".xyz); } }\n";
                } else if constexpr (std::is_same_v<T, KillVolume>) {
                    const u32 a = b.Put(name("update", i, "KillVolume", "center, radius"), m.center.x, m.center.y, m.center.z, m.radius);
                    const u32 h = b.Put(name("update", i, "KillVolume", "half_extents, kill_inside"), m.half_extents.x, m.half_extents.y, m.half_extents.z,
                                        m.kill_inside ? 1.0f : 0.0f);
                    b.post += "    { float3 c = " + PointIn(M(a) + ".xyz", m.world, local) + "; bool inside;\n";
                    if (m.shape == VolumeShape::Sphere) {
                        b.post += "      float3 r = p.position - c; inside = dot(r, r) <= " + M(a) + ".w * " + M(a) + ".w;\n";
                    } else {
                        std::string rel = "p.position - c";
                        if (!m.world && !local) rel = "QRotateInv(EmitterRotation, p.position - c)";
                        if (m.world && local) rel = "QRotate(EmitterRotation, p.position - c)";
                        b.post += "      float3 r = abs(" + rel + "); inside = all(r <= " + M(h) + ".xyz);\n";
                    }
                    b.post += "      if (inside == (" + M(h) + ".w > 0.5)) dead = true; }\n";
                } else if constexpr (std::is_same_v<T, SizeOverLife>) {
                    const u32 c = b.PutCurve(name("update", i, "SizeOverLife", "curve"), m.curve);
                    b.looks += "    p.size *= SampleCurve(" + std::to_string(c) + ", t);\n";
                } else if constexpr (std::is_same_v<T, ColorOverLife>) {
                    const u32 g = b.PutGradient(name("update", i, "ColorOverLife", "gradient"), m.gradient);
                    b.looks += "    p.color *= SampleGradient(" + std::to_string(g) + ", t);\n";
                }
            },
            e.update[i]);
    }
}

constexpr const char* kPrelude = R"(// Generated by Aether VFX (Phase 19 step 5). Do not edit.
struct Particle {
    float3 position; float age;
    float3 velocity; float lifetime;
    float4 color;
    float4 base_color;
    float size; float base_size; float rotation; float spin;
    uint seed; uint alive; uint2 pad;
};

cbuffer Frame : register(b0) {
    float DeltaTime; float Time; uint SpawnCount; uint FrameSeed;
    float3 EmitterPosition; uint MaxParticles;
    float4 EmitterRotation;
    float3 PreviousPosition; float DepthThickness;
    float3 EmitterVelocity; uint DepthWidth;
    uint DepthHeight; uint3 FramePad;
    float4x4 ViewProjection;
    float4x4 InverseViewProjection;
};

RWStructuredBuffer<Particle> Particles : register(u0);
RWStructuredBuffer<uint> DeadList : register(u1);  // [0]: how many; then free slots
RWStructuredBuffer<uint> AliveList : register(u2); // [0]: how many; then living slots (cleared by the CPU each frame)

uint PcgHash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
float Rand(inout uint s) { s = PcgHash(s); return (float)(s >> 8) * (1.0 / 16777216.0); }
float3 QRotate(float4 q, float3 v) { float3 t = 2.0 * cross(q.xyz, v); return v + q.w * t + cross(q.xyz, t); }
float3 QRotateInv(float4 q, float3 v) { return QRotate(float4(-q.xyz, q.w), v); }
float3 SafeNormalize(float3 v, float3 fallback) { float l = length(v); return l > 1e-8 ? v / l : fallback; }
float3 OnSphere(inout uint s) {
    float z = Rand(s) * 2.0 - 1.0, a = Rand(s) * 6.28318530718, r = sqrt(max(0.0, 1.0 - z * z));
    return float3(r * cos(a), r * sin(a), z);
}
float3 InCone(inout uint s, float angle) {
    float y = lerp(cos(clamp(angle, 0.0, 3.14159265)), 1.0, Rand(s)), a = Rand(s) * 6.28318530718, r = sqrt(max(0.0, 1.0 - y * y));
    return float3(r * cos(a), y, r * sin(a));
}
float3 AlignY(float3 v, float3 dir) {
    float3 up = SafeNormalize(dir, float3(0, 1, 0));
    float3 helper = abs(up.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 tangent = SafeNormalize(cross(helper, up), float3(1, 0, 0));
    return tangent * v.x + up * v.y + cross(up, tangent) * v.z;
}
void Bounce(inout Particle p, float3 n, float3 settings) { // bounce, friction, lifetime_loss
    float into = dot(p.velocity, n);
    if (into < 0.0) p.velocity = (p.velocity - n * into) * (1.0 - saturate(settings.y)) - n * (into * settings.x);
    p.age += p.lifetime * settings.z;
}
)";

constexpr const char* kTables = R"(float SampleCurve(uint first, float t) { // 16 samples in 4 float4s
    float x = saturate(t) * 15.0; uint i = min((uint)x, 14u); float f = x - (float)i;
    float a = M[first + i / 4u][i % 4u], b = M[first + (i + 1u) / 4u][(i + 1u) % 4u];
    return lerp(a, b, f);
}
float4 SampleGradient(uint first, float t) { // 16 float4s
    float x = saturate(t) * 15.0; uint i = min((uint)x, 14u);
    return lerp(M[first + i], M[first + i + 1u], x - (float)i);
}
)";

constexpr const char* kNoise = R"(uint NoiseHash(int x, int y, int z) {
    uint h = ((uint)x * 0x8da6b343u) ^ ((uint)y * 0xd8163841u) ^ ((uint)z * 0xcb1ab31fu);
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return h;
}
float Grad(uint h, float x, float y, float z) {
    switch (h % 12u) {
    case 0u: return x + y;  case 1u: return -x + y; case 2u: return x - y;  case 3u: return -x - y;
    case 4u: return x + z;  case 5u: return -x + z; case 6u: return x - z;  case 7u: return -x - z;
    case 8u: return y + z;  case 9u: return -y + z; case 10u: return y - z; default: return -y - z;
    }
}
float Fade(float t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }
float Noise3(float3 p) {
    float3 f = floor(p); int3 c = (int3)f; float3 d = p - f; float3 u = float3(Fade(d.x), Fade(d.y), Fade(d.z));
    float g000 = Grad(NoiseHash(c.x, c.y, c.z), d.x, d.y, d.z), g100 = Grad(NoiseHash(c.x + 1, c.y, c.z), d.x - 1, d.y, d.z);
    float g010 = Grad(NoiseHash(c.x, c.y + 1, c.z), d.x, d.y - 1, d.z), g110 = Grad(NoiseHash(c.x + 1, c.y + 1, c.z), d.x - 1, d.y - 1, d.z);
    float g001 = Grad(NoiseHash(c.x, c.y, c.z + 1), d.x, d.y, d.z - 1), g101 = Grad(NoiseHash(c.x + 1, c.y, c.z + 1), d.x - 1, d.y, d.z - 1);
    float g011 = Grad(NoiseHash(c.x, c.y + 1, c.z + 1), d.x, d.y - 1, d.z - 1), g111 = Grad(NoiseHash(c.x + 1, c.y + 1, c.z + 1), d.x - 1, d.y - 1, d.z - 1);
    return lerp(lerp(lerp(g000, g100, u.x), lerp(g010, g110, u.x), u.y), lerp(lerp(g001, g101, u.x), lerp(g011, g111, u.x), u.y), u.z);
}
float3 Psi(float3 q) { return float3(Noise3(q + float3(31.4, 0, 0)), Noise3(q + float3(0, 47.2, 0)), Noise3(q + float3(0, 0, 71.9))); }
float3 CurlNoise(float3 p) {
    const float e = 1e-2;
    float3 px0 = Psi(p - float3(e, 0, 0)), px1 = Psi(p + float3(e, 0, 0));
    float3 py0 = Psi(p - float3(0, e, 0)), py1 = Psi(p + float3(0, e, 0));
    float3 pz0 = Psi(p - float3(0, 0, e)), pz1 = Psi(p + float3(0, 0, e));
    float k = 1.0 / (2.0 * e);
    return float3((py1.z - py0.z) - (pz1.y - pz0.y), (pz1.x - pz0.x) - (px1.z - px0.z), (px1.y - px0.y) - (py1.x - py0.x)) * k;
}
)";

constexpr const char* kDepth = R"(Texture2D<float> SceneDepth : register(t0);
bool SurfaceAt(int2 px, out float3 surface) {
    surface = 0;
    if (px.x < 0 || px.y < 0 || px.x >= (int)DepthWidth || px.y >= (int)DepthHeight) return false;
    float d = SceneDepth.Load(int3(px, 0));
    if (d >= 1.0) return false;
    float2 ndc = float2((px.x + 0.5) / DepthWidth * 2.0 - 1.0, 1.0 - (px.y + 0.5) / DepthHeight * 2.0);
    float4 w = mul(InverseViewProjection, float4(ndc, d, 1.0));
    surface = w.xyz / w.w;
    return true;
}
bool DepthHit(float3 p, float3 from, out float3 hit, out float3 normal) { // as DepthBufferCollider does, at the particle's new place
    hit = 0; normal = float3(0, 1, 0);
    float4 clip = mul(ViewProjection, float4(p, 1.0));
    if (clip.w <= 1e-6) return false;
    float3 ndc = clip.xyz / clip.w;
    if (any(abs(ndc.xy) > 1.0)) return false;
    int2 px = int2((ndc.x * 0.5 + 0.5) * DepthWidth, (0.5 - ndc.y * 0.5) * DepthHeight);
    px = clamp(px, int2(0, 0), int2(DepthWidth - 1, DepthHeight - 1));
    if (ndc.z <= SceneDepth.Load(int3(px, 0))) return false;
    float3 s, sx, sy;
    if (!SurfaceAt(px, s) || length(p - s) > DepthThickness) return false;
    bool hx = SurfaceAt(px + int2(1, 0), sx) || SurfaceAt(px - int2(1, 0), sx);
    bool hy = SurfaceAt(px + int2(0, 1), sy) || SurfaceAt(px - int2(0, 1), sy);
    if (hx && hy) { float3 c = cross(sx - s, sy - s); if (length(c) > 1e-12) normal = normalize(c); }
    if (dot(normal, from - s) < 0.0) normal = -normal;
    hit = s;
    return true;
}
)";

u64 Fnv(const std::string& s) {
    u64 h = 1469598103934665603ULL;
    for (const char c : s) h = (h ^ static_cast<u8>(c)) * 1099511628211ULL;
    return h;
}

} // namespace

GpuSupport CheckGpuSupport(const Emitter& e) {
    GpuSupport s;
    if (!e.sub_emitters.empty()) s.reasons.push_back("sub-emitters need its particles' events back on the CPU");
    for (const RenderModule& m : e.render) {
        if (const auto* l = std::get_if<LightRenderer>(&m); l != nullptr && l->enabled) s.reasons.push_back("light renderers need its particles back on the CPU");
        if (const auto* r = std::get_if<RibbonRenderer>(&m); r != nullptr && r->enabled) s.reasons.push_back("ribbons need its particles in birth order");
    }
    s.supported = s.reasons.empty();
    return s;
}

SimTarget ChooseSimTarget(const Emitter& e, bool gpu_available, u32 threshold) {
    const bool can = gpu_available && CheckGpuSupport(e).supported;
    switch (e.settings.target) {
    case SimTarget::Cpu: return SimTarget::Cpu;
    case SimTarget::Gpu: return can ? SimTarget::Gpu : SimTarget::Cpu;
    case SimTarget::Auto: return can && e.settings.max_particles >= threshold ? SimTarget::Gpu : SimTarget::Cpu;
    }
    return SimTarget::Cpu;
}

std::vector<f32> BakeCurve(const FloatCurve& c, u32 samples) {
    std::vector<f32> out(samples);
    for (u32 i = 0; i < samples; ++i) out[i] = c.Evaluate(samples > 1 ? static_cast<f32>(i) / static_cast<f32>(samples - 1) : 0.0f);
    return out;
}

std::vector<LinearColor> BakeGradient(const ColorGradient& g, u32 samples) {
    std::vector<LinearColor> out(samples);
    for (u32 i = 0; i < samples; ++i) out[i] = g.Evaluate(samples > 1 ? static_cast<f32>(i) / static_cast<f32>(samples - 1) : 0.0f);
    return out;
}

f32 SampleBakedCurve(const std::vector<f32>& table, f32 t) {
    if (table.empty()) return 0.0f;
    if (table.size() == 1) return table[0];
    const f32 x = std::clamp(t, 0.0f, 1.0f) * static_cast<f32>(table.size() - 1);
    const usize i = std::min(static_cast<usize>(x), table.size() - 2);
    return table[i] + (table[i + 1] - table[i]) * (x - static_cast<f32>(i));
}

std::vector<f32> PackModuleConstants(const Emitter& e) {
    Builder b;
    Build(e, b);
    return b.values;
}

GeneratedParticleShader GenerateParticleShader(const Emitter& e) {
    GeneratedParticleShader out;
    const GpuSupport support = CheckGpuSupport(e);
    if (!support.supported) {
        out.errors = support.reasons;
        return out;
    }
    Builder b;
    Build(e, b);
    const bool local = e.settings.space == SimSpace::Local;
    const u32 count = std::max<u32>(static_cast<u32>(b.values.size() / 4), 1u);
    std::string h = kPrelude;
    h += "cbuffer Modules : register(b1) { float4 M[" + std::to_string(count) + "]; };\n\n";
    h += kTables;
    if (b.uses_noise) h += kNoise;
    if (b.uses_depth) h += kDepth;
    h += "\n[numthreads(64, 1, 1)]\nvoid ResetMain(uint3 id : SV_DispatchThreadID) { // once: every slot free\n"
         "    if (id.x == 0) DeadList[0] = MaxParticles;\n"
         "    if (id.x >= MaxParticles) return;\n"
         "    DeadList[id.x + 1] = id.x;\n"
         "    Particles[id.x].alive = 0;\n"
         "}\n\n";
    h += "[numthreads(64, 1, 1)]\nvoid EmitMain(uint3 id : SV_DispatchThreadID) {\n"
         "    if (id.x >= SpawnCount) return;\n"
         "    uint left;\n"
         "    InterlockedAdd(DeadList[0], 0xFFFFFFFFu, left); // take one\n"
         "    if (left == 0u || left > MaxParticles) { InterlockedAdd(DeadList[0], 1u); return; }\n"
         "    uint index = DeadList[left];\n"
         "    uint rng = PcgHash(FrameSeed ^ PcgHash(id.x + 0x9E3779B9u));\n"
         "    float3 origin = lerp(PreviousPosition, EmitterPosition, Rand(rng)); // along this frame's motion\n"
         "    Particle p = (Particle)0;\n"
         "    p.lifetime = 1.0; p.size = 0.1; p.color = float4(1, 1, 1, 1);\n"
         "    float3 local = 0, velocity = 0, inherited = 0; float angle = 0, spin = 0;\n";
    h += b.init;
    if (local) {
        h += "    p.position = local;\n    p.velocity = velocity + QRotateInv(EmitterRotation, inherited);\n";
    } else {
        h += "    p.position = origin + QRotate(EmitterRotation, local);\n    p.velocity = QRotate(EmitterRotation, velocity) + inherited;\n";
    }
    h += "    p.base_size = p.size; p.base_color = p.color; p.rotation = angle; p.spin = spin;\n"
         "    p.seed = PcgHash(rng); p.alive = 1u; p.age = 0.0;\n"
         "    Particles[index] = p;\n"
         "}\n\n";
    h += "[numthreads(64, 1, 1)]\nvoid UpdateMain(uint3 id : SV_DispatchThreadID) {\n"
         "    if (id.x >= MaxParticles) return;\n"
         "    Particle p = Particles[id.x];\n"
         "    if (p.alive == 0u) return;\n"
         "    float dt = DeltaTime;\n"
         "    p.age += dt;\n"
         "    bool dead = false;\n"
         "    float t = saturate(p.age / p.lifetime), speed = 1.0;\n";
    h += b.forces;
    h += "    float3 previous = p.position;\n    p.position += p.velocity * (speed * dt);\n";
    h += b.post;
    h += "    t = saturate(p.age / p.lifetime);\n"
         "    p.size = p.base_size; p.color = p.base_color; p.rotation += p.spin * dt;\n";
    h += b.looks;
    h += "    if (dead || p.age >= p.lifetime) {\n"
         "        p.alive = 0u;\n"
         "        Particles[id.x] = p;\n"
         "        uint slot;\n"
         "        InterlockedAdd(DeadList[0], 1u, slot);\n"
         "        DeadList[slot + 1u] = id.x;\n"
         "        return;\n"
         "    }\n"
         "    Particles[id.x] = p;\n"
         "    uint at;\n"
         "    InterlockedAdd(AliveList[0], 1u, at);\n"
         "    AliveList[at + 1u] = id.x;\n"
         "}\n";
    out.ok = true;
    out.hlsl = std::move(h);
    out.module_constants = static_cast<u32>(b.values.size() / 4);
    out.constant_names = std::move(b.names);
    out.uses_depth = b.uses_depth;
    out.uses_noise = b.uses_noise;
    out.key = Fnv(out.hlsl);
    return out;
}

std::vector<u32> PackFrameConstants(const GpuFrameConstants& f) {
    std::vector<u32> w(kFrameConstantWords, 0);
    auto put = [&](usize at, f32 v) { std::memcpy(&w[at], &v, sizeof v); };
    put(0, f.delta_time), put(1, f.time), w[2] = f.spawn_count, w[3] = f.frame_seed;
    put(4, f.emitter_position.x), put(5, f.emitter_position.y), put(6, f.emitter_position.z), w[7] = f.max_particles;
    put(8, f.emitter_rotation.x), put(9, f.emitter_rotation.y), put(10, f.emitter_rotation.z), put(11, f.emitter_rotation.w);
    put(12, f.previous_position.x), put(13, f.previous_position.y), put(14, f.previous_position.z), put(15, f.depth_thickness);
    put(16, f.emitter_velocity.x), put(17, f.emitter_velocity.y), put(18, f.emitter_velocity.z), w[19] = f.depth_width;
    w[20] = f.depth_height;
    for (int c = 0; c < 4; ++c) {
        // Column-major, as HLSL cbuffers store a float4x4 by default.
        put(24 + c * 4 + 0, f.view_projection.cols[c].x), put(24 + c * 4 + 1, f.view_projection.cols[c].y);
        put(24 + c * 4 + 2, f.view_projection.cols[c].z), put(24 + c * 4 + 3, f.view_projection.cols[c].w);
        put(40 + c * 4 + 0, f.inverse_view_projection.cols[c].x), put(40 + c * 4 + 1, f.inverse_view_projection.cols[c].y);
        put(40 + c * 4 + 2, f.inverse_view_projection.cols[c].z), put(40 + c * 4 + 3, f.inverse_view_projection.cols[c].w);
    }
    return w;
}

GpuEmitterDriver::GpuEmitterDriver(const Emitter& emitter, u64 seed) : emitter_(emitter), seed_(seed) {
    // A copy that only spawns: its particles are counted, then dropped.
    Emitter counting = emitter;
    counting.init.clear();
    counting.update.clear();
    counting.render.clear();
    counting.sub_emitters.clear();
    counting.bindings.clear();
    counting.settings.max_particles = 1u << 16; // births per frame at most
    spawner_ = std::make_unique<EmitterInstance>(counting, seed);
}

bool GpuEmitterDriver::SetField(const std::string& field, const ParameterValue& value, std::string* error) {
    if (!SetModuleField(emitter_, field, value, error)) return false;
    if (field.rfind("spawn[", 0) == 0) (void)spawner_->SetField(field, value); // same indices there
    return true;
}

void GpuEmitterDriver::SetPose(const EmitterPose& pose) {
    if (!placed_) previous_ = pose.position;
    placed_ = true;
    spawner_->SetPose(pose);
}

void GpuEmitterDriver::Teleport(const EmitterPose& pose) {
    previous_ = pose.position;
    placed_ = true;
    spawner_->Teleport(pose);
}

GpuFrameConstants GpuEmitterDriver::Update(f32 dt) {
    GpuFrameConstants f;
    const u64 before = spawner_->TotalSpawned();
    const Vec3 from = previous_;
    spawner_->Update(dt);
    spawner_->Clear();
    const u64 born = spawner_->TotalSpawned() - before;
    total_ += born;
    f.delta_time = dt;
    f.time = spawner_->Time();
    f.spawn_count = static_cast<u32>(born);
    f.frame_seed = static_cast<u32>((seed_ * 0x9E3779B97F4A7C15ULL + ++frame_ * 0xBF58476D1CE4E5B9ULL) >> 32);
    f.emitter_position = spawner_->Pose().position;
    f.emitter_rotation = spawner_->Pose().rotation;
    f.previous_position = from;
    f.emitter_velocity = dt > 0.0f ? (f.emitter_position - from) * (1.0f / dt) : Vec3{};
    f.max_particles = emitter_.settings.max_particles;
    previous_ = f.emitter_position;
    return f;
}

} // namespace aether::vfx
