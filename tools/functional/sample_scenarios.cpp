// Sample functional tests: a seeker walking to its goal, and a spawner.
#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"
#include "aether/testing/functional_test.h"

#include <algorithm>

using namespace aether;

namespace {

struct Seek {
    f32 speed = 4.0f; // m/s towards the first entity tagged "Goal"
};

struct Spawner {
    f32 every = 0.5f, timer = 0.0f;
    u32 left = 5;
};

SystemDesc SeekSystem() {
    SystemDesc d;
    d.name = "Seek";
    d.phase = SystemPhase::FixedUpdate;
    d.run = [](World& world, const FrameContext& frame) {
        const std::vector<Entity> goals = FindEntitiesWithTag(world, "Goal");
        if (goals.empty()) return;
        const Vec3 goal = world.GetComponent<Transform>(goals[0])->position;
        world.ForEach<Seek, Transform>([&](Seek& s, Transform& t) {
            const Vec3 to = goal - t.position;
            const f32 step = s.speed * frame.dt, dist = to.Length();
            t.position = dist <= step ? goal : t.position + to * (step / dist);
        });
    };
    return d;
}

SystemDesc SpawnerSystem() {
    SystemDesc d;
    d.name = "Spawner";
    d.phase = SystemPhase::FixedUpdate;
    d.run = [](World& world, const FrameContext& frame) {
        std::vector<Vec3> spawns;
        world.ForEach<Spawner, Transform>([&](Spawner& s, Transform& t) {
            s.timer += frame.dt;
            while (s.left > 0 && s.timer >= s.every) s.timer -= s.every, --s.left, spawns.push_back(t.position);
        });
        for (const Vec3& p : spawns) {
            const Entity crate = world.CreateEntity(Transform{p, Quaternion{}});
            AddTag(world, crate, "Crate");
        }
    };
    return d;
}

} // namespace

AETHER_FUNCTIONAL_TEST(SeekerReachesGoal) {
    return FunctionalTest("Sample.SeekerReachesGoal")
        .Tag("sample")
        .Setup([](FunctionalContext& c) {
            const Entity player = c.world.CreateEntity(Transform{Vec3(0, 0, 0), Quaternion{}}, Seek{});
            AddTag(c.world, player, "Player");
            const Entity goal = c.world.CreateEntity(Transform{Vec3(10, 0, 4), Quaternion{}});
            AddTag(c.world, goal, "Goal");
            c.scheduler.Add(SeekSystem());
        })
        .ExpectReaches("Player", Vec3(10, 0, 4), Vec3(0.25f, 1, 0.25f), 5.0)
        .Check("it took about distance / speed", [](FunctionalContext& c) { return c.time > 2.5 && c.time < 3.2; });
}

AETHER_FUNCTIONAL_TEST(SpawnerSpawnsFive) {
    return FunctionalTest("Sample.SpawnerSpawnsFive")
        .Tag("sample")
        .Setup([](FunctionalContext& c) {
            c.world.CreateEntity(Transform{Vec3(1, 2, 3), Quaternion{}}, Spawner{});
            c.scheduler.Add(SpawnerSystem());
        })
        .Simulate(1.0)
        .Check("two so far", [](FunctionalContext& c) { return c.AllTagged("Crate").size() == 2; })
        .SimulateUntil("all five", [](FunctionalContext& c) { return c.AllTagged("Crate").size() == 5; }, 5.0)
        .Simulate(2.0)
        .Check("and no more, at the spawner", [](FunctionalContext& c) {
            const auto crates = c.AllTagged("Crate");
            return crates.size() == 5 && std::all_of(crates.begin(), crates.end(), [&](Entity e) {
                       return (c.Position(e) - Vec3(1, 2, 3)).Length() < 1e-5f;
                   });
        });
}
