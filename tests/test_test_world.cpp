#include "test_framework.h"

#include "aether/scene/components.h"
#include "aether/scene/serialization.h"
#include "aether/scene/test_world.h"

// The generated test world (Phase 42 step 3): deterministic, sized by its parameters, and savable.

using namespace aether;
using namespace aether::scene;

namespace {

std::vector<f32> Positions(World& world) {
    std::vector<f32> out;
    world.ForEach<Transform>([&](Transform& t) {
        out.push_back(t.position.x);
        out.push_back(t.position.y);
        out.push_back(t.position.z);
    });
    return out;
}

} // namespace

AETHER_TEST(TestWorld_SameSeedGivesTheSameWorldAndEntityCountsFollowTheParameters) {
    TestWorldParams params;
    params.blocks_x = 5;
    params.blocks_z = 4;
    params.props_per_block = 3;
    World a, b;
    TestWorldStats stats;
    AETHER_CHECK(GenerateTestWorld(a, params, &stats));
    AETHER_CHECK(GenerateTestWorld(b, params));
    AETHER_CHECK(stats.entities == 5 * 4 * (1 + 3));
    AETHER_CHECK(a.EntityCount() == stats.entities);
    AETHER_CHECK(Positions(a) == Positions(b));

    params.seed = 2;
    World c;
    AETHER_CHECK(GenerateTestWorld(c, params));
    AETHER_CHECK(Positions(a) != Positions(c));
}

AETHER_TEST(TestWorld_RejectsBadParametersWithoutTouchingTheWorld) {
    World world;
    std::string error;
    TestWorldParams params;
    params.blocks_x = 0;
    AETHER_CHECK(!GenerateTestWorld(world, params, nullptr, &error));
    AETHER_CHECK(!error.empty());
    params = TestWorldParams{};
    params.props_per_block = 1000;
    AETHER_CHECK(!GenerateTestWorld(world, params));
    params = TestWorldParams{};
    params.model = std::string(300, 'x');
    AETHER_CHECK(!GenerateTestWorld(world, params));
    AETHER_CHECK(world.EntityCount() == 0);
}

AETHER_TEST(TestWorld_SavesAndReloads) {
    TestWorldParams params;
    params.blocks_x = 3;
    params.blocks_z = 3;
    World world;
    AETHER_CHECK(GenerateTestWorld(world, params));
    const std::vector<u8> bytes = SaveSceneToMemory(world);
    World loaded;
    AETHER_CHECK(LoadSceneFromMemory(loaded, bytes));
    AETHER_CHECK(loaded.EntityCount() == world.EntityCount());
}
