#pragma once

// @stability: experimental
//
// Phase 42.1: rebasing a loaded world onto a WorldOrigin. When the origin moves
// by `delta`, every entity's f32 Transform position moves by `-delta`, so the
// world stays where it is in absolute space and render-facing systems keep
// seeing the same small coordinates they had before.

#include "aether/ecs/world.h"
#include "aether/math/world_origin.h"

namespace aether {

// Shifts every Transform position by `-delta` (the origin moved by `delta`).
// Returns how many transforms moved.
usize RebaseTransforms(World& world, const Vec3d& delta);

// Shifts every Transform position by `+delta` (the inverse of RebaseTransforms).
usize ShiftTransforms(World& world, const Vec3d& delta);

} // namespace aether
