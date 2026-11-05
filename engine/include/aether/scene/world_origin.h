#pragma once

// @stability: experimental
//
// Phase 42.1: rebasing a loaded world onto a WorldOrigin. When the origin moves
// by `delta`, every root entity's f32 Transform position moves by `-delta`, so
// the world stays where it is in absolute space and render-facing systems keep
// seeing the same small coordinates.

#include "aether/ecs/world.h"
#include "aether/math/world_origin.h"
#include "aether/scene/entity_guid.h"

namespace aether {

// Shifts every root Transform position by `-delta` (the origin moved by `delta`).
// Children are left alone: their Transform is relative to their parent. With
// `guids`, a Parent that no longer resolves counts as a root, matching what the
// streamer's floating origin does; without it, an entity with a Parent is taken
// to be a child. Returns how many transforms moved.
usize RebaseTransforms(World& world, const Vec3d& delta, const GuidIndex* guids = nullptr);

// Shifts every root Transform position by `+delta` (the inverse of RebaseTransforms).
usize ShiftTransforms(World& world, const Vec3d& delta, const GuidIndex* guids = nullptr);

} // namespace aether
