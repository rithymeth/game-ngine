#pragma once

#include "aether/math/mat4.h"
#include "aether/math/quaternion.h"
#include "aether/math/vec.h"

namespace aether {

inline constexpr f32 kPi = 3.14159265358979323846f;

inline constexpr f32 Radians(f32 degrees) { return degrees * (kPi / 180.0f); }
inline constexpr f32 Degrees(f32 radians) { return radians * (180.0f / kPi); }

} // namespace aether
