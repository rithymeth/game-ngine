#include "aether/core/console.h"
#include "aether/core/log.h"
#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/serialization.h"
#include "aether/testing/functional_test.h"
#include "test_framework.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>

// Phase 23 step 5: functional tests - steps in order, simulation with a
// fixed step, "reaches" checks, failures (checks, timeouts, exceptions,
// missing entities and scenes), scenes loaded from files, captured logs,
// the registry's filters, JUnit and JSON reports, and the console command.

using namespace aether;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

struct Velocity {
    Vec3 v;
};

SystemDesc MoveSystem() {
    SystemDesc d;
    d.name = "Move";
    d.phase = SystemPhase::FixedUpdate;
    d.run = [](World& w, const FrameContext& f) { w.ForEach<Velocity, Transform>([&](Velocity& v, Transform& t) { t.position = t.position + v.v * f.dt; }); };
    return d;
}

FunctionalTest::Fn Runner(Vec3 velocity) {
    return [velocity](FunctionalContext& c) {
        const Entity e = c.world.CreateEntity(Transform{Vec3(), Quaternion{}}, Velocity{velocity});
        AddTag(c.world, e, "Runner");
        c.scheduler.Add(MoveSystem());
    };
}

} // namespace

AETHER_FUNCTIONAL_TEST(UnitPasses) {
    return FunctionalTest("Unit.Passes").Tag("unit-test").Setup(Runner(Vec3(1, 0, 0))).ExpectReaches("Runner", Vec3(2, 0, 0), Vec3(0.1f, 1, 1), 5);
}
AETHER_FUNCTIONAL_TEST(UnitFails) {
    return FunctionalTest("Unit.Fails").Tag("unit-test").Tag("broken").Check("never", [](FunctionalContext&) { return false; });
}

AETHER_TEST(Functional_StepsSimulationAndReaches) {
    int frames_seen = 0;
    const FunctionalResult r = FunctionalTest("Moves")
                                   .FixedStep(0.1f)
                                   .Setup(Runner(Vec3(2, 0, 0)))
                                   .Simulate(1.0, [&](FunctionalContext&) { ++frames_seen; })
                                   .Check("about 2 m along", [](FunctionalContext& c) {
                                       return std::abs(c.Position(c.FindTagged("Runner")).x - 2.0f) < 0.25f;
                                   })
                                   .Do("log", [](FunctionalContext& c) { c.Log("checkpoint"); })
                                   .ExpectReaches("Runner", Vec3(5, 0, 0), Vec3(0.15f, 1, 1), 3.0)
                                   .Run();
    CHECK(r.passed && r.failure.empty() && r.steps.size() == 5);
    CHECK(frames_seen == 10 && r.frames >= 25 && r.frames <= 26 && std::abs(r.simulated - 2.5) < 0.11);
    CHECK(r.steps[1].description == "simulate 1.00 s" && std::abs(r.steps[1].time - 1.0) < 1e-6);
    CHECK(r.steps[4].description.find("Runner reaches (5.00, 0.00, 0.00)") == 0);
    CHECK(std::find(r.log.begin(), r.log.end(), "checkpoint") != r.log.end() && r.wall_ms >= 0.0);
}

AETHER_TEST(Functional_FailuresAreExplained) {
    // A check that fails stops the test there.
    FunctionalResult r = FunctionalTest("Check").Check("1 + 1 is 3", [](FunctionalContext&) { return false; }).Do("never runs", [](FunctionalContext&) {}).Run();
    CHECK(!r.passed && r.steps.size() == 1 && r.failure == "1 + 1 is 3: the check didn't hold");
    // Not reaching in time says where it got to.
    r = FunctionalTest("Slow").Setup(Runner(Vec3(1, 0, 0))).ExpectReaches("Runner", Vec3(10, 0, 0), Vec3(0.5f, 1, 1), 2.0).Run();
    CHECK(!r.passed && r.failure.find("Runner is at (2.00, 0.00, 0.00) after 2.00 s") != std::string::npos);
    r = FunctionalTest("Nobody").ExpectReaches("Ghost", Vec3(), Vec3(1, 1, 1), 1).Run();
    CHECK(!r.passed && r.failure.find("no entity is tagged Ghost") != std::string::npos);
    r = FunctionalTest("Until").SimulateUntil("never", [](FunctionalContext&) { return false; }, 0.5).Run();
    CHECK(!r.passed && r.failure == "never: not done after 0.50 s" && std::abs(r.simulated - 0.5) < 0.02);
    r = FunctionalTest("Budget").Timeout(1.0).Simulate(5.0).Run();
    CHECK(!r.passed && r.failure.find("ran out of time") != std::string::npos);
    r = FunctionalTest("Throws").Do("boom", [](FunctionalContext&) { throw std::runtime_error("bad data"); }).Run();
    CHECK(!r.passed && r.failure == "boom: threw: bad data");
    r = FunctionalTest("Expect").Do("expectations", [](FunctionalContext& c) {
                                    c.Expect(true, "fine");
                                    c.Expect(false, "the first problem");
                                    c.Expect(false, "the second");
                                }).Run();
    CHECK(!r.passed && r.failure == "expectations: the first problem");
    Logger::Instance().SetStdout(false);
    r = FunctionalTest("NoScene").Scene("does/not/exist.json").Run();
    Logger::Instance().SetStdout(true);
    CHECK(!r.passed && r.failure.find("couldn't load does/not/exist.json") != std::string::npos && !r.log.empty());
}

AETHER_TEST(Functional_ScenesRegistryAndReports) {
    // A scene from a file.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_test_functional";
    std::filesystem::create_directories(dir);
    {
        World level;
        const Entity door = level.CreateEntity(Transform{Vec3(3, 0, 0), Quaternion{}});
        AddTag(level, door, "Door");
        CHECK(SaveSceneJson(level, (dir / "level.json").string()));
    }
    FunctionalResult r = FunctionalTest("Scene")
                             .Scene((dir / "level.json").string())
                             .Check("the door is there", [](FunctionalContext& c) { return c.Position(c.FindTagged("Door")).x == 3.0f; })
                             .Run();
    CHECK(r.passed);

    // The registry: filters and tags.
    FunctionalRunOptions unit;
    unit.tags = {"unit-test"};
    CHECK(FunctionalTestRegistry::Get().Matching(unit).size() == 2);
    unit.filters = {"Passes"};
    CHECK(FunctionalTestRegistry::Get().Matching(unit).size() == 1);
    unit.filters.clear();
    FunctionalReport report = FunctionalTestRegistry::Get().Run(unit);
    CHECK(report.results.size() == 2 && report.Passed() == 1 && report.Failed() == 1 && !report.Ok());
    CHECK(report.results[0].name == "Unit.Fails" && report.results[1].name == "Unit.Passes"); // by name
    unit.stop_on_failure = true;
    CHECK(FunctionalTestRegistry::Get().Run(unit).results.size() == 1);
    const std::string summary = FormatFunctionalSummary(report);
    CHECK(summary.find("[FAIL] Unit.Fails") != std::string::npos && summary.find("1/2 functional tests passed") != std::string::npos);

    // JUnit and JSON.
    const std::string xml_path = (dir / "results.xml").string(), json_path = (dir / "results.json").string();
    CHECK(WriteJUnitXml(report, xml_path) && WriteJsonReport(report, json_path));
    std::ifstream xml_in(xml_path);
    const std::string xml((std::istreambuf_iterator<char>(xml_in)), std::istreambuf_iterator<char>());
    CHECK(xml.find("<testsuites tests=\"2\" failures=\"1\"") != std::string::npos);
    CHECK(xml.find("<testcase classname=\"functional\" name=\"Unit.Fails\"") != std::string::npos);
    CHECK(xml.find("<failure message=\"never: the check didn&apos;t hold\"/>") != std::string::npos);
    std::ifstream json_in(json_path);
    const nlohmann::json j = nlohmann::json::parse(json_in);
    CHECK(j["passed"] == 1 && j["failed"] == 1 && j["tests"].size() == 2 && j["tests"][1]["steps"].size() == 2);
    CHECK(!WriteJUnitXml(report, (dir / "no" / "dir.xml").string()));
    std::filesystem::remove_all(dir);

    // The console.
    Console console;
    CHECK(console.Execute("functional.list"));
    CHECK(std::any_of(console.Output().begin(), console.Output().end(), [](const ConsoleLine& l) { return l.text == "Unit.Fails #unit-test #broken"; }));
    CHECK(console.Execute("functional.run Unit.Passes"));
    CHECK(console.Output().back().text == "1/1 functional tests passed");
}
