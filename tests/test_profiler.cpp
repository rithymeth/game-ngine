#include "aether/core/console.h"
#include "aether/core/profiler.h"
#include "aether/job/job_system.h"
#include "aether/memory/frame_allocator.h"
#include "aether/memory/linear_allocator.h"
#include "aether/memory/pool_allocator.h"
#include "aether/scene/scheduler.h"
#include "test_framework.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>

// Phase 23 step 2: the profiler - zones, nesting and self time, frames,
// counters and gauges, GPU timings, threads, enable/pause/history, the
// scheduler's and job system's zones, Chrome trace export, memory by
// category from the allocators, and the console's profiler commands.

using namespace aether;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

void Spin(u64 microseconds) {
    const u64 until = Profiler::NowNs() + microseconds * 1000;
    while (Profiler::NowNs() < until) {
    }
}

const ProfileZone* Find(const ProfileFrame& f, std::string_view name) {
    for (const ProfileZone& z : f.zones)
        if (name == z.name) return &z;
    return nullptr;
}

const ZoneStat* Stat(const std::vector<ZoneStat>& stats, std::string_view name) {
    for (const ZoneStat& s : stats)
        if (s.name == name) return &s;
    return nullptr;
}

// Leaves the global profiler as it was found.
struct ProfilerReset {
    ProfilerReset() {
        Profiler& p = Profiler::Get();
        p.SetEnabled(true), p.SetPaused(false), p.history = 300;
        p.EndFrame(); // flush zones from earlier tests
        p.Clear();
    }
    ~ProfilerReset() {
        Profiler& p = Profiler::Get();
        p.SetEnabled(true), p.SetPaused(false), p.history = 300;
        p.Clear();
    }
};

} // namespace

AETHER_TEST(Profiler_ZonesFramesAndSelfTime) {
    ProfilerReset reset;
    Profiler& p = Profiler::Get();
    p.BeginFrame();
    {
        AETHER_PROFILE_ZONE("Test.Outer");
        Spin(1500);
        {
            AETHER_PROFILE_ZONE("Test.Inner");
            Spin(1000);
        }
        {
            AETHER_PROFILE_ZONE("Test.Inner");
            Spin(500);
        }
    }
    p.Count("Test.DrawCalls", 3);
    p.Count("Test.DrawCalls", 4);
    p.Gauge("Test.Entities", 12);
    p.SubmitGpuTimings({{"Shadows", 1.5}, {"Lighting", 2.0}});
    p.EndFrame();

    ProfileFrame f;
    CHECK(p.Latest(f) && f.Ms() >= 3.0);
    const ProfileZone* outer = Find(f, "Test.Outer");
    const ProfileZone* inner = Find(f, "Test.Inner");
    CHECK(outer && inner && outer->depth == 0 && inner->depth == 1 && inner->thread == outer->thread);
    CHECK(inner->start_ns >= outer->start_ns && inner->end_ns <= outer->end_ns);
    CHECK(f.Counter("Test.DrawCalls") == 7.0 && f.Counter("Test.Entities") == 12.0 && f.Counter("nope", -1) == -1);
    CHECK(f.gpu.size() == 2 && f.gpu[1].name == "Lighting");
    // Totals and self time.
    const std::vector<ZoneStat> stats = Profiler::Aggregate(std::span<const ProfileFrame>(&f, 1));
    const ZoneStat* o = Stat(stats, "Test.Outer");
    const ZoneStat* in = Stat(stats, "Test.Inner");
    CHECK(o && in && o->calls == 1 && in->calls == 2);
    CHECK(o->total_ms >= 3.0 && in->total_ms >= 1.5 && in->self_ms == in->total_ms);
    CHECK(std::abs(o->self_ms - (o->total_ms - in->total_ms)) < 1e-6 && o->self_ms >= 1.5);
    CHECK(in->max_ms >= 1.0 && stats[0].name == "Test.Outer"); // by total

    // The next frame: counts reset, gauges stay, GPU timings are per frame.
    p.BeginFrame();
    p.EndFrame();
    CHECK(p.Latest(f) && f.Counter("Test.DrawCalls", -1) == -1 && f.Counter("Test.Entities") == 12 && f.gpu.empty());
    CHECK(p.Frames().size() == 2 && p.Frames()[1].index == p.Frames()[0].index + 1);

    // Off, paused, history.
    p.SetEnabled(false);
    p.BeginFrame();
    { AETHER_PROFILE_ZONE("Test.Unseen"); }
    p.EndFrame();
    CHECK(p.Latest(f) && Find(f, "Test.Unseen") == nullptr);
    p.SetEnabled(true);
    p.SetPaused(true);
    p.BeginFrame();
    p.EndFrame();
    CHECK(p.Frames().size() == 3);
    p.SetPaused(false);
    p.history = 4;
    for (int i = 0; i < 10; ++i) p.BeginFrame(), p.EndFrame();
    CHECK(p.Frames().size() == 4);
    // Interned names are stable.
    const char* a = p.Intern("Test.Dynamic");
    CHECK(a == p.Intern(std::string("Test.") + "Dynamic") && std::string(a) == "Test.Dynamic");
}

AETHER_TEST(Profiler_ThreadsSchedulerAndJobs) {
    ProfilerReset reset;
    Profiler& p = Profiler::Get();
    p.BeginFrame();
    std::thread t([] {
        Profiler::Get().SetThreadName("Test Loader");
        AETHER_PROFILE_ZONE("Test.Load");
        Spin(200);
    });
    t.join();
    { AETHER_PROFILE_ZONE("Test.Main"); }
    p.EndFrame();
    ProfileFrame f;
    CHECK(p.Latest(f));
    const ProfileZone* load = Find(f, "Test.Load");
    const ProfileZone* main_zone = Find(f, "Test.Main");
    CHECK(load && main_zone && load->thread != main_zone->thread && p.ThreadName(load->thread) == "Test Loader");

    // Systems and phases are zoned by the scheduler; workers are named.
    SystemScheduler scheduler;
    // Movement and Ai share a level and wait for each other, so (the main
    // thread running one job at a time) one of them runs on a worker.
    std::atomic<int> started{0};
    for (const char* name : {"Test.Movement", "Test.Ai", "Test.Render"}) {
        SystemDesc d;
        d.name = name;
        d.main_thread_only = std::string(name) == "Test.Render";
        d.run = [&started, render = d.main_thread_only](World&, const FrameContext&) {
            if (render) return Spin(100);
            ++started;
            const u64 give_up = Profiler::NowNs() + 2'000'000'000ull;
            while (started.load() < 2 && Profiler::NowNs() < give_up) {
            }
        };
        CHECK(scheduler.Add(d));
    }
    World world;
    JobSystem jobs(2);
    p.BeginFrame();
    scheduler.RunPhase(world, SystemPhase::Update, FrameContext{}, &jobs);
    scheduler.RunPhase(world, SystemPhase::LateUpdate, FrameContext{}); // without jobs
    p.EndFrame();
    CHECK(p.Latest(f));
    for (const char* name : {"Test.Movement", "Test.Ai", "Test.Render", "Update", "LateUpdate"}) CHECK(Find(f, name) != nullptr);
    CHECK(Find(f, "Test.Render")->depth == Find(f, "Update")->depth + 1); // inside the phase, on the main thread
    bool on_worker = false;
    for (const ProfileZone& z : f.zones)
        on_worker = on_worker || p.ThreadName(z.thread).rfind("Worker ", 0) == 0;
    CHECK(on_worker); // the workers named themselves and some system ran on one
}

AETHER_TEST(Profiler_ChromeTraceMemoryAndConsole) {
    ProfilerReset reset;
    Profiler& p = Profiler::Get();
    for (int i = 0; i < 3; ++i) {
        p.BeginFrame();
        { AETHER_PROFILE_ZONE("Test.Trace"); }
        p.Count("Test.Triangles", 1000);
        p.EndFrame();
    }
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_test_profiler";
    std::filesystem::create_directories(dir);
    const std::string path = (dir / "trace.json").string();
    const std::vector<ProfileFrame> frames = p.Frames();
    CHECK(p.WriteChromeTrace(path, frames));
    std::ifstream in(path);
    const nlohmann::json trace = nlohmann::json::parse(in);
    usize zones = 0, counters = 0, names = 0, frame_marks = 0;
    bool durations_ok = true, values_ok = true;
    for (const auto& e : trace["traceEvents"]) {
        if (e["ph"] == "X" && e["name"] == "Test.Trace") ++zones, durations_ok = durations_ok && e["dur"].get<double>() >= 0.0;
        if (e["ph"] == "X" && e["tid"] == 1000) ++frame_marks;
        if (e["ph"] == "C" && e["name"] == "Test.Triangles") ++counters, values_ok = values_ok && e["args"]["value"] == 1000.0;
        if (e["ph"] == "M") ++names;
    }
    CHECK(zones == 3 && counters == 3 && frame_marks == 3 && names >= 2 && durations_ok && values_ok);
    std::string error;
    CHECK(!p.WriteChromeTrace((dir / "no" / "such" / "dir.json").string(), frames, &error) && !error.empty());

    // Memory by category from the allocators.
    std::vector<u8> block(1024);
    LinearAllocator linear(block.data(), block.size());
    linear.SetMemoryCategory("Test.Linear");
    CHECK(linear.Allocate(100, 16) && linear.Allocate(28, 16));
    CHECK(MemoryTracker::Get().Bytes("Test.Linear") == static_cast<i64>(linear.Used()) && linear.Used() >= 128);
    linear.Reset();
    CHECK(MemoryTracker::Get().Bytes("Test.Linear") == 0);
    PoolAllocator pool(block.data(), block.size(), 64);
    pool.SetMemoryCategory("Test.Pool");
    void* x = pool.Allocate();
    void* y = pool.Allocate();
    CHECK(MemoryTracker::Get().Bytes("Test.Pool") == 128);
    pool.Free(x);
    pool.Free(y);
    const std::vector<MemoryCategoryStats> cats = MemoryTracker::Get().Categories();
    auto pool_stats = std::find_if(cats.begin(), cats.end(), [](const MemoryCategoryStats& c) { return c.name == "Test.Pool"; });
    CHECK(pool_stats != cats.end() && pool_stats->bytes == 0 && pool_stats->peak == 128 && pool_stats->allocations == 2 && pool_stats->frees == 2);
    const i64 before = MemoryTracker::Get().Bytes("Frame allocator (reserved)");
    {
        FrameAllocator frame(4096);
        CHECK(MemoryTracker::Get().Bytes("Frame allocator (reserved)") == before + 8192);
    }
    CHECK(MemoryTracker::Get().Bytes("Frame allocator (reserved)") == before);

    // The console drives it.
    Console console;
    CHECK(console.Execute("profiler.enabled 0") && !p.Enabled());
    CHECK(console.Execute("profiler.enabled 1") && p.Enabled());
    CHECK(console.Execute("profiler.history 5") && p.history == 5);
    CHECK(console.Execute("profiler.pause") && p.Paused() && console.Execute("profiler.pause") && !p.Paused());
    const std::string dumped = (dir / "dump.json").string();
    CHECK(console.Execute("profiler.dump " + dumped) && std::filesystem::exists(dumped));
    CHECK(console.Execute("profiler.clear") && p.Frames().empty());
    console.Execute("reset profiler.history");
    std::filesystem::remove_all(dir);
}
