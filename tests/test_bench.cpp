#include "test_framework.h"

#ifdef AETHER_TEST_HAS_BENCH

#include "aether/bench/bench.h"

#include <nlohmann/json.hpp>

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
    // A benchmark only in the current report has nothing to compare against.
    bench::Result fresh;
    fresh.name = "new/one";
    fresh.normalized = 9.0;
    AETHER_CHECK(bench::Compare(report(1.0), bench::ToJson({fresh}, 10.0), 1.5, regressions, &error) && regressions.empty());
}

#endif
