#pragma once

// @stability: experimental
//
// Large-world support (Phase 42.1). The engine renders and simulates in f32, so a
// position far from the origin loses precision (jitter, z-fighting, physics
// drift). A WorldOrigin is a double-precision anchor kept near the player: the
// caller tracks absolute positions in Vec3d and converts them to origin-relative
// Vec3 for Transform, rendering, physics and audio. Transform and the scene
// format keep their f32 values, so nothing is format-breaking until a system
// opts in.

#include "aether/math/vec.h"

namespace aether {

struct WorldOrigin {
    Vec3d absolute; // where the origin is, in absolute world metres

    // The origin-relative (f32) position of an absolute one.
    Vec3 ToRelative(const Vec3d& world) const { return (world - absolute).ToVec3(); }
    // The absolute position of an origin-relative one.
    Vec3d ToAbsolute(const Vec3& relative) const { return absolute + Vec3d::FromVec3(relative); }
    // How far `world` is from the origin, in metres.
    f64 DistanceTo(const Vec3d& world) const { return (world - absolute).Length(); }
};

// Moves the origin to `world` when it is farther than `threshold` metres from the
// current one; true when it moved. Systems re-derive their relative positions
// after a rebase.
inline bool RebaseOrigin(WorldOrigin& origin, const Vec3d& world, f64 threshold) {
    if (origin.DistanceTo(world) <= threshold) return false;
    origin.absolute = world;
    return true;
}

} // namespace aether
