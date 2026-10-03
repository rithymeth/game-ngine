#pragma once

#include "aether/terrain/terrain.h"

namespace aether {
namespace terrain {

// Phase 21 step 7: heightfield collider. Pure queries over a Heightmap, so it
// runs headless and can back a physics shape or gameplay ground checks.
// Heights are multiplied by `vertical_scale` to match the rendered mesh.

struct HeightfieldHit {
    bool hit = false;
    f32 distance = 0.0f; // along the ray direction
    Vec3 point{0, 0, 0};
    Vec3 normal{0, 1, 0};
};

// True when (x, z) lies inside the heightmap's world footprint.
bool HeightfieldContains(const Heightmap& hm, f32 x, f32 z);

// Surface height at (x, z); positions outside the footprint clamp to the edge.
f32 HeightfieldHeightAt(const Heightmap& hm, f32 vertical_scale, f32 x, f32 z);

// Unit surface normal at (x, z) from central differences.
Vec3 HeightfieldNormalAt(const Heightmap& hm, f32 vertical_scale, f32 x, f32 z);

// Ray cast against the surface. `direction` need not be normalized. A ray that
// starts below the surface reports no hit (the terrain is one-sided).
HeightfieldHit HeightfieldRaycast(const Heightmap& hm, f32 vertical_scale,
                                  const Vec3& origin, const Vec3& direction,
                                  f32 max_distance);

// Depth a sphere sinks below the surface, measured along the surface normal;
// zero when it is clear of the terrain or outside the footprint.
f32 HeightfieldSpherePenetration(const Heightmap& hm, f32 vertical_scale,
                                 const Vec3& center, f32 radius);

} // namespace terrain
} // namespace aether
