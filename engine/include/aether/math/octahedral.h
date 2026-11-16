#pragma once

// Unit-vector compression. Two algorithms, same bit budget, same decode:
//
// Direct octahedral (Cigolle, Donow, Evangelakos, Mara, McGuire, Meyer,
// "A Survey of Efficient Representations for Independent Unit Vectors").
// Project onto the octahedron, fold the lower half into the square, store
// the result as two snorms. 8-bit is about a degree. 16-bit is a few
// hundredths. Spherical coordinates waste bits at the poles, and storing
// XY then reconstructing Z only covers a hemisphere cleanly.
//
// Best-fit uses that same lattice but tries the neighbouring codes and keeps
// the one whose decode is closest to the original. Same size as direct, lower
// worst-case error. This is an encoder choice. The glTF OCTAHEDRAL filter is
// the same projection stored for meshopt, not this float codec.

#include "aether/math/vec.h"

#include <algorithm>
#include <cmath>

namespace aether {

struct OctCode {
    i16 u = 0;
    i16 v = 0;
};

inline int OctSnormMax(int bits) { return (1 << (bits - 1)) - 1; }

inline int OctQuantize(f32 v, int bits) {
    const int max = OctSnormMax(bits);
    const f32 clamped = std::clamp(v, -1.0f, 1.0f);
    const int q = static_cast<int>(std::round(clamped * static_cast<f32>(max)));
    return std::clamp(q, -max, max);
}

inline f32 OctDequantize(int q, int bits) {
    const int max = OctSnormMax(bits);
    return std::max(-1.0f, static_cast<f32>(q) / static_cast<f32>(max));
}

inline f32 OctSign(f32 v) { return v >= 0.0f ? 1.0f : -1.0f; }

// bits is 2..16. The vector does not need to be unit length.
inline OctCode OctEncodeDirect(Vec3 n, int bits) {
    n = n.Normalized();
    const f32 sum = std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z);
    f32 u = 0.0f;
    f32 v = 0.0f;
    if (sum > 0.0f) {
        u = n.x / sum;
        v = n.y / sum;
        if (n.z < 0.0f) {
            const f32 ou = u;
            const f32 ov = v;
            u = (1.0f - std::fabs(ov)) * OctSign(ou);
            v = (1.0f - std::fabs(ou)) * OctSign(ov);
        }
    }
    return {static_cast<i16>(OctQuantize(u, bits)), static_cast<i16>(OctQuantize(v, bits))};
}

inline Vec3 OctDecode(OctCode code, int bits) {
    const f32 u = OctDequantize(code.u, bits);
    const f32 v = OctDequantize(code.v, bits);
    Vec3 n{u, v, 1.0f - std::fabs(u) - std::fabs(v)};
    if (n.z < 0.0f) {
        const f32 ou = n.x;
        const f32 ov = n.y;
        n.x = (1.0f - std::fabs(ov)) * OctSign(ou);
        n.y = (1.0f - std::fabs(ou)) * OctSign(ov);
    }
    return n.Normalized();
}

// Neighbourhood search around the direct code. Radius 1 is the 3x3 that
// matters at 8 bits; wider searches rarely pay for the extra encodes.
inline OctCode OctEncodeBestFit(Vec3 n, int bits) {
    n = n.Normalized();
    const OctCode direct = OctEncodeDirect(n, bits);
    const int max = OctSnormMax(bits);
    OctCode best = direct;
    f32 best_dot = -2.0f;
    for (int du = -1; du <= 1; ++du) {
        for (int dv = -1; dv <= 1; ++dv) {
            const int u = std::clamp(static_cast<int>(direct.u) + du, -max, max);
            const int v = std::clamp(static_cast<int>(direct.v) + dv, -max, max);
            const OctCode candidate{static_cast<i16>(u), static_cast<i16>(v)};
            const f32 d = n.Dot(OctDecode(candidate, bits));
            if (d > best_dot) {
                best_dot = d;
                best = candidate;
            }
        }
    }
    return best;
}

}  // namespace aether
