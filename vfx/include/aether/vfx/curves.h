#pragma once

#include "aether/core/base.h"
#include "aether/math/vec.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace aether::vfx {

// Values that change over a particle's life, or vary between particles
// (Phase 19 step 1, §19.1). Times are normalized (0 at birth, 1 at death)
// wherever a curve is sampled "over life".

struct LinearColor {
    f32 r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
    LinearColor operator*(const LinearColor& o) const { return {r * o.r, g * o.g, b * o.b, a * o.a}; }
    bool operator==(const LinearColor& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
};
LinearColor Lerp(const LinearColor& a, const LinearColor& b, f32 t);

// A random value between min and max (equal: a constant).
struct FloatRange {
    f32 min = 0.0f, max = 0.0f;
    static FloatRange Constant(f32 v) { return {v, v}; }
    bool operator==(const FloatRange& o) const { return min == o.min && max == o.max; }
};

enum class CurveInterp : u8 {
    Linear,
    Smooth,   // Catmull-Rom through the keys (flat at the ends)
    Constant, // holds each key's value until the next
};

// A float curve: keys in time order, clamped outside them. No keys is 0.
struct FloatCurve {
    struct Key {
        f32 time = 0.0f;
        f32 value = 0.0f;
        bool operator==(const Key& o) const { return time == o.time && value == o.value; }
    };
    std::vector<Key> keys;
    CurveInterp interp = CurveInterp::Linear;

    static FloatCurve Constant(f32 v) { return {{{0.0f, v}}, CurveInterp::Linear}; }
    static FloatCurve Line(f32 from, f32 to) { return {{{0.0f, from}, {1.0f, to}}, CurveInterp::Linear}; }
    f32 Evaluate(f32 t) const;
    bool Sorted() const;
    bool operator==(const FloatCurve& o) const { return keys == o.keys && interp == o.interp; }
};

// A colour gradient: colour and alpha keys apart (as in Unity), each
// linear between keys and clamped outside them.
struct ColorGradient {
    struct ColorKey {
        f32 time = 0.0f;
        f32 r = 1.0f, g = 1.0f, b = 1.0f;
        bool operator==(const ColorKey& o) const { return time == o.time && r == o.r && g == o.g && b == o.b; }
    };
    struct AlphaKey {
        f32 time = 0.0f;
        f32 a = 1.0f;
        bool operator==(const AlphaKey& o) const { return time == o.time && a == o.a; }
    };
    std::vector<ColorKey> colors;
    std::vector<AlphaKey> alphas;

    static ColorGradient Constant(const LinearColor& c) { return {{{0.0f, c.r, c.g, c.b}}, {{0.0f, c.a}}}; }
    static ColorGradient Fade(const LinearColor& from, const LinearColor& to) {
        return {{{0.0f, from.r, from.g, from.b}, {1.0f, to.r, to.g, to.b}}, {{0.0f, from.a}, {1.0f, to.a}}};
    }
    LinearColor Evaluate(f32 t) const;
    bool operator==(const ColorGradient& o) const { return colors == o.colors && alphas == o.alphas; }
};

// A small, fast, seedable generator (PCG32): the same seed gives the same
// particles, so effects are repeatable (the editor's scrubber, replays).
class Random {
public:
    explicit Random(u64 seed = 0x853c49e6748fea9bULL) { Seed(seed); }
    void Seed(u64 seed);
    u32 NextU32();
    f32 Next01(); // [0, 1)
    f32 Range(f32 min, f32 max) { return min + (max - min) * Next01(); }
    f32 Range(const FloatRange& r) { return Range(r.min, r.max); }
    Vec3 InUnitSphere();
    Vec3 OnUnitSphere();
    // A direction within `angle` radians of +Y (uniform over the cap).
    Vec3 InCone(f32 angle);

private:
    u64 state_ = 0, inc_ = 1;
};

// Smooth 3D noise (-1..1 roughly) and its curl: divergence-free swirls for
// particle motion.
f32 Noise3(const Vec3& p);
Vec3 CurlNoise(const Vec3& p);

// JSON: a number is a constant; ranges are [min, max]; curves are
// {"keys": [[t, v], ...], "interp": "Smooth"}; gradients are
// {"colors": [[t, r, g, b], ...], "alphas": [[t, a], ...]}; colours [r, g, b(, a)].
nlohmann::json ToJson(const FloatRange& r);
nlohmann::json ToJson(const FloatCurve& c);
nlohmann::json ToJson(const ColorGradient& g);
nlohmann::json ToJson(const LinearColor& c);
nlohmann::json ToJson(const Vec3& v);
bool FromJson(const nlohmann::json& j, FloatRange& out, std::string* error = nullptr);
bool FromJson(const nlohmann::json& j, FloatCurve& out, std::string* error = nullptr);
bool FromJson(const nlohmann::json& j, ColorGradient& out, std::string* error = nullptr);
bool FromJson(const nlohmann::json& j, LinearColor& out, std::string* error = nullptr);
bool FromJson(const nlohmann::json& j, Vec3& out, std::string* error = nullptr);

} // namespace aether::vfx
