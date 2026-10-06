// Engine benchmarks (Phase 38 steps 2, 3 and 4): ECS iteration and structural changes, the job system, and
// scene save and load.
#include "aether/bench/bench.h"
#include "aether/ecs/world.h"
#include "aether/job/job_system.h"
#include "aether/scene/components.h"
#include "aether/scene/serialization.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

using namespace aether;

namespace {

struct Velocity {
    Vec3 value;
};

struct EcsState {
    World world;
    std::vector<Entity> entities;
};
EcsState& Ecs() {
    static EcsState s;
    return s;
}

void Spin(void* data) {
    auto* x = static_cast<u64*>(data);
    for (int i = 0; i < 200; ++i) *x = *x * 6364136223846793005ull + 1442695040888963407ull;
}

World& SceneWorld() {
    static World world;
    return world;
}

struct JobState {
    std::unique_ptr<JobSystem> jobs;
    std::vector<u64> data = std::vector<u64>(1024, 1);
    std::vector<JobDecl> decls;
};
JobState& Jobs() {
    static JobState s;
    return s;
}

} // namespace

// Moving 100k entities with a Transform and a Velocity: the everyday system loop.
AETHER_BENCH_SETUP(
    "ecs/iterate_100k", 20,
    [] {
        EcsState& s = Ecs();
        for (int i = 0; i < 100'000; ++i) s.entities.push_back(s.world.CreateEntity(Transform{Vec3(static_cast<f32>(i), 0, 0), Quaternion::Identity()}, Velocity{Vec3(1, 0, 0)}));
    },
    [] {
        Ecs().world.ForEach<Transform, Velocity>([](Transform& t, Velocity& v) { t.position = t.position + v.value * 0.016f; });
    });

// Creating and destroying 10k entities (structural changes, which move rows between chunks).
AETHER_BENCH("ecs/create_destroy_10k", 5, [] {
    World world;
    std::vector<Entity> made;
    made.reserve(10'000);
    for (int i = 0; i < 10'000; ++i) made.push_back(world.CreateEntity(Transform{}, Velocity{}));
    for (const Entity e : made) world.DestroyEntity(e);
});

// Adding and removing a component on 10k entities (moves between archetypes).
AETHER_BENCH("ecs/add_remove_component_10k", 5, [] {
    World world;
    std::vector<Entity> made;
    made.reserve(10'000);
    for (int i = 0; i < 10'000; ++i) made.push_back(world.CreateEntity(Transform{}));
    for (const Entity e : made) world.AddComponent<Velocity>(e, Velocity{});
    for (const Entity e : made) world.RemoveComponent<Velocity>(e);
});

// A batch of 1024 small jobs through the work-stealing scheduler.
AETHER_BENCH_SETUP(
    "jobs/batch_1024", 20,
    [] {
        JobState& s = Jobs();
        s.jobs = std::make_unique<JobSystem>();
        for (usize i = 0; i < s.data.size(); ++i) s.decls.push_back({&Spin, &s.data[i]});
    },
    [] {
        JobState& s = Jobs();
        JobCounter counter{0};
        s.jobs->ScheduleBatch(s.decls.data(), static_cast<u32>(s.decls.size()), counter);
        s.jobs->Wait(counter);
    });

// ---------------------------------------------------------------------------------------------------
// Scene save and load (Phase 38 step 4): 10k entities through the binary and the JSON formats.
// ---------------------------------------------------------------------------------------------------

namespace {

constexpr int kSceneEntities = 10'000;

struct SceneState {
    std::vector<u8> binary;
    std::vector<u8> json;
};
SceneState& SceneData() {
    static SceneState s;
    return s;
}

void FillSceneWorld(World& world) {
    for (int i = 0; i < kSceneEntities; ++i) {
        world.CreateEntity(Transform{Vec3(static_cast<f32>(i), static_cast<f32>(i % 97), 0), Quaternion::Identity()});
    }
}

} // namespace

AETHER_BENCH_SETUP(
    "scene/save_binary_10k", 5,
    [] { FillSceneWorld(SceneWorld()); },
    [] { SceneData().binary = SaveSceneToMemory(SceneWorld()); });

AETHER_BENCH_SETUP(
    "scene/load_binary_10k", 5,
    [] {
        World world;
        FillSceneWorld(world);
        SceneData().binary = SaveSceneToMemory(world);
    },
    [] {
        World world;
        LoadSceneFromMemory(world, SceneData().binary, "bench");
    });

AETHER_BENCH_SETUP(
    "scene/save_json_10k", 3,
    [] { FillSceneWorld(SceneWorld()); },
    [] {
        const std::filesystem::path path = std::filesystem::temp_directory_path() / "aether_bench_scene_save.json";
        SaveSceneJson(SceneWorld(), path.string());
        std::filesystem::remove(path);
    });

AETHER_BENCH_SETUP(
    "scene/load_json_10k", 3,
    [] {
        World world;
        FillSceneWorld(world);
        const std::filesystem::path path = std::filesystem::temp_directory_path() / "aether_bench_scene_load.json";
        SaveSceneJson(world, path.string());
        std::ifstream in(path, std::ios::binary);
        SceneData().json.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        in.close();
        std::filesystem::remove(path);
    },
    [] {
        World world;
        LoadSceneJsonFromMemory(world, SceneData().json, "bench");
    });
