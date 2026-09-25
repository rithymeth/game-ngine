#pragma once

#include "aether/math/mat4.h"
#include "aether/math/vec.h"

namespace aether {

struct AETHER_ALIGN(16) Quaternion {
    union {
        __m128 simd;
        struct { f32 x, y, z, w; };
    };

    Quaternion() : simd(_mm_set_ps(1, 0, 0, 0)) {}
    Quaternion(f32 x_, f32 y_, f32 z_, f32 w_) : simd(_mm_set_ps(w_, z_, y_, x_)) {}
    explicit Quaternion(__m128 v) : simd(v) {}

    static Quaternion Identity() { return Quaternion(); }

    static Quaternion FromAxisAngle(const Vec3& axis, f32 radians) {
        f32 half = radians * 0.5f;
        f32 s = std::sin(half);
        Vec3 n = axis.Normalized();
        return Quaternion(n.x * s, n.y * s, n.z * s, std::cos(half));
    }

    Quaternion operator*(const Quaternion& o) const {
        return Quaternion(
            w * o.x + x * o.w + y * o.z - z * o.y,
            w * o.y - x * o.z + y * o.w + z * o.x,
            w * o.z + x * o.y - y * o.x + z * o.w,
            w * o.w - x * o.x - y * o.y - z * o.z);
    }

    f32 LengthSq() const { return x * x + y * y + z * z + w * w; }
    f32 Length() const { return std::sqrt(LengthSq()); }

    Quaternion Normalized() const {
        f32 len = Length();
        if (len <= 0.0f) {
            return Identity();
        }
        f32 inv = 1.0f / len;
        return Quaternion(x * inv, y * inv, z * inv, w * inv);
    }

    Mat4 ToMat4() const {
        Mat4 m;
        f32 xx = x * x, yy = y * y, zz = z * z;
        f32 xy = x * y, xz = x * z, yz = y * z;
        f32 wx = w * x, wy = w * y, wz = w * z;

        m.cols[0] = Vec4(1 - 2 * (yy + zz), 2 * (xy + wz), 2 * (xz - wy), 0);
        m.cols[1] = Vec4(2 * (xy - wz), 1 - 2 * (xx + zz), 2 * (yz + wx), 0);
        m.cols[2] = Vec4(2 * (xz + wy), 2 * (yz - wx), 1 - 2 * (xx + yy), 0);
        m.cols[3] = Vec4(0, 0, 0, 1);
        return m;
    }
};

} // namespace aether
