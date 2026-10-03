#include "aether/terrain/foliage.h"
#include "test_framework.h"

#include <cmath>

using namespace aether;
using namespace aether::terrain;

namespace {
bool NearEqual(f32 a, f32 b, f32 tol) { return std::fabs(a - b) <= tol; }
bool NearEqual(const Vec3& a, const Vec3& b, f32 tol) { return (a - b).Length() <= tol; }
} // namespace

AETHER_TEST(FoliageDensityMapGeneration) {
    auto dm = GenerateDensityMap(64, 64, 64.0f, 4, 0.5f, 123.0f);
    AETHER_CHECK(dm.width == 64);
    AETHER_CHECK(dm.height == 64);
    AETHER_CHECK(dm.cells.size() == 64u * 64u);

    // All values should be 0..255.
    for (u8 v : dm.cells) {
        AETHER_CHECK(v <= 255);
    }

    // Should have some variation (not all the same).
    bool all_same = true;
    for (u8 v : dm.cells) {
        if (v != dm.cells[0]) { all_same = false; break; }
    }
    AETHER_CHECK(!all_same);
}

AETHER_TEST(FoliageAddRemoveInstance) {
    FoliageLayer layer;
    layer.types.push_back(FoliageType{});
    layer.max_instances = 100;
    std::mt19937 rng(42);

    AETHER_CHECK(layer.instances.empty());

    AddInstance(layer, Vec3(0, 0, 0), Vec3(0, 1, 0), rng);
    AETHER_CHECK(layer.instances.size() == 1);
    AETHER_CHECK(NearEqual(layer.instances[0].position, Vec3(0, 0, 0), 1e-4f));

    // Remove near center.
    RemoveInstancesNear(layer, Vec3(0, 0, 0), 1.0f);
    AETHER_CHECK(layer.instances.empty());
}

AETHER_TEST(FoliageFilterByChunk) {
    std::vector<FoliageInstance> insts(3);
    insts[0].chunk_x = 1; insts[0].chunk_z = 2;
    insts[1].chunk_x = 1; insts[1].chunk_z = 2;
    insts[2].chunk_x = 0; insts[2].chunk_z = 1;

    auto filtered = FilterByChunk(insts, 1, 2);
    AETHER_CHECK(filtered.size() == 2);
}

AETHER_TEST(FoliageSortByType) {
    std::vector<FoliageInstance> insts(5);
    insts[0].type_index = 2;
    insts[1].type_index = 0;
    insts[2].type_index = 1;
    insts[3].type_index = 0;
    insts[4].type_index = 2;

    auto order = SortInstancesByType(insts);
    // 1,3,2,0,4 in index order (types 0,0,1,2,2)
    AETHER_CHECK(insts[order[0]].type_index == 0);
    // Let's just check it's sorted.
    for (usize i = 1; i < order.size(); ++i) {
        AETHER_CHECK(insts[order[i-1]].type_index <= insts[order[i]].type_index);
    }
}

AETHER_TEST(FoliageBounds) {
    std::vector<FoliageInstance> insts(3);
    insts[0].position = Vec3(0, 0, 0);
    insts[1].position = Vec3(10, 5, 7);
    insts[2].position = Vec3(-3, 2, -1);

    Vec3 mn{0, 0, 0}, mx{0, 0, 0};
    ComputeBounds(insts, &mn, &mx);

    AETHER_CHECK(mn.x == -3 && mn.y == 0 && mn.z == -1);
    AETHER_CHECK(mx.x == 10 && mx.y == 5 && mx.z == 7);
}

AETHER_TEST(FoliageRandomScaleIsInRange) {
    FoliageType type;
    type.scale_min = 0.5f;
    type.scale_max = 2.0f;
    std::mt19937 rng(42);

    for (int i = 0; i < 100; ++i) {
        Vec3 s = RandomScale(rng, type);
        AETHER_CHECK(s.x >= 0.5f && s.x <= 2.0f);
        AETHER_CHECK(s.y >= 0.5f && s.y <= 2.0f);
        AETHER_CHECK(s.z >= 0.5f && s.z <= 2.0f);
        // Uniform scale.
        AETHER_CHECK(NearEqual(s.x, s.y, 1e-5f));
        AETHER_CHECK(NearEqual(s.y, s.z, 1e-5f));
    }
}

AETHER_TEST(FoliageRandomRotationIsYOnly) {
    FoliageType type;
    type.rotation_range = 3.14159f; // full 360
    std::mt19937 rng(99);

    for (int i = 0; i < 50; ++i) {
        Quaternion q = RandomRotation(rng, type);
        // Only Y rotation: x,z ~ 0, w in [-1,1]
        AETHER_CHECK(std::abs(q.x) < 1e-3f);
        AETHER_CHECK(std::abs(q.z) < 1e-3f);
        // Should be normalized.
        AETHER_CHECK(NearEqual(q.Length(), 1.0f, 1e-4f));
    }
}
