#include "aether/terrain/world.h"
#include "test_framework.h"

#include <algorithm>

using namespace aether;
using namespace aether::terrain;

namespace {

bool Has(const std::vector<CellCoord>& v, i32 x, i32 z) {
    return std::find(v.begin(), v.end(), CellCoord{x, z}) != v.end();
}

WorldPartitionSettings Small() {
    WorldPartitionSettings s;
    s.cell_size = 100.0f;
    s.load_radius = 120.0f;
    s.unload_radius = 250.0f;
    s.max_loads_per_update = 0;
    return s;
}

} // namespace

AETHER_TEST(WorldPartition_CellOfHandlesNegatives) {
    WorldPartition wp(Small());
    AETHER_CHECK(wp.CellOf(0.0, 0.0) == CellCoord({0, 0}));
    AETHER_CHECK(wp.CellOf(99.9, 100.0) == CellCoord({0, 1}));
    AETHER_CHECK(wp.CellOf(-0.1, -100.1) == CellCoord({-1, -2}));

    f64 x0, z0, x1, z1;
    wp.CellBounds({-1, 2}, x0, z0, x1, z1);
    AETHER_CHECK(x0 == -100.0 && z0 == 200.0 && x1 == 0.0 && z1 == 300.0);
}

AETHER_TEST(WorldPartition_LoadsCellsWithinRadiusNearestFirst) {
    WorldPartition wp(Small());
    // Focus in the middle of cell (0,0): the 3x3 block is within 120 units.
    StreamingDelta d = wp.Update(50.0, 50.0);
    AETHER_CHECK(d.to_unload.empty());
    AETHER_CHECK(d.to_load.size() == 9);
    AETHER_CHECK(d.to_load[0] == CellCoord({0, 0}));
    AETHER_CHECK(Has(d.to_load, -1, -1) && Has(d.to_load, 1, 1));
    AETHER_CHECK(!Has(d.to_load, 2, 0)); // 150 units away: outside the radius
    AETHER_CHECK(wp.LoadedCount() == 9 && wp.IsLoaded({1, 0}));

    // Nothing changes while the focus stays put.
    StreamingDelta again = wp.Update(50.0, 50.0);
    AETHER_CHECK(again.to_load.empty() && again.to_unload.empty());
}

AETHER_TEST(WorldPartition_UnloadHasHysteresis) {
    WorldPartition wp(Small());
    wp.Update(50.0, 50.0);

    // Move 120 units east: cell (-1,*) is now 170 away -- past load but inside unload.
    StreamingDelta d = wp.Update(170.0, 50.0);
    AETHER_CHECK(d.to_unload.empty());
    AETHER_CHECK(wp.IsLoaded({-1, 0}));
    AETHER_CHECK(Has(d.to_load, 2, 0)); // newly in range

    // Move far enough that column -1 passes the unload radius (250).
    StreamingDelta far = wp.Update(360.0, 50.0);
    AETHER_CHECK(Has(far.to_unload, -1, 0) && !wp.IsLoaded({-1, 0}));
    AETHER_CHECK(Has(far.to_unload, 0, 0));
    // Farthest unloads first.
    AETHER_CHECK(far.to_unload.size() >= 2);
}

AETHER_TEST(WorldPartition_CapsLoadsPerUpdate) {
    WorldPartitionSettings s = Small();
    s.max_loads_per_update = 2;
    WorldPartition wp(s);

    StreamingDelta d1 = wp.Update(50.0, 50.0);
    AETHER_CHECK(d1.to_load.size() == 2 && d1.to_load[0] == CellCoord({0, 0}));
    StreamingDelta d2 = wp.Update(50.0, 50.0);
    AETHER_CHECK(d2.to_load.size() == 2);
    // Eventually everything in range loads, with no duplicates.
    for (int i = 0; i < 10; ++i) wp.Update(50.0, 50.0);
    AETHER_CHECK(wp.LoadedCount() == 9);
}

AETHER_TEST(WorldPartition_ResetAndInvalidSize) {
    WorldPartition wp(Small());
    wp.Update(0.0, 0.0);
    AETHER_CHECK(wp.LoadedCount() > 0);
    wp.Reset();
    AETHER_CHECK(wp.LoadedCount() == 0);

    WorldPartitionSettings bad;
    bad.cell_size = 0.0f;
    WorldPartition broken(bad);
    StreamingDelta d = broken.Update(0.0, 0.0);
    AETHER_CHECK(d.to_load.empty() && d.to_unload.empty());
}

AETHER_TEST(FloatingOrigin_RebaseKeepsWorldPositionStable) {
    FloatingOrigin fo(1000.0);
    Vec3 shift;

    // Inside the threshold: no rebase.
    AETHER_CHECK(!fo.Update({500.0, 0.0, 0.0}, shift));
    AETHER_CHECK(fo.Origin().x == 0.0);

    // A point far from the origin, held in double precision.
    const WorldPosition far_object{1.0e7 + 0.25, 3.0, -2.0e7 + 0.5};
    const WorldPosition focus{1.0e7, 0.0, -2.0e7};
    AETHER_CHECK(fo.Update(focus, shift));
    AETHER_CHECK(fo.Origin().x == focus.x && fo.Origin().z == focus.z);
    AETHER_CHECK_NEAR(shift.x, -1.0e7f, 1.0f);

    // Relative to the new origin the offset keeps full precision.
    const Vec3 local = fo.ToLocal(far_object);
    AETHER_CHECK_NEAR(local.x, 0.25f, 1e-6f);
    AETHER_CHECK_NEAR(local.y, 3.0f, 1e-6f);
    AETHER_CHECK_NEAR(local.z, 0.5f, 1e-6f);

    // Round trip back to world space.
    const WorldPosition back = fo.ToWorld(local);
    AETHER_CHECK(std::fabs(back.x - far_object.x) < 1e-6);
    AETHER_CHECK(std::fabs(back.z - far_object.z) < 1e-6);

    // Staying near the new origin does not rebase again.
    AETHER_CHECK(!fo.Update({focus.x + 10.0, 0.0, focus.z}, shift));
}
