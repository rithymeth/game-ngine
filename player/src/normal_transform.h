#pragma once

#include "aether/math/mat4.h"

#include <cmath>

namespace aether::player {

// The matrix that carries a model's normals to world space: the inverse transpose of its linear part, with no translation.
//
// This used to invert the whole 4x4 with Mat4::TryInverse (pivoted Gauss-Jordan in double, about 2,500 instructions) for every
// node of every entity every frame, and take the transpose of the result's upper 3x3. A model transform is affine, so the
// linear part's inverse transpose comes straight from its cofactors: the columns of the result are c1 x c2, c2 x c0 and c0 x c1
// over the determinant, which is a few dozen operations. In double, like the old path, then narrowed to float.
//
// A degenerate transform (a zero scale on an axis, a non-finite value) gives the identity, as the old path did. A matrix that
// is not affine (nothing in the player makes one) takes the general path.
inline Mat4 BuildNormalTransform(const Mat4& model) {
    const bool affine = model.cols[0].w == 0.0f && model.cols[1].w == 0.0f && model.cols[2].w == 0.0f && model.cols[3].w == 1.0f;
    if (!affine) {
        Mat4 inverse;
        if (!model.TryInverse(inverse)) return Mat4::Identity();
        Mat4 normal;
        normal.cols[0] = Vec4(inverse.cols[0].x, inverse.cols[1].x, inverse.cols[2].x, 0.0f);
        normal.cols[1] = Vec4(inverse.cols[0].y, inverse.cols[1].y, inverse.cols[2].y, 0.0f);
        normal.cols[2] = Vec4(inverse.cols[0].z, inverse.cols[1].z, inverse.cols[2].z, 0.0f);
        normal.cols[3] = Vec4(0.0f, 0.0f, 0.0f, 1.0f);
        return normal;
    }
    const f64 ax = model.cols[0].x, ay = model.cols[0].y, az = model.cols[0].z;
    const f64 bx = model.cols[1].x, by = model.cols[1].y, bz = model.cols[1].z;
    const f64 cx = model.cols[2].x, cy = model.cols[2].y, cz = model.cols[2].z;
    // b x c, c x a and a x b
    const f64 r0x = by * cz - bz * cy, r0y = bz * cx - bx * cz, r0z = bx * cy - by * cx;
    const f64 r1x = cy * az - cz * ay, r1y = cz * ax - cx * az, r1z = cx * ay - cy * ax;
    const f64 r2x = ay * bz - az * by, r2y = az * bx - ax * bz, r2z = ax * by - ay * bx;
    const f64 det = ax * r0x + ay * r0y + az * r0z;
    // Singular as TryInverse judges it: its pivots (about the scales) at or below 1e-12, so a determinant at or below 1e-36.
    if (!std::isfinite(det) || std::fabs(det) <= 1e-36) return Mat4::Identity();
    const f64 inv = 1.0 / det;
    Mat4 normal;
    normal.cols[0] = Vec4(static_cast<f32>(r0x * inv), static_cast<f32>(r0y * inv), static_cast<f32>(r0z * inv), 0.0f);
    normal.cols[1] = Vec4(static_cast<f32>(r1x * inv), static_cast<f32>(r1y * inv), static_cast<f32>(r1z * inv), 0.0f);
    normal.cols[2] = Vec4(static_cast<f32>(r2x * inv), static_cast<f32>(r2y * inv), static_cast<f32>(r2z * inv), 0.0f);
    normal.cols[3] = Vec4(0.0f, 0.0f, 0.0f, 1.0f);
    return normal;
}

} // namespace aether::player
