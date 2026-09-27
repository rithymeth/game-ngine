#include "aether/vfx/curves.h"

#include <algorithm>
#include <cmath>

namespace aether::vfx {

using nlohmann::json;

namespace {
bool Fail(std::string* error, const std::string& m) {
    if (error != nullptr) *error = m;
    return false;
}

bool Numbers(const json& j, usize n) {
    return j.is_array() && j.size() == n && std::all_of(j.begin(), j.end(), [](const json& x) { return x.is_number(); });
}

const char* kInterp[] = {"Linear", "Smooth", "Constant"};
} // namespace

LinearColor Lerp(const LinearColor& a, const LinearColor& b, f32 t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

bool FloatCurve::Sorted() const {
    return std::is_sorted(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.time < b.time; });
}

f32 FloatCurve::Evaluate(f32 t) const {
    if (keys.empty()) return 0.0f;
    if (t <= keys.front().time) return keys.front().value;
    if (t >= keys.back().time) return keys.back().value;
    // The segment [i, i + 1] holding t.
    usize i = 0; // on a key, the segment it starts (so a held value changes at its key)
    while (i + 2 < keys.size() && keys[i + 1].time <= t) ++i;
    const Key& a = keys[i];
    const Key& b = keys[i + 1];
    const f32 span = b.time - a.time;
    if (span <= 0.0f) return b.value;
    const f32 u = (t - a.time) / span;
    switch (interp) {
    case CurveInterp::Constant: return a.value;
    case CurveInterp::Linear: return a.value + (b.value - a.value) * u;
    case CurveInterp::Smooth: {
        // Catmull-Rom tangents from the neighbours (scaled to this segment's span); flat at the ends.
        auto slope = [&](usize k) {
            if (k == 0 || k + 1 >= keys.size()) return 0.0f;
            const f32 dt = keys[k + 1].time - keys[k - 1].time;
            return dt > 0.0f ? (keys[k + 1].value - keys[k - 1].value) / dt : 0.0f;
        };
        const f32 m0 = slope(i) * span, m1 = slope(i + 1) * span;
        const f32 u2 = u * u, u3 = u2 * u;
        return (2 * u3 - 3 * u2 + 1) * a.value + (u3 - 2 * u2 + u) * m0 + (-2 * u3 + 3 * u2) * b.value + (u3 - u2) * m1;
    }
    }
    return a.value;
}

LinearColor ColorGradient::Evaluate(f32 t) const {
    LinearColor out;
    if (!colors.empty()) {
        if (t <= colors.front().time) {
            out.r = colors.front().r, out.g = colors.front().g, out.b = colors.front().b;
        } else if (t >= colors.back().time) {
            out.r = colors.back().r, out.g = colors.back().g, out.b = colors.back().b;
        } else {
            usize i = 0;
            while (i + 1 < colors.size() && colors[i + 1].time < t) ++i;
            const ColorKey& a = colors[i];
            const ColorKey& b = colors[i + 1];
            const f32 u = b.time > a.time ? (t - a.time) / (b.time - a.time) : 1.0f;
            out.r = a.r + (b.r - a.r) * u, out.g = a.g + (b.g - a.g) * u, out.b = a.b + (b.b - a.b) * u;
        }
    }
    if (!alphas.empty()) {
        if (t <= alphas.front().time) {
            out.a = alphas.front().a;
        } else if (t >= alphas.back().time) {
            out.a = alphas.back().a;
        } else {
            usize i = 0;
            while (i + 1 < alphas.size() && alphas[i + 1].time < t) ++i;
            const AlphaKey& a = alphas[i];
            const AlphaKey& b = alphas[i + 1];
            const f32 u = b.time > a.time ? (t - a.time) / (b.time - a.time) : 1.0f;
            out.a = a.a + (b.a - a.a) * u;
        }
    }
    return out;
}

// --- Random ------------------------------------------------------------------------------------------

void Random::Seed(u64 seed) {
    state_ = 0;
    inc_ = (seed << 1u) | 1u;
    (void)NextU32();
    state_ += seed ^ 0x9E3779B97F4A7C15ULL;
    (void)NextU32();
}

u32 Random::NextU32() {
    const u64 old = state_;
    state_ = old * 6364136223846793005ULL + inc_;
    const u32 xorshifted = static_cast<u32>(((old >> 18u) ^ old) >> 27u);
    const u32 rot = static_cast<u32>(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
}

f32 Random::Next01() { return static_cast<f32>(NextU32() >> 8) * (1.0f / 16777216.0f); }

Vec3 Random::OnUnitSphere() {
    const f32 z = Range(-1.0f, 1.0f), a = Range(0.0f, 6.28318530718f);
    const f32 r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    return {r * std::cos(a), r * std::sin(a), z};
}

Vec3 Random::InUnitSphere() { return OnUnitSphere() * std::cbrt(Next01()); }

Vec3 Random::InCone(f32 angle) {
    // Uniform over the spherical cap around +Y.
    const f32 cos_max = std::cos(std::clamp(angle, 0.0f, 3.14159265f));
    const f32 y = Range(cos_max, 1.0f), a = Range(0.0f, 6.28318530718f);
    const f32 r = std::sqrt(std::max(0.0f, 1.0f - y * y));
    return {r * std::cos(a), y, r * std::sin(a)};
}

// --- Noise -------------------------------------------------------------------------------------------

namespace {
u32 Hash(i32 x, i32 y, i32 z) {
    u32 h = static_cast<u32>(x) * 0x8da6b343u ^ static_cast<u32>(y) * 0xd8163841u ^ static_cast<u32>(z) * 0xcb1ab31fu;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}

f32 Gradient(u32 h, f32 x, f32 y, f32 z) {
    // One of 12 edge directions of a cube.
    switch (h % 12u) {
    case 0: return x + y;
    case 1: return -x + y;
    case 2: return x - y;
    case 3: return -x - y;
    case 4: return x + z;
    case 5: return -x + z;
    case 6: return x - z;
    case 7: return -x - z;
    case 8: return y + z;
    case 9: return -y + z;
    case 10: return y - z;
    default: return -y - z;
    }
}

f32 Fade(f32 t) { return t * t * t * (t * (t * 6 - 15) + 10); }
} // namespace

f32 Noise3(const Vec3& p) {
    // Gradient (Perlin) noise.
    const f32 fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
    const i32 x = static_cast<i32>(fx), y = static_cast<i32>(fy), z = static_cast<i32>(fz);
    const f32 dx = p.x - fx, dy = p.y - fy, dz = p.z - fz;
    const f32 u = Fade(dx), v = Fade(dy), w = Fade(dz);
    auto g = [&](i32 i, i32 j, i32 k) { return Gradient(Hash(x + i, y + j, z + k), dx - static_cast<f32>(i), dy - static_cast<f32>(j), dz - static_cast<f32>(k)); };
    auto lerp = [](f32 a, f32 b, f32 t) { return a + (b - a) * t; };
    const f32 x00 = lerp(g(0, 0, 0), g(1, 0, 0), u), x10 = lerp(g(0, 1, 0), g(1, 1, 0), u);
    const f32 x01 = lerp(g(0, 0, 1), g(1, 0, 1), u), x11 = lerp(g(0, 1, 1), g(1, 1, 1), u);
    return lerp(lerp(x00, x10, v), lerp(x01, x11, v), w);
}

Vec3 CurlNoise(const Vec3& p) {
    // The curl of a potential made of three offset noise fields, by central differences.
    constexpr f32 e = 1e-2f;
    const Vec3 o1{31.4f, 0.0f, 0.0f}, o2{0.0f, 47.2f, 0.0f}, o3{0.0f, 0.0f, 71.9f};
    auto psi = [&](const Vec3& q) { return Vec3(Noise3(q + o1), Noise3(q + o2), Noise3(q + o3)); };
    const Vec3 px0 = psi(p - Vec3(e, 0, 0)), px1 = psi(p + Vec3(e, 0, 0));
    const Vec3 py0 = psi(p - Vec3(0, e, 0)), py1 = psi(p + Vec3(0, e, 0));
    const Vec3 pz0 = psi(p - Vec3(0, 0, e)), pz1 = psi(p + Vec3(0, 0, e));
    const f32 k = 1.0f / (2.0f * e);
    // curl = (dPz/dy - dPy/dz, dPx/dz - dPz/dx, dPy/dx - dPx/dy)
    return Vec3(((py1.z - py0.z) - (pz1.y - pz0.y)) * k, ((pz1.x - pz0.x) - (px1.z - px0.z)) * k, ((px1.y - px0.y) - (py1.x - py0.x)) * k);
}

// --- JSON --------------------------------------------------------------------------------------------

json ToJson(const FloatRange& r) { return r.min == r.max ? json(r.min) : json::array({r.min, r.max}); }

json ToJson(const FloatCurve& c) {
    json keys = json::array();
    for (const FloatCurve::Key& k : c.keys) keys.push_back({k.time, k.value});
    if (c.interp == CurveInterp::Linear && c.keys.size() == 1 && c.keys[0].time == 0.0f) return c.keys[0].value; // a constant
    json j = {{"keys", keys}};
    if (c.interp != CurveInterp::Linear) j["interp"] = kInterp[static_cast<usize>(c.interp)];
    return j;
}

json ToJson(const ColorGradient& g) {
    json colors = json::array(), alphas = json::array();
    for (const auto& k : g.colors) colors.push_back({k.time, k.r, k.g, k.b});
    for (const auto& k : g.alphas) alphas.push_back({k.time, k.a});
    return {{"colors", colors}, {"alphas", alphas}};
}

json ToJson(const LinearColor& c) { return {c.r, c.g, c.b, c.a}; }
json ToJson(const Vec3& v) { return {v.x, v.y, v.z}; }

bool FromJson(const json& j, FloatRange& out, std::string* error) {
    if (j.is_number()) {
        out = FloatRange::Constant(j.get<f32>());
        return true;
    }
    if (!Numbers(j, 2)) return Fail(error, "a range is a number or [min, max]");
    out = {j[0].get<f32>(), j[1].get<f32>()};
    return true;
}

bool FromJson(const json& j, FloatCurve& out, std::string* error) {
    if (j.is_number()) {
        out = FloatCurve::Constant(j.get<f32>());
        return true;
    }
    if (!j.is_object() || !j.contains("keys") || !j["keys"].is_array()) return Fail(error, "a curve is a number or {\"keys\": [[t, v], ...]}");
    FloatCurve c;
    for (const json& k : j["keys"]) {
        if (!Numbers(k, 2)) return Fail(error, "a curve key is [time, value]");
        c.keys.push_back({k[0].get<f32>(), k[1].get<f32>()});
    }
    if (j.contains("interp")) {
        const std::string s = j["interp"].is_string() ? j["interp"].get<std::string>() : std::string();
        const auto it = std::find_if(std::begin(kInterp), std::end(kInterp), [&](const char* n) { return s == n; });
        if (it == std::end(kInterp)) return Fail(error, "unknown curve interpolation '" + s + "'");
        c.interp = static_cast<CurveInterp>(it - std::begin(kInterp));
    }
    std::stable_sort(c.keys.begin(), c.keys.end(), [](const FloatCurve::Key& a, const FloatCurve::Key& b) { return a.time < b.time; });
    out = std::move(c);
    return true;
}

bool FromJson(const json& j, ColorGradient& out, std::string* error) {
    if (j.is_array()) { // a single colour
        LinearColor c;
        if (!FromJson(j, c, error)) return false;
        out = ColorGradient::Constant(c);
        return true;
    }
    if (!j.is_object()) return Fail(error, "a gradient is a colour or {\"colors\": [...], \"alphas\": [...]}");
    ColorGradient g;
    for (const json& k : j.value("colors", json::array())) {
        if (!Numbers(k, 4)) return Fail(error, "a gradient colour key is [time, r, g, b]");
        g.colors.push_back({k[0].get<f32>(), k[1].get<f32>(), k[2].get<f32>(), k[3].get<f32>()});
    }
    for (const json& k : j.value("alphas", json::array())) {
        if (!Numbers(k, 2)) return Fail(error, "a gradient alpha key is [time, alpha]");
        g.alphas.push_back({k[0].get<f32>(), k[1].get<f32>()});
    }
    std::stable_sort(g.colors.begin(), g.colors.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    std::stable_sort(g.alphas.begin(), g.alphas.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    out = std::move(g);
    return true;
}

bool FromJson(const json& j, LinearColor& out, std::string* error) {
    if (Numbers(j, 3)) {
        out = {j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>(), 1.0f};
        return true;
    }
    if (!Numbers(j, 4)) return Fail(error, "a colour is [r, g, b(, a)]");
    out = {j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>(), j[3].get<f32>()};
    return true;
}

bool FromJson(const json& j, Vec3& out, std::string* error) {
    if (!Numbers(j, 3)) return Fail(error, "a vector is [x, y, z]");
    out = Vec3(j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>());
    return true;
}

} // namespace aether::vfx
