#pragma once

#include "aether/core/base.h"
#include "aether/math/math.h"

// Per-instance colour variation for foliage: a field of grass, a forest of one tree kind, is hundreds of copies of a few meshes, and copies of one colour read as a
// pattern. Each instance of a material with wind (the plants) takes a small, steady tint from where it stands (the same place is the same tint, every frame and
// every run, so nothing flickers when the camera moves): a value change of about +-12% and a slight shift between warm and cool.
namespace aether::player {

// The multiplier for an instance's base colour. Each channel is within 0.78 and 1.22 and the average over many places is about 1.
Vec3 InstanceTint(const Vec3& world_position);

} // namespace aether::player
