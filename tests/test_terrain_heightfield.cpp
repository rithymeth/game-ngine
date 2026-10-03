#include "aether/terrain/heightfield.h"
#include "test_framework.h"

using namespace aether;
using namespace aether::terrain;

namespace {

Heightmap Flat(u32 n, f32 h) {
    Heightmap hm;
    hm.width = n;
    hm.height = n;
    hm.cell_size = 1.0f;
    hm.heights.assign(static_cast<usize>(n) * n, h);
    return hm;
}

// Height rises 0.5 per cell along +X.
Heightmap Ramp(u32 n) {
    Heightmap hm = Flat(n, 0.0f);
    for (u32 z = 0; z < n; ++z)
        for (u32 x = 0; x < n; ++x) hm.heights[static_cast<usize>(z) * n + x] = 0.5f * static_cast<f32>(x);
    return hm;
}

} // namespace

AETHER_TEST(Heightfield_HeightAndContains) {
    Heightmap hm = Ramp(17);
    AETHER_CHECK(HeightfieldContains(hm, 0.0f, 0.0f));
    AETHER_CHECK(HeightfieldContains(hm, 16.0f, 16.0f));
    AETHER_CHECK(!HeightfieldContains(hm, -0.1f, 5.0f));
    AETHER_CHECK(!HeightfieldContains(hm, 5.0f, 16.5f));
    AETHER_CHECK(!HeightfieldContains(Heightmap{}, 0.0f, 0.0f));

    AETHER_CHECK_NEAR(HeightfieldHeightAt(hm, 1.0f, 4.0f, 3.0f), 2.0f, 1e-4f);
    AETHER_CHECK_NEAR(HeightfieldHeightAt(hm, 2.0f, 4.0f, 3.0f), 4.0f, 1e-4f); // vertical scale
    AETHER_CHECK_NEAR(HeightfieldHeightAt(hm, 1.0f, 4.5f, 3.0f), 2.25f, 1e-4f); // bilinear
    AETHER_CHECK_NEAR(HeightfieldHeightAt(hm, 1.0f, 99.0f, 3.0f), 8.0f, 1e-4f); // clamps to the edge
}

AETHER_TEST(Heightfield_Normal) {
    Heightmap flat = Flat(9, 2.0f);
    const Vec3 up = HeightfieldNormalAt(flat, 1.0f, 4.0f, 4.0f);
    AETHER_CHECK_NEAR(up.y, 1.0f, 1e-5f);

    Heightmap ramp = Ramp(17);
    const Vec3 n = HeightfieldNormalAt(ramp, 1.0f, 8.0f, 8.0f);
    AETHER_CHECK_NEAR(n.Length(), 1.0f, 1e-4f);
    AETHER_CHECK(n.x < 0.0f && n.y > 0.0f);          // tilts away from the rise
    AETHER_CHECK_NEAR(n.x / n.y, -0.5f, 1e-3f);       // slope 0.5
    AETHER_CHECK_NEAR(n.z, 0.0f, 1e-4f);
}

AETHER_TEST(Heightfield_RaycastDownHitsSurface) {
    Heightmap hm = Ramp(17);
    HeightfieldHit hit = HeightfieldRaycast(hm, 1.0f, Vec3(4.0f, 10.0f, 5.0f), Vec3(0, -1, 0), 50.0f);
    AETHER_CHECK(hit.hit);
    AETHER_CHECK_NEAR(hit.point.y, 2.0f, 1e-3f);
    AETHER_CHECK_NEAR(hit.distance, 8.0f, 1e-3f);
    AETHER_CHECK(hit.normal.y > 0.0f);

    // Unnormalized direction gives the same result.
    HeightfieldHit scaled = HeightfieldRaycast(hm, 1.0f, Vec3(4.0f, 10.0f, 5.0f), Vec3(0, -5, 0), 50.0f);
    AETHER_CHECK(scaled.hit && std::fabs(scaled.distance - hit.distance) < 1e-3f);
}

AETHER_TEST(Heightfield_RaycastMissesAndLimits) {
    Heightmap hm = Flat(17, 0.0f);
    // Too short to reach the surface.
    AETHER_CHECK(!HeightfieldRaycast(hm, 1.0f, Vec3(4, 10, 4), Vec3(0, -1, 0), 5.0f).hit);
    // Pointing away.
    AETHER_CHECK(!HeightfieldRaycast(hm, 1.0f, Vec3(4, 10, 4), Vec3(0, 1, 0), 50.0f).hit);
    // Starting below the surface.
    AETHER_CHECK(!HeightfieldRaycast(hm, 1.0f, Vec3(4, -1, 4), Vec3(0, -1, 0), 50.0f).hit);
    // Degenerate input.
    AETHER_CHECK(!HeightfieldRaycast(hm, 1.0f, Vec3(4, 10, 4), Vec3(0, 0, 0), 50.0f).hit);
    AETHER_CHECK(!HeightfieldRaycast(Heightmap{}, 1.0f, Vec3(4, 10, 4), Vec3(0, -1, 0), 50.0f).hit);
    // Off the footprint entirely.
    AETHER_CHECK(!HeightfieldRaycast(hm, 1.0f, Vec3(100, 10, 4), Vec3(0, -1, 0), 50.0f).hit);
}

AETHER_TEST(Heightfield_RaycastEntersFromTheSide) {
    Heightmap hm = Ramp(17); // rises to 8 at x = 16
    // Fired horizontally from outside along +X at y = 4: meets the ramp at x = 8.
    HeightfieldHit hit = HeightfieldRaycast(hm, 1.0f, Vec3(-5.0f, 4.0f, 8.0f), Vec3(1, 0, 0), 50.0f);
    AETHER_CHECK(hit.hit);
    AETHER_CHECK_NEAR(hit.point.x, 8.0f, 1e-2f);
}

AETHER_TEST(Heightfield_SpherePenetration) {
    Heightmap hm = Flat(17, 0.0f);
    AETHER_CHECK_NEAR(HeightfieldSpherePenetration(hm, 1.0f, Vec3(8, 2.0f, 8), 1.0f), 0.0f, 1e-6f);
    AETHER_CHECK_NEAR(HeightfieldSpherePenetration(hm, 1.0f, Vec3(8, 0.25f, 8), 1.0f), 0.75f, 1e-4f);
    AETHER_CHECK_NEAR(HeightfieldSpherePenetration(hm, 1.0f, Vec3(8, -1.0f, 8), 1.0f), 2.0f, 1e-4f);
    // Outside the footprint there is no terrain to hit.
    AETHER_CHECK_NEAR(HeightfieldSpherePenetration(hm, 1.0f, Vec3(50, 0.0f, 8), 1.0f), 0.0f, 1e-6f);
}
