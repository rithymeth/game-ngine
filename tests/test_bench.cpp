#include "test_framework.h"

#ifdef AETHER_TEST_HAS_BENCH

#include "aether/bench/bench.h"
#include "aether/ecs/world.h"
#include "aether/scene/components.h"
#include "aether/scene/serialization.h"

#include <nlohmann/json.hpp>

#include <chrono>

// The benchmark harness (Phase 38 step 1): running, the JSON report and the baseline comparison.

using namespace aether;

namespace {
volatile u64 g_work = 0;
}

AETHER_BENCH("test/tiny_loop", 4, [] {
    u64 x = 1;
    for (int i = 0; i < 20000; ++i) x = x * 6364136223846793005ull + 1;
    g_work += x;
});

AETHER_TEST(Bench_RunsAndReports) {
    const std::vector<bench::Result> all = bench::Run("test/tiny", 5);
    AETHER_CHECK(all.size() == 1 && all[0].name == "test/tiny_loop" && all[0].samples == 5 && all[0].iterations == 4);
    AETHER_CHECK(all[0].min_ms > 0.0 && all[0].min_ms <= all[0].median_ms && all[0].median_ms <= all[0].p95_ms && all[0].p95_ms <= all[0].max_ms && all[0].normalized > 0.0);
    AETHER_CHECK(bench::Run("no such benchmark", 3).empty());
    const double calibration = bench::CalibrationMs();
    AETHER_CHECK(calibration > 0.0);
    const nlohmann::json j = nlohmann::json::parse(bench::ToJson(all, calibration));
    AETHER_CHECK(j["results"].size() == 1 && j["results"][0]["name"] == "test/tiny_loop" && j["calibration_ms"].get<double>() > 0.0);
}

AETHER_TEST(Bench_BaselineComparisonFindsRegressions) {
    const auto report = [](double normalized) {
        bench::Result r;
        r.name = "a/b";
        r.normalized = normalized;
        bench::Result other;
        other.name = "a/c";
        other.normalized = 1.0;
        return bench::ToJson({r, other}, 10.0);
    };
    std::vector<bench::Regression> regressions;
    std::string error;
    AETHER_CHECK(bench::Compare(report(1.0), report(1.2), 1.5, regressions, &error) && regressions.empty()); // within tolerance
    AETHER_CHECK(bench::Compare(report(1.0), report(2.0), 1.5, regressions, &error) && regressions.size() == 1 && regressions[0].name == "a/b" && regressions[0].ratio > 1.9);
    AETHER_CHECK(bench::Compare(report(2.0), report(1.0), 1.5, regressions, &error) && regressions.empty()); // faster is fine
    AETHER_CHECK(!bench::Compare("nonsense", report(1.0), 1.5, regressions, &error) && !error.empty());
    // New benchmarks may be added alongside established baselines, but no-overlap reports fail.
    bench::Result fresh;
    fresh.name = "new/one";
    fresh.normalized = 9.0;
    AETHER_CHECK(!bench::Compare(report(1.0), bench::ToJson({fresh}, 10.0), 1.5, regressions, &error) && error.find("overlap") != std::string::npos);
    bench::Result established;
    established.name = "a/b";
    established.normalized = 1.2;
    bench::Result second_established;
    second_established.name = "a/c";
    second_established.normalized = 1.0;
    AETHER_CHECK(bench::Compare(report(1.0), bench::ToJson({fresh, established, second_established}, 10.0), 1.5, regressions, &error) && regressions.empty());
    AETHER_CHECK(!bench::Compare(report(1.0), bench::ToJson({established}, 10.0), 1.5, regressions, &error) && error.find("missing baseline benchmark: a/c") != std::string::npos);
    regressions.push_back({"stale", 1.0, 2.0, 2.0});
    AETHER_CHECK(!bench::Compare(report(1.0), report(1.2), 0.0, regressions, &error) && error.find("tolerance") != std::string::npos);
    AETHER_CHECK(regressions.empty());
    AETHER_CHECK(!bench::Compare(report(1.0), "{\"results\":[{\"name\":\"a/b\",\"normalized\":0}]}", 1.5, regressions, &error) && error.find("invalid") != std::string::npos);
    AETHER_CHECK(!bench::Compare("{\"results\":[{\"name\":\"a/b\",\"normalized\":1},{\"name\":\"a/b\",\"normalized\":2}]}",
                                 report(1.0), 1.5, regressions, &error) && error.find("duplicate") != std::string::npos);
}


// Sanitizers slow code several times over; the budget is for a normal build, so it scales up under them
// (still tight enough to catch an accidental quadratic).
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr double kSceneBudgetScale = 10.0;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr double kSceneBudgetScale = 10.0;
#else
constexpr double kSceneBudgetScale = 1.0;
#endif
#else
constexpr double kSceneBudgetScale = 1.0;
#endif

// The plan's budget: 10k entities save and load in under 2 seconds (a wide margin; a Debug build on a busy
// CI runner is the slow case), and saving the same world twice gives the same bytes.
AETHER_TEST(Bench_Scene10kRoundTripsUnderBudget) {
    World world;
    for (int i = 0; i < 10'000; ++i) {
        world.CreateEntity(Transform{Vec3(static_cast<f32>(i), 0, 0), Quaternion::Identity()});
    }
    const auto start = std::chrono::steady_clock::now();
    const std::vector<u8> bytes = SaveSceneToMemory(world);
    World loaded;
    AETHER_CHECK(LoadSceneFromMemory(loaded, bytes, "budget"));
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    usize count = 0;
    loaded.ForEach<Transform>([&](Transform&) { ++count; });
    AETHER_CHECK(count == 10'000);
    AETHER_CHECK(seconds < 2.0 * kSceneBudgetScale);
    AETHER_CHECK(SaveSceneToMemory(world) == bytes);
}

#endif
