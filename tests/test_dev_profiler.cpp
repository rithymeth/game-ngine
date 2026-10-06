#include "aether/core/profile.h"
#include "aether/dev/profiler.h"
#include "test_framework.h"

#include <string>

// Phase 23 step 1: the CPU profiler. Exercises zone capture, self/inclusive
// timing, counters, history/percentiles, memory categories and the
// install/uninstall hook lifecycle, with a fake clock so the numbers are
// exact.

using namespace aether;
using namespace aether::dev;

namespace {

constexpr u64 kNsPerMs = 1'000'000ull;

struct Clock {
    Profiler& profiler;
    u64 t = 0;
    explicit Clock(Profiler& p) : profiler(p) { profiler.SetClock([this] { return t; }); }
    void Advance(u64 dt_ns) { t += dt_ns; }
    void Frame(u64 dt_ns) {
        profiler.BeginFrame();
        t += dt_ns;
        profiler.EndFrame();
    }
};

const ZoneStat* FindStat(const std::vector<ZoneStat>& stats, const std::string& name) {
    for (const ZoneStat& s : stats) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

} // namespace

AETHER_TEST(DevProfiler_NestedZonesMeasureSelfAndInclusive) {
    Profiler p(10);
    Clock clk(p);

    p.BeginFrame();
    p.BeginZone("A");
    clk.Advance(20 * kNsPerMs / 1000); // 20 us
    p.BeginZone("B");
    clk.Advance(5 * kNsPerMs / 1000); // 5 us
    p.EndZone();                       // B ends
    p.EndZone();                       // A ends (A total 25 us, self 20 us)
    p.EndFrame();

    const std::vector<ZoneStat> stats = p.Stats();
    AETHER_CHECK(stats.size() == 2);
    const ZoneStat* a = FindStat(stats, "A");
    const ZoneStat* b = FindStat(stats, "B");
    AETHER_CHECK(a != nullptr);
    AETHER_CHECK(b != nullptr);
    if (a == nullptr || b == nullptr) return;

    AETHER_CHECK_NEAR(a->inclusive_ms, 25.0 / 1000.0, 1e-9);
    AETHER_CHECK_NEAR(a->self_ms, 20.0 / 1000.0, 1e-9);
    AETHER_CHECK_NEAR(a->max_ms, 25.0 / 1000.0, 1e-9);
    AETHER_CHECK_NEAR(b->inclusive_ms, 5.0 / 1000.0, 1e-9);
    AETHER_CHECK_NEAR(b->max_ms, 5.0 / 1000.0, 1e-9);
    AETHER_CHECK(a->count == 1);
    AETHER_CHECK(b->count == 1);
    AETHER_CHECK(stats[0].name == "A"); // costliest first
    p.Uninstall();
}

AETHER_TEST(DevProfiler_ZonesDisabledOrOutsideFrameAreNotRecorded) {
    Profiler p(10);
    Clock clk(p);

    p.BeginZone("orphan"); // outside a frame
    p.EndZone();

    p.BeginFrame();
    p.SetEnabled(false);
    p.BeginZone("hidden");
    p.EndZone();
    p.SetEnabled(true);
    p.EndFrame();

    AETHER_CHECK(p.FrameCount() == 1);
    AETHER_CHECK(p.Frame(0) != nullptr);
    AETHER_CHECK(p.Frame(0)->zones.empty());
    AETHER_CHECK(p.Stats().empty());
    p.Uninstall();
}

AETHER_TEST(DevProfiler_CountersAccumulateAndResetEachFrame) {
    Profiler p(10);
    Clock clk(p);

    p.BeginFrame();
    p.SetCounter("draw_calls", 42.0);
    p.AddCounter("draw_calls", 3.0);
    p.AddCounter("triangles", 1200.0);
    p.EndFrame();

    const FrameProfile* f = p.Frame(0);
    AETHER_CHECK(f != nullptr);
    AETHER_CHECK(f->counters.size() == 2);
    auto draw = f->counters.find("draw_calls");
    auto tris = f->counters.find("triangles");
    AETHER_CHECK(draw != f->counters.end());
    AETHER_CHECK(tris != f->counters.end());
    if (draw != f->counters.end()) AETHER_CHECK_NEAR(draw->second, 45.0, 1e-9);
    if (tris != f->counters.end()) AETHER_CHECK_NEAR(tris->second, 1200.0, 1e-9);

    p.BeginFrame();
    p.EndFrame();
    AETHER_CHECK(p.Frame(0)->counters.empty());
    p.Uninstall();
}

AETHER_TEST(DevProfiler_HistoryAndPercentiles) {
    Profiler p(10);
    Clock clk(p);

    clk.Frame(10 * kNsPerMs); // index 0: 10 ms
    clk.Frame(20 * kNsPerMs); // index 1: 20 ms
    clk.Frame(30 * kNsPerMs); // index 2: 30 ms

    AETHER_CHECK(p.FrameCount() == 3);
    AETHER_CHECK(p.Frame(0)->index == 2);
    AETHER_CHECK(p.Frame(1)->index == 1);
    AETHER_CHECK(p.Frame(2) != nullptr);   // two back is still valid with 3 frames
    AETHER_CHECK(p.Frame(3) == nullptr);   // three back exceeds the history

    AETHER_CHECK_NEAR(p.AverageFrameMs(10), 20.0, 1e-6);
    AETHER_CHECK_NEAR(p.PercentileFrameMs(0.99, 10), 30.0, 1e-6);
    AETHER_CHECK_NEAR(p.Fps(10), 1000.0 / 20.0, 1e-6);

    p.SetHistory(2);
    AETHER_CHECK(p.FrameCount() == 2);
    AETHER_CHECK(p.Frame(0)->index == 2);
    AETHER_CHECK(p.Frame(1)->index == 1);
    const std::vector<f32> series = p.FrameTimesMs(10);
    AETHER_CHECK(series.size() == 2);
    AETHER_CHECK_NEAR(series[0], 20.0, 1e-6);
    AETHER_CHECK_NEAR(series[1], 30.0, 1e-6);
    p.Uninstall();
}

AETHER_TEST(DevProfiler_MemoryTracksCurrentAndPeak) {
    Profiler p(10);
    p.AddMemory("Frame allocator", 100);
    p.AddMemory("Frame allocator", 100);
    p.AddMemory("Frame allocator", -50);
    p.AddMemory("Assets", 200);

    AETHER_CHECK(p.TotalMemory() == 350);
    const std::vector<MemoryCategory> mem = p.Memory();
    AETHER_CHECK(mem.size() == 2);
    AETHER_CHECK(mem[0].name == "Assets"); // 200
    AETHER_CHECK(mem[0].bytes == 200);
    AETHER_CHECK(mem[0].peak == 200);
    AETHER_CHECK(mem[1].name == "Frame allocator"); // 150
    AETHER_CHECK(mem[1].bytes == 150);
    AETHER_CHECK(mem[1].peak == 200);
    p.Uninstall();
}

AETHER_TEST(DevProfiler_HooksInstallAndUninstallCleanly) {
    AETHER_CHECK(prof::CurrentHooks() == nullptr);
    {
        Profiler p(10);
        Clock clk(p);
        p.Install();
        AETHER_CHECK(p.Installed());
        AETHER_CHECK(prof::CurrentHooks() != nullptr);

        p.BeginFrame();
        { AETHER_ZONE("MacroZone"); clk.Advance(kNsPerMs / 1000); }
        p.EndFrame();

        const ZoneStat* z = FindStat(p.Stats(), "MacroZone");
        AETHER_CHECK(z != nullptr);
        if (z != nullptr) AETHER_CHECK_NEAR(z->self_ms, 0.001, 1e-6);

        p.Uninstall();
        AETHER_CHECK(!p.Installed());
        AETHER_CHECK(prof::CurrentHooks() == nullptr);
    }
    AETHER_CHECK(prof::CurrentHooks() == nullptr);
}

AETHER_TEST(DevProfiler_DynamicZoneNamesByContent) {
    Profiler p(10);
    Clock clk(p);

    p.BeginFrame();
    std::string name = "System." + std::to_string(3);
    p.BeginZone(name.c_str(), /*dynamic_name=*/true);
    clk.Advance(kNsPerMs / 1000);
    p.EndZone();
    p.EndFrame();

    const ZoneStat* z = FindStat(p.Stats(), "System.3");
    AETHER_CHECK(z != nullptr);
    AETHER_CHECK_NEAR(z->inclusive_ms, 0.001, 1e-6);
    p.Uninstall();
}
