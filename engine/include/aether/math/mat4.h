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

    // Inverts a general 4x4 matrix with pivoted Gauss-Jordan elimination.
    // Returns false for a singular or numerically near-singular matrix.
    bool TryInverse(Mat4& out, f64 epsilon = 1e-12) const {
        f64 augmented[4][8]{};
        for (usize row = 0; row < 4; ++row) {
            for (usize column = 0; column < 4; ++column) {
                const Vec4& source = cols[column];
                const f32 values[4] = {source.x, source.y, source.z, source.w};
                augmented[row][column] = values[row];
            }
            augmented[row][row + 4] = 1.0;
        }

        for (usize column = 0; column < 4; ++column) {
            usize pivot_row = column;
            for (usize row = column + 1; row < 4; ++row) {
                if (std::abs(augmented[row][column]) > std::abs(augmented[pivot_row][column])) {
                    pivot_row = row;
                }
            }
            if (std::abs(augmented[pivot_row][column]) <= epsilon) return false;
            if (pivot_row != column) {
                for (usize entry = 0; entry < 8; ++entry) {
                    const f64 value = augmented[pivot_row][entry];
                    augmented[pivot_row][entry] = augmented[column][entry];
                    augmented[column][entry] = value;
                }
            }

            const f64 pivot = augmented[column][column];
            for (usize entry = 0; entry < 8; ++entry) augmented[column][entry] /= pivot;
            for (usize row = 0; row < 4; ++row) {
                if (row == column) continue;
                const f64 factor = augmented[row][column];
                for (usize entry = 0; entry < 8; ++entry) {
                    augmented[row][entry] -= factor * augmented[column][entry];
                }
            }
        }

        out.cols[0] = Vec4(static_cast<f32>(augmented[0][4]), static_cast<f32>(augmented[1][4]),
                           static_cast<f32>(augmented[2][4]), static_cast<f32>(augmented[3][4]));
        out.cols[1] = Vec4(static_cast<f32>(augmented[0][5]), static_cast<f32>(augmented[1][5]),
                           static_cast<f32>(augmented[2][5]), static_cast<f32>(augmented[3][5]));
        out.cols[2] = Vec4(static_cast<f32>(augmented[0][6]), static_cast<f32>(augmented[1][6]),
                           static_cast<f32>(augmented[2][6]), static_cast<f32>(augmented[3][6]));
        out.cols[3] = Vec4(static_cast<f32>(augmented[0][7]), static_cast<f32>(augmented[1][7]),
                           static_cast<f32>(augmented[2][7]), static_cast<f32>(augmented[3][7]));
        return true;
    }
};

} // namespace aether
