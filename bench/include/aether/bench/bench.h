#pragma once

#include "aether/core/base.h"

#include <functional>
#include <string>
#include <vector>

namespace aether::bench {

// A benchmark: `body` is the work, run `iterations` times per sample; `setup` (optional) runs once
// before the samples and may keep state in captured variables.
struct Benchmark {
    std::string name;                  // "group/name"
    std::function<void()> setup;
    std::function<void()> body;
    u32 iterations = 1;                // body calls per sample
};

struct Result {
    std::string name;
    u32 samples = 0;
    u32 iterations = 0;
    double min_ms = 0.0, median_ms = 0.0, p95_ms = 0.0, max_ms = 0.0; // per body call
    double normalized = 0.0;           // median_ms divided by the calibration loop's: comparable across machines
};

std::vector<Benchmark>& Registry();

struct Registrar {
    Registrar(Benchmark b) { Registry().push_back(std::move(b)); }
};

// Time of the calibration workload in milliseconds (a fixed integer and memory loop; the median of a few runs).
double CalibrationMs();

// Runs the benchmarks whose name contains `filter` (all if empty): `samples` samples each, after one warm-up.
std::vector<Result> Run(const std::string& filter, u32 samples);

// The report as JSON text: {"calibration_ms": ..., "results": [{"name":..., "median_ms":..., ...}]}.
std::string ToJson(const std::vector<Result>& results, double calibration_ms);

struct Regression {
    std::string name;
    double baseline = 0.0, current = 0.0; // normalized medians
    double ratio = 0.0;
};
// Compares a report with a baseline (both JSON from ToJson) by normalized median: a benchmark in both whose
// current value is more than `tolerance` times the baseline's is a regression. `error` is set (and false returned)
// if either text isn't a report.
bool Compare(const std::string& baseline_json, const std::string& current_json, double tolerance, std::vector<Regression>& regressions, std::string* error);

} // namespace aether::bench

#define AETHER_BENCH_CONCAT2(a, b) a##b
#define AETHER_BENCH_CONCAT(a, b) AETHER_BENCH_CONCAT2(a, b)
// AETHER_BENCH("ecs/iterate", 10, [] { ... })  or  AETHER_BENCH_SETUP(name, iterations, setup_lambda, body_lambda).
// The body is variadic so commas inside it (template arguments) are fine.
#define AETHER_BENCH(name, iterations, ...) \
    static ::aether::bench::Registrar AETHER_BENCH_CONCAT(aether_bench_registrar_, __LINE__)(::aether::bench::Benchmark{name, nullptr, __VA_ARGS__, iterations})
#define AETHER_BENCH_SETUP(name, iterations, setup, ...) \
    static ::aether::bench::Registrar AETHER_BENCH_CONCAT(aether_bench_registrar_, __LINE__)(::aether::bench::Benchmark{name, setup, __VA_ARGS__, iterations})
