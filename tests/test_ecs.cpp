#include "aether/ecs/world.h"
#include "test_framework.h"

#include <array>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace aether;

namespace {

struct Position { f32 x = 0, y = 0, z = 0; };
struct Velocity { f32 x = 0, y = 0, z = 0; };
struct Health { i32 value = 100; };

// Non-trivial component to exercise the type-erased construct/move/destruct
// path (and catch double-destruct / leak bugs in archetype migration).
struct Name {
    std::string value;
};

} // namespace

AETHER_TEST(Entity_HashUsesBothIndexAndGeneration) {
    const std::hash<Entity> hash;
    const std::array<Entity, 8> handles{{
        {0, 7}, {1, 7}, {2, 7}, {0x80000000u, 7},
        {7, 0}, {7, 1}, {7, 2}, {7, 0x80000000u},
    }};
    std::unordered_set<usize> hashes;
    for (Entity handle : handles) {
        hashes.insert(hash(handle));
        AETHER_CHECK(hash(handle) == hash(Entity{handle.index, handle.generation}));
    }
    // Representative changes in either field, including its high bit, must
    // contribute on 32-bit hosts too. Truncating a packed u64 loses the index.
    AETHER_CHECK(hashes.size() == handles.size());
    if constexpr (sizeof(usize) >= sizeof(u64)) {
        AETHER_CHECK(hash(Entity{0x89abcdefu, 0xfedcba98u}) == static_cast<usize>(0x89abcdeffedcba98ull));
        AETHER_CHECK(hash(Entity{1, 0}) == static_cast<usize>(0x0000000100000000ull));
    }
}

AETHER_TEST(Entity_UnorderedMapKeepsRecycledHandlesDistinct) {
    World world;
    const Entity original = world.CreateEntity(Position{1, 0, 0});
    world.DestroyEntity(original);
    const Entity replacement = world.CreateEntity(Position{2, 0, 0});
    const Entity another = world.CreateEntity(Position{3, 0, 0});
    AETHER_CHECK(original.index == replacement.index);
    AETHER_CHECK(original.generation != replacement.generation);
    std::unordered_map<Entity, int> values{{original, 11}, {replacement, 22}, {another, 33}};
    AETHER_CHECK(values.size() == 3);
    AETHER_CHECK(values.at(original) == 11);
    AETHER_CHECK(values.at(replacement) == 22);
    AETHER_CHECK(values.at(another) == 33);
    values[Entity{replacement.index, replacement.generation}] = 44;
    AETHER_CHECK(values.size() == 3 && values.at(replacement) == 44);
    values.erase(original);
    AETHER_CHECK(!values.contains(original));
    AETHER_CHECK(values.size() == 2 && values.at(replacement) == 44 && values.at(another) == 33);
}

AETHER_TEST(World_CreateAndGetComponents) {
    World world;
    Entity e = world.CreateEntity(Position{1, 2, 3}, Velocity{4, 5, 6});

    Position* pos = world.GetComponent<Position>(e);
    Velocity* vel = world.GetComponent<Velocity>(e);
    AETHER_CHECK(pos != nullptr);
    AETHER_CHECK(vel != nullptr);
    AETHER_CHECK_NEAR(pos->x, 1.0f, 1e-6);
    AETHER_CHECK_NEAR(vel->z, 6.0f, 1e-6);

    AETHER_CHECK(world.GetComponent<Health>(e) == nullptr);
    AETHER_CHECK(!world.HasComponent<Health>(e));
}

AETHER_TEST(World_DestroyEntitySwapsLastEntityIn) {
    World world;
    Entity a = world.CreateEntity(Position{1, 0, 0});
    Entity b = world.CreateEntity(Position{2, 0, 0});
    Entity c = world.CreateEntity(Position{3, 0, 0});

    world.DestroyEntity(a);

    AETHER_CHECK(world.EntityCount() == 2);
    AETHER_CHECK(world.GetComponent<Position>(b)->x == 2.0f);
    AETHER_CHECK(world.GetComponent<Position>(c)->x == 3.0f);
}

AETHER_TEST(World_AddComponentMigratesArchetype) {
    World world;
    Entity e = world.CreateEntity(Position{1, 1, 1});
    AETHER_CHECK(!world.HasComponent<Velocity>(e));

    world.AddComponent<Velocity>(e, Velocity{9, 9, 9});

    AETHER_CHECK(world.HasComponent<Velocity>(e));
    AETHER_CHECK(world.HasComponent<Position>(e)); // survived the migration
    AETHER_CHECK_NEAR(world.GetComponent<Position>(e)->x, 1.0f, 1e-6);
    AETHER_CHECK_NEAR(world.GetComponent<Velocity>(e)->x, 9.0f, 1e-6);
}

AETHER_TEST(World_RemoveComponentMigratesArchetype) {
    World world;
    Entity e = world.CreateEntity(Position{1, 1, 1}, Velocity{2, 2, 2});

    world.RemoveComponent<Velocity>(e);

    AETHER_CHECK(!world.HasComponent<Velocity>(e));
    AETHER_CHECK(world.HasComponent<Position>(e));
    AETHER_CHECK_NEAR(world.GetComponent<Position>(e)->x, 1.0f, 1e-6);
}

AETHER_TEST(World_NonTrivialComponentSurvivesMigration) {
    World world;
    Entity e = world.CreateEntity(Position{}, Name{"aether"});

    world.AddComponent<Velocity>(e, Velocity{1, 2, 3});
    AETHER_CHECK(world.GetComponent<Name>(e)->value == "aether");

    world.RemoveComponent<Position>(e);
    AETHER_CHECK(world.GetComponent<Name>(e)->value == "aether");
}

AETHER_TEST(World_ForEachIteratesMatchingArchetypesOnly) {
    World world;
    world.CreateEntity(Position{1, 0, 0}, Velocity{1, 0, 0});
    world.CreateEntity(Position{2, 0, 0}, Velocity{1, 0, 0});
    world.CreateEntity(Position{3, 0, 0}); // no Velocity: should be excluded

    int count = 0;
    f32 sum_x = 0;
    world.ForEach<Position, Velocity>([&](Position& pos, Velocity& vel) {
        pos.x += vel.x;
        ++count;
        sum_x += pos.x;
    });

    AETHER_CHECK(count == 2);
    AETHER_CHECK_NEAR(sum_x, 2.0f + 3.0f, 1e-6); // (1+1) + (2+1)
}

AETHER_TEST(World_ForEachChunkSpansMultipleChunks) {
    World world;
    // Force multiple chunks: many entities of a component large enough that
    // kChunkSize / sizeof(Position) is comfortably smaller than this count.
    constexpr int kCount = 5000;
    std::vector<Entity> entities;
    entities.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        entities.push_back(world.CreateEntity(Position{static_cast<f32>(i), 0, 0}));
    }

    usize chunk_calls = 0;
    usize total_seen = 0;
    world.ForEachChunk<Position>([&](u32 count, Position* positions) {
        ++chunk_calls;
        for (u32 i = 0; i < count; ++i) {
            total_seen += static_cast<usize>(positions[i].x) >= 0 ? 1 : 0;
        }
    });

    AETHER_CHECK(chunk_calls > 1);
    AETHER_CHECK(total_seen == static_cast<usize>(kCount));

    for (Entity e : entities) {
        world.DestroyEntity(e);
    }
    AETHER_CHECK(world.EntityCount() == 0);
}

AETHER_TEST(World_EntityHandleGenerationPreventsStaleAccessAliasing) {
    World world;
    Entity a = world.CreateEntity(Position{1, 0, 0});
    world.DestroyEntity(a);

    Entity b = world.CreateEntity(Position{2, 0, 0});
    AETHER_CHECK(a.index == b.index);          // slot reused
    AETHER_CHECK(a.generation != b.generation); // but handle no longer matches
}

AETHER_TEST(World_RawAddAndRemoveComponentById) {
    World world;
    Entity e = world.CreateEntity(Position{1, 2, 3});
    ComponentId name_id = GetComponentId<Name>();
    AETHER_CHECK(RegisteredComponentCount() > name_id);

    AETHER_CHECK(!world.HasComponentRaw(e, name_id));
    world.AddComponentRaw(e, name_id);
    AETHER_CHECK(world.HasComponentRaw(e, name_id));
    AETHER_CHECK(world.GetComponent<Name>(e) != nullptr && world.GetComponent<Name>(e)->value.empty());
    AETHER_CHECK(world.GetComponent<Position>(e)->y == 2.0f); // existing component carried over

    world.GetComponent<Name>(e)->value = "a string long enough to allocate on the heap";
    world.AddComponentRaw(e, name_id); // already present: no-op
    AETHER_CHECK(world.GetComponent<Name>(e)->value == "a string long enough to allocate on the heap");

    world.RemoveComponentRaw(e, name_id);
    AETHER_CHECK(!world.HasComponentRaw(e, name_id));
    AETHER_CHECK(world.GetComponent<Position>(e)->z == 3.0f);
    world.RemoveComponentRaw(e, name_id); // absent: no-op
    AETHER_CHECK(world.EntityCount() == 1);
}
