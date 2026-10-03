#pragma once

#include "aether/ecs/world.h"
#include "aether/math/math.h"
#include "aether/scene/scheduler.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aether {

// Functional tests (Phase 23 step 5, docs/design/PHASE_SPECS.md §23.5):
// whole-game scenarios run headless - "load the scene, simulate 5 s,
// assert that the entity tagged Player reached the box around the exit".
//
//     AETHER_FUNCTIONAL_TEST(PlayerReachesExit) {
//         return FunctionalTest("PlayerReachesExit")
//             .Scene("levels/test_corridor.json")
//             .Setup([](FunctionalContext& c) { c.scheduler.Add(MovementSystem()); })
//             .ExpectReaches("Player", Vec3(20, 0, 0), Vec3(1, 2, 1), 10.0)
//             .Check("no enemies left", [](FunctionalContext& c) { return c.AllTagged("Enemy").empty(); });
//     }
//
// Steps run in order on one world with a fixed time step; the first that
// fails ends the test. Results name the step, the simulated time and why,
// and carry the log lines written meanwhile. The registry runs them by
// name or tag, and writes JUnit XML and JSON reports for CI; the
// aether_functional runner does that from the command line.

class FunctionalContext {
public:
    World& world;
    SystemScheduler& scheduler;
    f64 time = 0.0; // simulated seconds
    u64 frame = 0;

    FunctionalContext(World& w, SystemScheduler& s) : world(w), scheduler(s) {}

    Entity FindTagged(std::string_view tag) const; // the first, or null
    std::vector<Entity> AllTagged(std::string_view tag) const;
    Vec3 Position(Entity e) const;                  // its Transform's (zero without one)
    bool Inside(Entity e, const Vec3& center, const Vec3& half_extents) const;
    void Log(const std::string& text); // into the result
    // Marks the current step failed with `message` (it still runs to its end).
    void Fail(const std::string& message);
    bool Expect(bool condition, const std::string& message) {
        if (!condition) Fail(message);
        return condition;
    }

private:
    friend class FunctionalTest;
    std::vector<std::string>* log_ = nullptr;
    std::string failure_;
};

struct FunctionalStepResult {
    std::string description;
    bool passed = true;
    std::string message; // why it failed
    f64 time = 0.0;      // simulated seconds when it finished
};

struct FunctionalResult {
    std::string name;
    std::vector<std::string> tags;
    bool passed = true;
    std::string failure; // the failing step's description and message
    std::vector<FunctionalStepResult> steps;
    f64 simulated = 0.0, wall_ms = 0.0;
    u64 frames = 0;
    std::vector<std::string> log;
};

class FunctionalTest {
public:
    using Fn = std::function<void(FunctionalContext&)>;
    using Pred = std::function<bool(FunctionalContext&)>;

    explicit FunctionalTest(std::string name);

    FunctionalTest& Tag(std::string tag);
    FunctionalTest& FixedStep(f32 dt);  // 1/60 by default
    FunctionalTest& Scene(std::string path); // loaded into the world first (binary or JSON scene)
    FunctionalTest& Setup(Fn fn);       // build the world, add systems
    FunctionalTest& Do(std::string description, Fn fn);
    FunctionalTest& Simulate(f64 seconds, Fn each_frame = {});
    // Simulates until `done` holds; fails after `timeout` simulated seconds.
    FunctionalTest& SimulateUntil(std::string description, Pred done, f64 timeout, Fn each_frame = {});
    FunctionalTest& Check(std::string description, Pred check);
    // "Entity X reached trigger Y": the first entity tagged `tag` is inside the box within `timeout`.
    FunctionalTest& ExpectReaches(std::string tag, const Vec3& center, const Vec3& half_extents, f64 timeout);
    // Steps across all simulation, at most (default 600 simulated seconds).
    FunctionalTest& Timeout(f64 seconds);

    const std::string& Name() const { return name_; }
    const std::vector<std::string>& Tags() const { return tags_; }
    FunctionalResult Run() const;

private:
    struct Step {
        std::string description;
        std::function<bool(FunctionalContext&, FrameLoop&, f32 dt, f64 budget)> run;
    };
    std::string name_;
    std::vector<std::string> tags_;
    f32 dt_ = 1.0f / 60.0f;
    f64 timeout_ = 600.0;
    std::vector<Step> steps_;
};

struct FunctionalRunOptions {
    std::vector<std::string> filters; // names containing any of these (empty: all)
    std::vector<std::string> tags;    // and having any of these tags (empty: any)
    bool stop_on_failure = false;
};

struct FunctionalReport {
    std::vector<FunctionalResult> results;
    usize Passed() const;
    usize Failed() const { return results.size() - Passed(); }
    bool Ok() const { return Failed() == 0; }
    f64 WallMs() const;
};

class FunctionalTestRegistry {
public:
    static FunctionalTestRegistry& Get();
    void Add(std::function<FunctionalTest()> make);
    std::vector<FunctionalTest> Tests() const; // by name
    std::vector<FunctionalTest> Matching(const FunctionalRunOptions& options) const;
    FunctionalReport Run(const FunctionalRunOptions& options = {}) const;

private:
    std::vector<std::function<FunctionalTest()>> makers_;
};

bool WriteJUnitXml(const FunctionalReport& report, const std::string& path, const std::string& suite = "functional");
bool WriteJsonReport(const FunctionalReport& report, const std::string& path);
std::string FormatFunctionalSummary(const FunctionalReport& report); // a few lines for a console or terminal

struct FunctionalTestRegistrar {
    explicit FunctionalTestRegistrar(std::function<FunctionalTest()> make) { FunctionalTestRegistry::Get().Add(std::move(make)); }
};

} // namespace aether

#define AETHER_FUNCTIONAL_TEST(Name)                                                                     \
    static ::aether::FunctionalTest AetherFunctional_##Name();                                           \
    static const ::aether::FunctionalTestRegistrar AetherFunctionalRegistrar_##Name(&AetherFunctional_##Name); \
    static ::aether::FunctionalTest AetherFunctional_##Name()
