#include "aether/math/math.h"

// Vec3/Vec4/Mat4/Quaternion are header-only (SIMD ops are all inline
// intrinsics); this translation unit exists so the math module has a stable
// place to grow non-inline code (e.g. SIMD dispatch, LUTs) without touching
// the public headers.
