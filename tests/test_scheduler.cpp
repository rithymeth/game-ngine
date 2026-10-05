#include "aether/scene/scheduler.h"
#include "test_framework.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

using namespace aether;

namespace scheduler_test {
struct Health {
    f32 value = 100.0f;
};
struct Score {
    i32 value = 0;
};
struct Ammo {
    i32 value = 0;
};
} // namespace scheduler_test

AETHER_REFLECT(scheduler_test::Health, 1, AETHER_FIELD(value))
AETHER_REFLECT(scheduler_test::Score, 1, AETHER_FIELD(value))
AETHER_REFLECT(scheduler_test::Ammo, 1, AETHER_FIELD(value))

namespace {

using namespace scheduler_test;
using Names = std::vector<std::string>;

SystemDesc Make(const std::string& name, ComponentMask reads, ComponentMask writes, Names after = {},
                SystemPhase phase = SystemPhase::Update) {
    SystemDesc system;
    system.name = name;
    system.phase = phase;
    system.reads = reads;
    system.writes = writes;
    system.after = std::move(after);
    system.run = [](World&, const FrameContext&) {};
    return system;
}

bool Near(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) < eps; }

} // namespace

AETHER_TEST(Scheduler_OrdersByAccessAndAfter) {
    SystemScheduler s;
    AETHER_CHECK(s.Add(Make("Move", {}, Access<Transform>())));
    AETHER_CHECK(s.Add(Make("Regen", {}, Access<Health>())));
    AETHER_CHECK(s.Add(Make("Aim", Access<Transform>(), Access<Ammo>())));    // reads what Move writes
    AETHER_CHECK(s.Add(Make("Hud", {}, {}, {"Aim"})));                        // explicitly after Aim
    AETHER_CHECK(s.Add(Make("Score", Access<Health>(), Access<Score>())));    // reads what Regen writes
    AETHER_CHECK(s.Add(Make("Log", Access<Health, Transform>(), {})));        // readers don't conflict with readers
    std::string error;
    AETHER_CHECK(s.Build(&error));
    AETHER_CHECK((s.Levels(SystemPhase::Update) ==
                  std::vector<Names>{{"Move", "Regen"}, {"Aim", "Score", "Log"}, {"Hud"}}));
    AETHER_CHECK((s.Order(SystemPhase::Update) == Names{"Move", "Regen", "Aim", "Score", "Log", "Hud"}));
    AETHER_CHECK(s.Order(SystemPhase::FixedUpdate).empty());

    // An explicit `after` wins over registration order for conflicting systems.
    SystemScheduler t;
    t.Add(Make("Integrate", {}, Access<Transform>(), {"Input"}));
    t.Add(Make("Input", {}, Access<Transform>()));
    AETHER_CHECK((t.Levels(SystemPhase::Update) == std::vector<Names>{{"Input"}, {"Integrate"}}));
}

AETHER_TEST(Scheduler_RejectsBadGraphs) {
    std::string error;
    SystemScheduler s;
    AETHER_CHECK(s.Add(Make("A", {}, {})));
    AETHER_CHECK(!s.Add(Make("A", {}, {}), &error) && error.find("already") != std::string::npos);
    SystemDesc no_run = Make("B", {}, {});
    no_run.run = nullptr;
    AETHER_CHECK(!s.Add(no_run, &error));

    SystemScheduler unknown;
    unknown.Add(Make("A", {}, {}, {"Ghost"}));
    AETHER_CHECK(!unknown.Build(&error) && error.find("Ghost") != std::string::npos);

    SystemScheduler cross;
    cross.Add(Make("Physics", {}, {}, {}, SystemPhase::FixedUpdate));
    cross.Add(Make("Camera", {}, {}, {"Physics"}, SystemPhase::LateUpdate));
    AETHER_CHECK(!cross.Build(&error) && error.find("FixedUpdate") != std::string::npos);

    SystemScheduler cycle;
    cycle.Add(Make("P", {}, {}, {"Q"}));
    cycle.Add(Make("Q", {}, {}, {"R"}));
    cycle.Add(Make("R", {}, {}, {"P"}));
    cycle.Add(Make("Fine", {}, {}));
    AETHER_CHECK(!cycle.Build(&error) && error.find("cycle") != std::string::npos);
    AETHER_CHECK(error.find("P") != std::string::npos && error.find("Fine") == std::string::npos);
}

AETHER_TEST(Scheduler_RunsInParallelWithoutConflicts) {
    // Each run registers the components it touches while it runs; an overlap
    // with a conflicting system that's also running is a scheduling bug.
    std::mutex mutex;
    std::vector<std::pair<ComponentMask, ComponentMask>> running; // (reads, writes)
    int max_concurrent = 0;
    int violations = 0;
    std::vector<std::string> ran;
    const std::thread::id main_thread = std::this_thread::get_id();
    bool main_only_on_main = true;

    SystemScheduler s;
    auto add = [&](const std::string& name, ComponentMask reads, ComponentMask writes, bool main_only = false) {
        SystemDesc system = Make(name, reads, writes);
        system.main_thread_only = main_only;
        system.run = [&, name, reads, writes, main_only](World&, const FrameContext&) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                for (const auto& [r, w] : running) {
                    if ((w & (reads | writes)).any() || (writes & r).any()) {
                        ++violations;
                    }
                }
                running.push_back({reads, writes});
                max_concurrent = std::max(max_concurrent, static_cast<int>(running.size()));
                if (main_only && std::this_thread::get_id() != main_thread) {
                    main_only_on_main = false;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            std::lock_guard<std::mutex> lock(mutex);
            running.erase(std::find(running.begin(), running.end(), std::make_pair(reads, writes)));
            ran.push_back(name);
        };
        s.Add(std::move(system));
    };
    add("W1", {}, Access<Transform>());
    add("W2", {}, Access<Health>());
    add("W3", {}, Access<Score>());
    add("W4", {}, Access<Ammo>());
    add("R1", Access<Transform, Health>(), {});
    add("R2", Access<Transform, Score>(), {});
    add("Ui", Access<Score>(), {}, /*main_only=*/true);
    add("W1b", {}, Access<Transform>());

    JobSystem jobs(3);
    World world;
    FrameContext frame;
    for (int round = 0; round < 3; ++round) {
        s.RunPhase(world, SystemPhase::Update, frame, &jobs);
    }
    AETHER_CHECK(violations == 0 && ran.size() == 24 && main_only_on_main);
    AETHER_CHECK(max_concurrent > 1); // the four independent writers overlap
    // Without a job system: the sequential order, one at a time.
    ran.clear();
    max_concurrent = 0;
    s.RunPhase(world, SystemPhase::Update, frame, nullptr);
    AETHER_CHECK(max_concurrent == 1 && ran == s.Order(SystemPhase::Update));
}

AETHER_TEST(FixedTimestep_AccumulatesAndCaps) {
    FixedTimestep step(0.02f, 5);
    AETHER_CHECK(step.Advance(0.01f) == 0 && Near(step.Alpha(), 0.5f));
    AETHER_CHECK(step.Advance(0.015f) == 1 && Near(step.Alpha(), 0.25f));
    AETHER_CHECK(step.Advance(0.04f) == 2 && Near(step.Alpha(), 0.25f));
    // A one-second hitch: 5 steps, the rest dropped (keeping the fraction).
    AETHER_CHECK(step.Advance(1.0f) == 5 && step.DroppedTime() > 0.89 && step.Alpha() < 1.0f);
    AETHER_CHECK(step.Advance(0.0f) == 0 && step.Advance(-1.0f) == 0);

    // Over time, 60 Hz fixed from 144 Hz frames: steps average out to the fixed rate.
    FixedTimestep sixty(1.0f / 60.0f, 5);
    u32 total = 0;
    for (int i = 0; i < 144 * 10; ++i) {
        total += sixty.Advance(1.0f / 144.0f);
    }
    AETHER_CHECK(total >= 599 && total <= 600);
}

AETHER_TEST(FrameLoop_RunsPhasesAndInterpolates) {
    SystemScheduler s;
    std::vector<std::string> log;
    auto logging = [&](const std::string& name, SystemPhase phase) {
        SystemDesc system = Make(name, {}, {}, {}, phase);
        system.run = [&, name](World&, const FrameContext& frame) {
            log.push_back(name + (name == "fixed" ? std::to_string(frame.fixed_step) : ""));
        };
        return system;
    };
    s.Add(logging("pre", SystemPhase::PreUpdate));
    s.Add(logging("fixed", SystemPhase::FixedUpdate));
    s.Add(logging("update", SystemPhase::Update));
    s.Add(logging("late", SystemPhase::LateUpdate));
    s.Add(logging("render", SystemPhase::PreRender));
    // A moving entity, simulated at the fixed rate and recorded for interpolation.
    s.Add(RecordPreviousTransformsSystem());
    SystemDesc mover = Make("Mover", {}, Access<Transform>(), {"RecordPreviousTransforms"}, SystemPhase::FixedUpdate);
    mover.run = [](World& world, const FrameContext& frame) {
        world.ForEach<Transform>([&](Transform& t) { t.position.x += 10.0f * frame.dt; });
    };
    s.Add(mover);

    World world;
    Entity e = world.CreateEntity(Transform{}, PreviousTransform{});
    FrameLoop loop(s, FixedTimestep(0.1f, 5));
    FrameContext frame = loop.Tick(world, 0.25f);
    AETHER_CHECK((log == std::vector<std::string>{"pre", "fixed1", "fixed2", "update", "late", "render"}));
    AETHER_CHECK(frame.frame == 0 && frame.fixed_step == 2 && Near(frame.alpha, 0.5f) && Near(static_cast<f32>(frame.time), 0.25f));
    // Two fixed steps of 1 m each: current x = 2, previous x = 1; render at alpha 0.5.
    const Transform& current = *world.GetComponent<Transform>(e);
    const PreviousTransform& previous = *world.GetComponent<PreviousTransform>(e);
    AETHER_CHECK(Near(current.position.x, 2.0f) && Near(previous.position.x, 1.0f));
    AETHER_CHECK(Near(InterpolateTransform(previous, current, frame.alpha).position.x, 1.5f));
    log.clear();
    frame = loop.Tick(world, 0.01f); // not enough for a step
    AETHER_CHECK((log == std::vector<std::string>{"pre", "update", "late", "render"}) && frame.frame == 1);
    AETHER_CHECK(Near(frame.alpha, 0.6f));

    // Rotation interpolation takes the short way, whichever sign the target has.
    const Quaternion a = Quaternion::Identity();
    const Quaternion b = Quaternion::FromAxisAngle(Vec3{0, 1, 0}, 3.14159265f / 2.0f);
    const Quaternion half = NlerpRotation(a, b, 0.5f);
    const Quaternion expected = Quaternion::FromAxisAngle(Vec3{0, 1, 0}, 3.14159265f / 4.0f);
    AETHER_CHECK(Near(half.y, expected.y, 1e-3f) && Near(half.w, expected.w, 1e-3f));
    const Quaternion negated(-b.x, -b.y, -b.z, -b.w);
    const Quaternion same = NlerpRotation(a, negated, 0.5f);
    AETHER_CHECK(Near(std::fabs(same.y), expected.y, 1e-3f) && Near(std::fabs(same.w), expected.w, 1e-3f));
    AETHER_CHECK(Near(InterpolateTransform(previous, current, 5.0f).position.x, 2.0f)); // clamped
}
