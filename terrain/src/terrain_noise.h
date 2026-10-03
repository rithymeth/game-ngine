#pragma once

#include "aether/core/base.h"

#include <cmath>

// Value noise for the terrain and foliage generators. The lattice hash is
// unsigned arithmetic (signed overflow is undefined behaviour).
namespace aether::terrain {

inline u32 LatticeHash(i32 x, i32 z, u32 seed) {
    u32 h = static_cast<u32>(x) * 0x8DA6B343u ^ static_cast<u32>(z) * 0xD8163841u ^ seed * 0xCB1AB31Fu;
    h ^= h >> 13;
    h *= 0x5BD1E995u;
    h ^= h >> 15;
    return h;
}

// -1..1 at each lattice point.
inline f32 LatticeValue(i32 x, i32 z, u32 seed) { return static_cast<f32>(LatticeHash(x, z, seed) & 0xFFFFFFu) / 8388607.5f - 1.0f; }

// Smoothly interpolated between lattice points: -1..1.
inline f32 ValueNoise(f32 x, f32 z, u32 seed) {
    const f32 fx0 = std::floor(x), fz0 = std::floor(z);
    const i32 ix = static_cast<i32>(fx0), iz = static_cast<i32>(fz0);
    const f32 fx = x - fx0, fz = z - fz0;
    const f32 sx = fx * fx * (3.0f - 2.0f * fx), sz = fz * fz * (3.0f - 2.0f * fz);
    const f32 n00 = LatticeValue(ix, iz, seed), n10 = LatticeValue(ix + 1, iz, seed);
    const f32 n01 = LatticeValue(ix, iz + 1, seed), n11 = LatticeValue(ix + 1, iz + 1, seed);
    const f32 a = n00 + (n10 - n00) * sx, b = n01 + (n11 - n01) * sx;
    return a + (b - a) * sz;
}

// Octaves of value noise, each twice the frequency and `persistence` times the amplitude: -1..1.
inline f32 FractalNoise(f32 x, f32 z, u32 octaves, f32 persistence, u32 seed) {
    f32 sum = 0.0f, amp = 1.0f, freq = 1.0f, norm = 0.0f;
    for (u32 o = 0; o < std::max(octaves, 1u); ++o) {
        sum += amp * ValueNoise(x * freq, z * freq, seed + o * 0x9E3779B9u);
        norm += amp;
        amp *= persistence;
        freq *= 2.0f;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

} // namespace aether::terrain
