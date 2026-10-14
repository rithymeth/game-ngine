#include "aether/bench/bench.h"
#include "aether/core/version.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

using namespace aether;

#ifndef AETHER_BENCH_COMPILER_ID
#define AETHER_BENCH_COMPILER_ID "unknown"
#define AETHER_BENCH_COMPILER_VERSION "unknown"
#define AETHER_BENCH_BUILD_CONFIG "unknown"
#define AETHER_BENCH_PLATFORM "unknown"
#define AETHER_BENCH_ARCH "unknown"
#endif

static const char* Environment(const char* name) {
    const char* value = std::getenv(name);
    return value && *value ? value : "unknown";
}

static bool ReadFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

int main(int argc, char** argv) {
    std::string filter, json_path, baseline_path;
    u32 samples = 7;
    double tolerance = 1.5;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--filter") && i + 1 < argc) filter = argv[++i];
        else if (!std::strcmp(argv[i], "--repeat") && i + 1 < argc) samples = static_cast<u32>(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--json") && i + 1 < argc) json_path = argv[++i];
        else if (!std::strcmp(argv[i], "--baseline") && i + 1 < argc) baseline_path = argv[++i];
        else if (!std::strcmp(argv[i], "--tolerance") && i + 1 < argc) tolerance = std::atof(argv[++i]);
        else {
            std::fprintf(stderr, "usage: aether_bench [--filter text] [--repeat N] [--json report.json] [--baseline file.json] [--tolerance 1.5]\n");
            return 2;
        }
    }
    double calibration = 0.0;
    const std::vector<bench::Result> results = bench::Run(filter, samples, &calibration);
    std::printf("calibration %.3f ms\n%-34s %10s %10s %10s %10s\n", calibration, "benchmark", "median ms", "p95 ms", "min ms", "norm");
    for (const bench::Result& r : results) std::printf("%-34s %10.4f %10.4f %10.4f %10.3f\n", r.name.c_str(), r.median_ms, r.p95_ms, r.min_ms, r.normalized);
    nlohmann::json report_json = nlohmann::json::parse(bench::ToJson(results, calibration));
    const u32 logical_processors = std::thread::hardware_concurrency();
    report_json["environment"] = {
        {"engine_version", kEngineVersion},
        {"compiler_id", AETHER_BENCH_COMPILER_ID},
        {"compiler_version", AETHER_BENCH_COMPILER_VERSION},
        {"build_configuration", AETHER_BENCH_BUILD_CONFIG},
        {"platform", AETHER_BENCH_PLATFORM},
        {"architecture", AETHER_BENCH_ARCH},
        {"logical_processors", logical_processors},
        {"runner_os", Environment("RUNNER_OS")},
        {"runner_arch", Environment("RUNNER_ARCH")},
        {"commit", Environment("GITHUB_SHA")}
    };
    const std::string report = report_json.dump(2);
    if (!json_path.empty()) {
        std::ofstream output(json_path, std::ios::binary);
        if (!output || !(output << report) || !output.flush()) {
            std::fprintf(stderr, "couldn't write benchmark report to %s\n", json_path.c_str());
            return 2;
        }
        std::printf("wrote %s\n", json_path.c_str());
    }
    if (!baseline_path.empty()) {
        std::string baseline, error;
        std::vector<bench::Regression> regressions;
        if (!ReadFile(baseline_path, baseline) || !bench::Compare(baseline, report, tolerance, regressions, &error)) {
            std::fprintf(stderr, "can't compare with %s: %s\n", baseline_path.c_str(), error.empty() ? "unreadable" : error.c_str());
            return 2;
        }
        for (const bench::Regression& r : regressions) std::printf("REGRESSION %s: %.2fx the baseline (limit %.2fx)\n", r.name.c_str(), r.ratio, tolerance);
        if (!regressions.empty()) return 1;
        std::printf("no regressions against %s (tolerance %.2fx)\n", baseline_path.c_str(), tolerance);
    }
    return 0;
}
