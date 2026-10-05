#pragma once

#include "aether/math/math.h"

#include <cmath>

// Free-function vector helpers the terrain sources are written with (Vec3
// has them as members).
namespace aether::terrain {

inline f32 Dot(const Vec3& a, const Vec3& b) { return a.Dot(b); }
inline Vec3 Cross(const Vec3& a, const Vec3& b) { return a.Cross(b); }
inline f32 Length(const Vec3& v) { return v.Length(); }
inline Vec3 Normalize(const Vec3& v) { return v.Normalized(); }
inline Vec3 operator*(f32 s, const Vec3& v) { return v * s; }
inline f32 Fract(f32 x) { return x - std::floor(x); }
inline Vec3 Min(const Vec3& a, const Vec3& b) { return {std::fmin(a.x, b.x), std::fmin(a.y, b.y), std::fmin(a.z, b.z)}; }
inline Vec3 Max(const Vec3& a, const Vec3& b) { return {std::fmax(a.x, b.x), std::fmax(a.y, b.y), std::fmax(a.z, b.z)}; }

} // namespace aether::terrain
