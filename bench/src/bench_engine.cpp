// Engine benchmarks (Phase 38 steps 2 and 3): ECS iteration and structural changes, and the job system.
#include "aether/bench/bench.h"
#include "aether/ecs/world.h"
#include "aether/job/job_system.h"
#include "aether/scene/components.h"

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
