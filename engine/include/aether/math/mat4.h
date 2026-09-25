#pragma once

#include "aether/math/vec.h"

namespace aether {

// Column-major 4x4 matrix, matching Vulkan/GLSL conventions. Stored as four
// SIMD columns so multiply/transform are pure SSE shuffles-and-fma.
struct AETHER_ALIGN(16) Mat4 {
    Vec4 cols[4];

    Mat4() {
        cols[0] = Vec4(1, 0, 0, 0);
        cols[1] = Vec4(0, 1, 0, 0);
        cols[2] = Vec4(0, 0, 1, 0);
        cols[3] = Vec4(0, 0, 0, 1);
    }

    static Mat4 Identity() { return Mat4(); }

    static Mat4 Translation(const Vec3& t) {
        Mat4 m;
        m.cols[3] = Vec4(t.x, t.y, t.z, 1.0f);
        return m;
    }

    static Mat4 Scale(const Vec3& s) {
        Mat4 m;
        m.cols[0] = Vec4(s.x, 0, 0, 0);
        m.cols[1] = Vec4(0, s.y, 0, 0);
        m.cols[2] = Vec4(0, 0, s.z, 0);
        return m;
    }

    // Right-handed perspective projection matching Vulkan clip space
    // (depth range [0, 1], Y pointing down after the standard Vulkan flip is
    // left to the caller / viewport setup).
    static Mat4 PerspectiveRH(f32 fovy_radians, f32 aspect, f32 z_near, f32 z_far) {
        f32 tan_half_fovy = std::tan(fovy_radians * 0.5f);
        Mat4 m;
        m.cols[0] = Vec4(1.0f / (aspect * tan_half_fovy), 0, 0, 0);
        m.cols[1] = Vec4(0, 1.0f / tan_half_fovy, 0, 0);
        m.cols[2] = Vec4(0, 0, z_far / (z_near - z_far), -1.0f);
        m.cols[3] = Vec4(0, 0, -(z_far * z_near) / (z_far - z_near), 0);
        return m;
    }

    static Mat4 LookAtRH(const Vec3& eye, const Vec3& target, const Vec3& up) {
        Vec3 f = (target - eye).Normalized();
        Vec3 s = f.Cross(up).Normalized();
        Vec3 u = s.Cross(f);

        Mat4 m;
        m.cols[0] = Vec4(s.x, u.x, -f.x, 0);
        m.cols[1] = Vec4(s.y, u.y, -f.y, 0);
        m.cols[2] = Vec4(s.z, u.z, -f.z, 0);
        m.cols[3] = Vec4(-s.Dot(eye), -u.Dot(eye), f.Dot(eye), 1.0f);
        return m;
    }

    Mat4 operator*(const Mat4& rhs) const {
        Mat4 result;
        for (int i = 0; i < 4; ++i) {
            __m128 col = rhs.cols[i].simd;

            __m128 r = _mm_mul_ps(cols[0].simd, _mm_shuffle_ps(col, col, _MM_SHUFFLE(0, 0, 0, 0)));
            r = _mm_add_ps(r, _mm_mul_ps(cols[1].simd, _mm_shuffle_ps(col, col, _MM_SHUFFLE(1, 1, 1, 1))));
            r = _mm_add_ps(r, _mm_mul_ps(cols[2].simd, _mm_shuffle_ps(col, col, _MM_SHUFFLE(2, 2, 2, 2))));
            r = _mm_add_ps(r, _mm_mul_ps(cols[3].simd, _mm_shuffle_ps(col, col, _MM_SHUFFLE(3, 3, 3, 3))));

            result.cols[i] = Vec4(r);
        }
        return result;
    }

    Vec4 operator*(const Vec4& v) const {
        __m128 r = _mm_mul_ps(cols[0].simd, _mm_shuffle_ps(v.simd, v.simd, _MM_SHUFFLE(0, 0, 0, 0)));
        r = _mm_add_ps(r, _mm_mul_ps(cols[1].simd, _mm_shuffle_ps(v.simd, v.simd, _MM_SHUFFLE(1, 1, 1, 1))));
        r = _mm_add_ps(r, _mm_mul_ps(cols[2].simd, _mm_shuffle_ps(v.simd, v.simd, _MM_SHUFFLE(2, 2, 2, 2))));
        r = _mm_add_ps(r, _mm_mul_ps(cols[3].simd, _mm_shuffle_ps(v.simd, v.simd, _MM_SHUFFLE(3, 3, 3, 3))));
        return Vec4(r);
    }
};

} // namespace aether
