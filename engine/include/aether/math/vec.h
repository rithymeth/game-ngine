#pragma once

// @stability: stable

#include "aether/core/base.h"

#include <cmath>
#if defined(__aarch64__) || defined(_M_ARM64)
// ARM (Apple Silicon, Android): the same SSE intrinsics, mapped to NEON.
#include <sse2neon.h>
#else
#include <immintrin.h>
#endif

namespace aether {

struct Vec3 {
    f32 x, y, z;

    Vec3() : x(0), y(0), z(0) {}
    Vec3(f32 x_, f32 y_, f32 z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(f32 s) const { return {x * s, y * s, z * s}; }

    f32 Dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 Cross(const Vec3& o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    f32 LengthSq() const { return Dot(*this); }
    f32 Length() const { return std::sqrt(LengthSq()); }
    Vec3 Normalized() const {
        f32 len = Length();
        return (len > 0.0f) ? (*this) * (1.0f / len) : Vec3{};
    }
};

// A double-precision position (Phase 42.1, large worlds): the absolute world
// position a WorldOrigin rebases into the f32 Vec3 the rest of the engine uses.
// Not SIMD-backed; keep it to the arithmetic a position needs.
struct Vec3d {
    f64 x = 0.0, y = 0.0, z = 0.0;

    Vec3d() = default;
    Vec3d(f64 x_, f64 y_, f64 z_) : x(x_), y(y_), z(z_) {}

    Vec3d operator+(const Vec3d& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3d operator-(const Vec3d& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3d operator*(f64 s) const { return {x * s, y * s, z * s}; }

    f64 Dot(const Vec3d& o) const { return x * o.x + y * o.y + z * o.z; }
    f64 LengthSq() const { return Dot(*this); }
    f64 Length() const { return std::sqrt(LengthSq()); }

    Vec3 ToVec3() const { return {static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(z)}; }
    static Vec3d FromVec3(const Vec3& v) { return {v.x, v.y, v.z}; }
};

// 16-byte aligned, SSE-backed 4-wide vector. Used for positions, rotations
// (via Quaternion) and anywhere hot loops benefit from packed SIMD ops.
struct AETHER_ALIGN(16) Vec4 {
    union {
        __m128 simd;
        struct { f32 x, y, z, w; };
    };

    Vec4() : simd(_mm_setzero_ps()) {}
    Vec4(f32 x_, f32 y_, f32 z_, f32 w_) : simd(_mm_set_ps(w_, z_, y_, x_)) {}
    explicit Vec4(__m128 v) : simd(v) {}

    Vec4 operator+(const Vec4& o) const { return Vec4(_mm_add_ps(simd, o.simd)); }
    Vec4 operator-(const Vec4& o) const { return Vec4(_mm_sub_ps(simd, o.simd)); }
    Vec4 operator*(const Vec4& o) const { return Vec4(_mm_mul_ps(simd, o.simd)); }
    Vec4 operator*(f32 s) const { return Vec4(_mm_mul_ps(simd, _mm_set1_ps(s))); }

    f32 Dot(const Vec4& o) const {
        __m128 mul = _mm_mul_ps(simd, o.simd);
        __m128 shuf = _mm_shuffle_ps(mul, mul, _MM_SHUFFLE(2, 3, 0, 1));
        __m128 sums = _mm_add_ps(mul, shuf);
        shuf = _mm_movehl_ps(shuf, sums);
        sums = _mm_add_ss(sums, shuf);
        return _mm_cvtss_f32(sums);
    }

    f32 LengthSq() const { return Dot(*this); }
    f32 Length() const { return std::sqrt(LengthSq()); }

    Vec4 Normalized() const {
        f32 len = Length();
        return (len > 0.0f) ? (*this) * (1.0f / len) : Vec4{};
    }
};

} // namespace aether
