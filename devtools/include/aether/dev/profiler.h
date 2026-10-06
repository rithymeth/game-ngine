#pragma once

#include "aether/core/profile.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace aether::dev {

// The CPU profiler (Phase 23 step 1). It records the zones the engine and the
// game mark with AETHER_ZONE (core/profile.h), from any thread, between
// BeginFrame and EndFrame, and keeps a history of frames: when each one
// started, how long it took, every zone with its thread, depth and time, and
// the counters set during it (draw calls, triangles, ...). From that it works
// out per-zone totals - inclusive time, self time (inclusive minus the zones
// nested directly inside), count and worst case - the data behind the
// editor's profiler panel. It also keeps memory by category.
//
// Install() makes this profiler the target of the engine's hooks; nothing is
// recorded while disabled or outside a frame, and uninstalled the hooks cost
// the engine one branch. A Tracy adapter would install its own Hooks instead.

struct ZoneRecord {
    u32 name = 0;   // index into the profiler's name table
    u32 thread = 0; // small per-profiler thread index
    u32 depth = 0;  // nesting on its thread
    u64 start_ns = 0;
    u64 end_ns = 0;
    u64 DurationNs() const { return end_ns - start_ns; }
};

struct FrameProfile {
    u64 index = 0;
    u64 start_ns = 0;
    u64 end_ns = 0;
    std::vector<ZoneRecord> zones;
    std::map<std::string, f64> counters;
    f64 DurationMs() const { return static_cast<f64>(end_ns - start_ns) / 1.0e6; }
};

struct ZoneStat {
    std::string name;
    f64 inclusive_ms = 0.0; // every occurrence's full duration, summed
    f64 self_ms = 0.0;      // minus the zones directly inside it
    f64 max_ms = 0.0;       // the longest single occurrence
    u32 count = 0;
};

struct MemoryCategory {
    std::string name;
    i64 bytes = 0;
    i64 peak = 0;
};

class Profiler {
public:
    explicit Profiler(usize history_frames = 300);
    ~Profiler();
    Profiler(const Profiler&) = delete;
    Profiler& operator=(const Profiler&) = delete;

    // Becomes the engine's hook target; Uninstall (or destruction) detaches.
    void Install();
    void Uninstall();
    bool Installed() const;

    void SetEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }
    bool Enabled() const { return enabled_.load(std::memory_order_relaxed); }
    // Replaces the nanosecond clock (a fake one makes tests exact).
    void SetClock(std::function<u64()> clock);
    void SetHistory(usize frames);

    void BeginFrame();
    void EndFrame();
    bool InFrame() const { return in_frame_.load(std::memory_order_relaxed); }

    // Direct (macro-free) zone use on the calling thread.
    void BeginZone(const char* name, bool dynamic_name = false);
    void EndZone();

    // Per-frame values, stored with the frame that is open and cleared at its end.
    void SetCounter(const std::string& name, f64 value);
    void AddCounter(const std::string& name, f64 delta);

    // History: Frame(0) is the latest finished frame, Frame(1) the one before.
    usize FrameCount() const { return frames_.size(); }
    const FrameProfile* Frame(usize back = 0) const;
    std::string NameOf(u32 name_id) const;
    // Per-zone totals for a frame, costliest (inclusive) first.
    std::vector<ZoneStat> Stats(usize back = 0) const;

    // Frame time series, oldest first, for the last `count` frames.
    std::vector<f32> FrameTimesMs(usize count) const;
    f64 AverageFrameMs(usize count) const;
    // p in [0, 1]; 0.99 is the slow-frame mark.
    f64 PercentileFrameMs(f64 p, usize count) const;
    f64 Fps(usize count = 60) const;

    // Memory by category (also fed by the engine's allocators through the hooks).
    void AddMemory(const std::string& category, i64 delta_bytes);
    std::vector<MemoryCategory> Memory() const;
    i64 TotalMemory() const;

private:
    struct ThreadBuffer;
    struct Open {
        u32 name;
        u64 start;
    };
    ThreadBuffer& Buffer();
    u32 Intern(const char* name, bool dynamic);
    u64 Now() const;

    static void HookBegin(const char* name, bool dynamic);
    static void HookEnd();
    static void HookMemory(const char* category, i64 delta);

    std::atomic<bool> enabled_{true};
    std::atomic<bool> in_frame_{false};
    u64 generation_;
    std::function<u64()> clock_;
    usize history_;
    u64 frame_index_ = 0;
    u64 frame_start_ = 0;

    mutable std::mutex names_mutex_;
    std::vector<std::string> names_;
    std::unordered_map<std::string, u32> name_ids_;
    std::unordered_map<const char*, u32> static_name_ids_;

    mutable std::mutex buffers_mutex_;
    std::vector<std::shared_ptr<ThreadBuffer>> buffers_;

    mutable std::mutex counters_mutex_;
    std::map<std::string, f64> counters_;

    mutable std::mutex memory_mutex_;
    std::map<std::string, MemoryCategory> memory_;

    std::vector<FrameProfile> frames_; // oldest first
    prof::Hooks hooks_;
};

} // namespace aether::dev
