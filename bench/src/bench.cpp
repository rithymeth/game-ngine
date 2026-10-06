#include "aether/bench/bench.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <numeric>

namespace aether::bench {

std::vector<Benchmark>& Registry() {
    static std::vector<Benchmark> registry;
    return registry;
}

namespace {

using Clock = std::chrono::steady_clock;

double Ms(Clock::time_point from) { return std::chrono::duration<double, std::milli>(Clock::now() - from).count(); }

volatile u64 g_sink = 0;

// A fixed workload: an integer mix over a buffer that fits in L2. Its time stands for "how fast is this machine today".
void CalibrationWork() {
    static std::vector<u32> data(1u << 16);
    u32 x = 12345u;
    for (usize pass = 0; pass < 24; ++pass) {
        for (usize i = 0; i < data.size(); ++i) {
            x = x * 1664525u + 1013904223u;
            data[i] = data[i] * 31u + x;
        }
    }
    g_sink += data[x % data.size()];
}

double Percentile(std::vector<double> sorted, double p) {
    if (sorted.empty()) return 0.0;
    const usize i = std::min(sorted.size() - 1, static_cast<usize>(p * static_cast<double>(sorted.size() - 1) + 0.5));
    return sorted[i];
}

} // namespace

double CalibrationMs() {
    CalibrationWork(); // warm up
    std::vector<double> t;
    for (int i = 0; i < 5; ++i) {
        const auto start = Clock::now();
        CalibrationWork();
        t.push_back(Ms(start));
    }
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
}

std::vector<Result> Run(const std::string& filter, u32 samples) {
    std::vector<Result> out;
    const double calibration = CalibrationMs();
    samples = std::max<u32>(samples, 1);
    for (Benchmark& b : Registry()) {
        if (!filter.empty() && b.name.find(filter) == std::string::npos) continue;
        if (b.setup) b.setup();
        b.body(); // warm up
        std::vector<double> t;
        for (u32 s = 0; s < samples; ++s) {
            const auto start = Clock::now();
            for (u32 i = 0; i < std::max<u32>(b.iterations, 1); ++i) b.body();
            t.push_back(Ms(start) / static_cast<double>(std::max<u32>(b.iterations, 1)));
        }
        std::sort(t.begin(), t.end());
        Result r;
        r.name = b.name;
        r.samples = samples;
        r.iterations = std::max<u32>(b.iterations, 1);
        r.min_ms = t.front();
        r.median_ms = Percentile(t, 0.5);
        r.p95_ms = Percentile(t, 0.95);
        r.max_ms = t.back();
        r.normalized = calibration > 0.0 ? r.median_ms / calibration : 0.0;
        out.push_back(std::move(r));
    }
    return out;
}

std::string ToJson(const std::vector<Result>& results, double calibration_ms) {
    nlohmann::json j = {{"calibration_ms", calibration_ms}, {"results", nlohmann::json::array()}};
    for (const Result& r : results) {
        j["results"].push_back({{"name", r.name}, {"samples", r.samples}, {"iterations", r.iterations}, {"min_ms", r.min_ms}, {"median_ms", r.median_ms},
                                {"p95_ms", r.p95_ms}, {"max_ms", r.max_ms}, {"normalized", r.normalized}});
    }
    return j.dump(2);
}

bool Compare(const std::string& baseline_json, const std::string& current_json, double tolerance, std::vector<Regression>& regressions, std::string* error) {
    const auto base = nlohmann::json::parse(baseline_json, nullptr, false);
    const auto cur = nlohmann::json::parse(current_json, nullptr, false);
    const auto ok = [](const nlohmann::json& j) { return j.is_object() && j.contains("results") && j["results"].is_array(); };
    if (!ok(base) || !ok(cur)) {
        if (error) *error = "not a benchmark report";
        return false;
    }
    regressions.clear();
    for (const auto& c : cur["results"]) {
        for (const auto& b : base["results"]) {
            if (b.value("name", "") != c.value("name", "")) continue;
            const double was = b.value("normalized", 0.0), now = c.value("normalized", 0.0);
            if (was > 0.0 && now > was * tolerance) regressions.push_back({c.value("name", ""), was, now, now / was});
        }
    }
    return true;
}

} // namespace aether::bench
