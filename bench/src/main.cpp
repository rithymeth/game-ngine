#include "aether/bench/bench.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

using namespace aether;

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
    const double calibration = bench::CalibrationMs();
    const std::vector<bench::Result> results = bench::Run(filter, samples);
    std::printf("calibration %.3f ms\n%-34s %10s %10s %10s %10s\n", calibration, "benchmark", "median ms", "p95 ms", "min ms", "norm");
    for (const bench::Result& r : results) std::printf("%-34s %10.4f %10.4f %10.4f %10.3f\n", r.name.c_str(), r.median_ms, r.p95_ms, r.min_ms, r.normalized);
    const std::string report = bench::ToJson(results, calibration);
    if (!json_path.empty()) {
        std::ofstream(json_path, std::ios::binary) << report;
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
