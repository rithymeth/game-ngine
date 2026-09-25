#include "aether/scene/serialization.h"
#include "test_framework.h"

#include <filesystem>
#include <string>

using namespace aether;

namespace {

struct Position { f32 x = 0, y = 0, z = 0; };
struct Health { i32 value = 100; };

} // namespace

AETHER_TEST(Serialization_RoundTripsComponentsAcrossWorlds) {
    std::string path = (std::filesystem::temp_directory_path() / "aether_test_scene.aesc").string();

    World saved_world;
    saved_world.CreateEntity(Position{1, 2, 3}, Health{50});
    saved_world.CreateEntity(Position{4, 5, 6});
    saved_world.CreateEntity(Health{7});

    AETHER_CHECK(SaveScene(saved_world, path));

    World loaded_world;
    AETHER_CHECK(LoadScene(loaded_world, path));
    AETHER_CHECK(loaded_world.EntityCount() == 3);

    int with_both = 0;
    loaded_world.ForEach<Position, Health>([&](Position& p, Health& h) {
        ++with_both;
        AETHER_CHECK_NEAR(p.x, 1.0f, 1e-6);
        AETHER_CHECK_NEAR(p.y, 2.0f, 1e-6);
        AETHER_CHECK_NEAR(p.z, 3.0f, 1e-6);
        AETHER_CHECK(h.value == 50);
    });
    AETHER_CHECK(with_both == 1);

    int position_count = 0;
    loaded_world.ForEach<Position>([&](Position&) { ++position_count; });
    AETHER_CHECK(position_count == 2); // the {Position,Health} entity + the Position-only entity

    int health_count = 0;
    i32 lone_health_value = 0;
    loaded_world.ForEach<Health>([&](Health& h) {
        ++health_count;
        if (h.value != 50) {
            lone_health_value = h.value;
        }
    });
    AETHER_CHECK(health_count == 2); // the {Position,Health} entity + the Health-only entity
    AETHER_CHECK(lone_health_value == 7);

    std::filesystem::remove(path);
}

AETHER_TEST(Serialization_RejectsMissingFile) {
    World world;
    AETHER_CHECK(!LoadScene(world, "this_scene_file_should_not_exist_98765.aesc"));
}
