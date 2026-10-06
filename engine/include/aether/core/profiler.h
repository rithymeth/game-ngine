#pragma once

#include "aether/core/base.h"

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#ifdef AETHER_TRACY
#include <tracy/Tracy.hpp>
#endif

namespace aether {

// The profiler (Phase 23 step 2, docs/design/PHASE_SPECS.md §23.2).
//
// Zones are scoped timers (AETHER_PROFILE_ZONE("Physics")) recorded per
// thread without locking; EndFrame gathers every thread's zones since the
// last frame, the frame's counters and GPU timings into a ProfileFrame and
// keeps the last `history` frames for the profiler panel. Aggregate turns
// frames into per-zone totals with self time; WriteChromeTrace exports
// them for chrome://tracing or Perfetto. Built with AETHER_TRACY, the same
// macros also feed Tracy.

struct ProfileZone {
    const char* name = ""; // static or interned (Profiler::Intern)
    u64 start_ns = 0, end_ns = 0;
    u32 thread = 0;
    u16 depth = 0; // nesting on its thread, 0 = outermost
    f64 Ms() const { return static_cast<f64>(end_ns - start_ns) / 1e6; }
};

struct GpuTiming {
    std::string name; // a render pass
    f64 ms = 0.0;
};

struct ProfileFrame {
    u64 index = 0;
    u64 start_ns = 0, end_ns = 0;
    std::vector<ProfileZone> zones;
    std::vector<std::pair<std::string, f64>> counters; // per-frame counts and gauges, by name
    std::vector<GpuTiming> gpu;
    f64 Ms() const { return static_cast<f64>(end_ns - start_ns) / 1e6; }
    f64 Counter(std::string_view name, f64 fallback = 0.0) const;
};

struct ZoneStat {
    std::string name;
    u32 calls = 0;
    f64 total_ms = 0.0, self_ms = 0.0, max_ms = 0.0; // over the frames given
    f64 per_frame_ms = 0.0;                           // total / frames
};

// Bytes by category (Phase 23 step 2): allocators and systems report what
// they take and give back; the profiler panel shows current, peak and count.
struct MemoryCategoryStats {
    std::string name;
    i64 bytes = 0, peak = 0;
    u64 allocations = 0, frees = 0;
};

class MemoryTracker {
public:
    static MemoryTracker& Get();
    void Alloc(const char* category, usize bytes);
    void Free(const char* category, usize bytes);
    std::vector<MemoryCategoryStats> Categories() const; // by name
    i64 Bytes(std::string_view category) const;
    void Reset(); // forget everything (tests)

private:
    mutable std::mutex mutex_;
    std::map<std::string, MemoryCategoryStats, std::less<>> categories_;
};

class Profiler {
public:
    static Profiler& Get();

    void SetEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }
    bool Enabled() const { return enabled_.load(std::memory_order_relaxed); }
    // While paused, frames still run but aren't kept (so the panel can be studied).
    void SetPaused(bool paused) { paused_ = paused; }
    bool Paused() const { return paused_; }
    usize history = 300;

    // Zones on the calling thread. Prefer AETHER_PROFILE_ZONE.
    bool BeginZone(const char* name);
    void EndZone();

    // The frame boundary, on the main thread.
    void BeginFrame();
    void EndFrame();
    u64 FrameIndex() const { return frame_index_; }

    // Per-frame counts (summed over the frame, then reset): draw calls,
    // triangles. Gauges keep their last value: entities, bodies.
    void Count(const char* name, f64 delta = 1.0);
    void Gauge(const char* name, f64 value);
    // The GPU's pass times for the frame being recorded (from the render backend's queries).
    void SubmitGpuTimings(std::vector<GpuTiming> timings);

    // A stable name pointer for zones named at runtime (a system, an asset).
    const char* Intern(std::string_view name);
    void SetThreadName(std::string_view name); // the calling thread's
    std::string ThreadName(u32 thread) const;

    std::vector<ProfileFrame> Frames() const; // oldest first
    bool Latest(ProfileFrame& out) const;
    void Clear();

    static u64 NowNs();
    static std::vector<ZoneStat> Aggregate(std::span<const ProfileFrame> frames); // by total time
    bool WriteChromeTrace(const std::string& path, std::span<const ProfileFrame> frames, std::string* error = nullptr) const;

private:
    struct ThreadBuffer;
    ThreadBuffer& Thread();

    std::atomic<bool> enabled_{true};
    bool paused_ = false;
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<ThreadBuffer>> threads_;
    std::deque<ProfileFrame> frames_;
    std::unordered_set<std::string> interned_;
    std::map<std::string, f64, std::less<>> counts_, gauges_;
    std::vector<GpuTiming> gpu_;
    u64 frame_index_ = 0, frame_start_ = 0;
    bool in_frame_ = false;
};

// Times its scope on the calling thread.
class ScopedZone {
public:
    explicit ScopedZone(const char* name) : active_(Profiler::Get().BeginZone(name)) {}
    ~ScopedZone() {
        if (active_) Profiler::Get().EndZone();
    }
    ScopedZone(const ScopedZone&) = delete;
    ScopedZone& operator=(const ScopedZone&) = delete;

private:
    bool active_;
};

} // namespace aether

#define AETHER_PROFILE_CONCAT_(a, b) a##b
#define AETHER_PROFILE_CONCAT(a, b) AETHER_PROFILE_CONCAT_(a, b)

#ifdef AETHER_TRACY
#define AETHER_PROFILE_ZONE(name)                                                                  \
    ZoneScoped;                                                                                    \
    ZoneName(name, std::char_traits<char>::length(name));                                          \
    ::aether::ScopedZone AETHER_PROFILE_CONCAT(aether_zone_, __LINE__)(name)
#define AETHER_PROFILE_FRAME_END() FrameMark
#else
#define AETHER_PROFILE_ZONE(name) ::aether::ScopedZone AETHER_PROFILE_CONCAT(aether_zone_, __LINE__)(name)
#define AETHER_PROFILE_FRAME_END() ((void)0)
#endif
#define AETHER_PROFILE_FUNCTION() AETHER_PROFILE_ZONE(__func__)
