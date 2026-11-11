#include "instance_variation.h"

#include <cmath>

namespace aether::player {

namespace {
// A hash of a place to 0..1: the position quantised to a decimetre, so a plant that moves a hair keeps its colour.
f32 PlaceHash(i64 x, i64 y, i64 z, u32 salt) {
    u64 h = static_cast<u64>(x) * 0x9E3779B97F4A7C15ull ^ (static_cast<u64>(y) + salt) * 0xC2B2AE3D27D4EB4Full ^ static_cast<u64>(z) * 0x165667B19E3779F9ull;
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    return static_cast<f32>(h & 0xFFFFFFu) / 16777216.0f;
}
} // namespace

Vec3 InstanceTint(const Vec3& p) {
    const i64 x = static_cast<i64>(std::floor(p.x * 10.0f)), y = static_cast<i64>(std::floor(p.y * 10.0f)), z = static_cast<i64>(std::floor(p.z * 10.0f));
    const f32 value = 1.0f + 0.24f * (PlaceHash(x, y, z, 11u) - 0.5f); // +-12%
    const f32 warm = 0.20f * (PlaceHash(x, y, z, 23u) - 0.5f);         // +-10% between red and blue
    return Vec3(value * (1.0f + warm * 0.5f), value, value * (1.0f - warm * 0.5f)) * 1.0f;
}

} // namespace aether::player
