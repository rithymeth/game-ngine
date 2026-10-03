#include "aether/core/profiler.h"

#include "aether/core/console.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>

namespace aether {

// ---------------------------------------------------------------- console

namespace {
AutoCVar<bool> g_profiler_enabled("profiler.enabled", true, "Record profiler zones");
const int g_profiler_enabled_hook =
    g_profiler_enabled.Raw().OnChange([](const CVar& v) { Profiler::Get().SetEnabled(v.GetBool()); });
AutoCVar<int> g_profiler_history("profiler.history", 300, "Frames the profiler keeps", CVar_Archive, 1, 10000);
const int g_profiler_history_hook =
    g_profiler_history.Raw().OnChange([](const CVar& v) { Profiler::Get().history = static_cast<usize>(v.GetInt()); });
AutoConsoleCommand g_profiler_pause("profiler.pause", "Stops (or resumes) keeping frames",
                                    [](const std::vector<std::string>&, Console& c) {
                                        Profiler::Get().SetPaused(!Profiler::Get().Paused());
                                        c.Print(Profiler::Get().Paused() ? "profiler paused" : "profiler running");
                                    });
AutoConsoleCommand g_profiler_clear("profiler.clear", "Forgets the kept frames",
                                    [](const std::vector<std::string>&, Console&) { Profiler::Get().Clear(); });
AutoConsoleCommand g_profiler_dump(
    "profiler.dump", "Writes the kept frames as a Chrome trace (chrome://tracing, Perfetto): profiler.dump [file]",
    [](const std::vector<std::string>& args, Console& c) {
        const std::string path = args.empty() ? "profile.json" : args[0];
        const std::vector<ProfileFrame> frames = Profiler::Get().Frames();
        std::string error;
        if (Profiler::Get().WriteChromeTrace(path, frames, &error)) c.Print("wrote " + std::to_string(frames.size()) + " frames to " + path);
        else c.Print(error, LogLevel::Error);
    });
} // namespace

// ---------------------------------------------------------------- memory

MemoryTracker& MemoryTracker::Get() {
    static MemoryTracker tracker;
    return tracker;
}

void MemoryTracker::Alloc(const char* category, usize bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = categories_.find(std::string_view(category));
    if (it == categories_.end()) it = categories_.emplace(category, MemoryCategoryStats{category}).first;
    it->second.bytes += static_cast<i64>(bytes);
    it->second.peak = std::max(it->second.peak, it->second.bytes);
    ++it->second.allocations;
}

void MemoryTracker::Free(const char* category, usize bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = categories_.find(std::string_view(category));
    if (it == categories_.end()) it = categories_.emplace(category, MemoryCategoryStats{category}).first;
    it->second.bytes -= static_cast<i64>(bytes);
    ++it->second.frees;
}

std::vector<MemoryCategoryStats> MemoryTracker::Categories() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<MemoryCategoryStats> out;
    for (const auto& [name, stats] : categories_) out.push_back(stats);
    return out;
}

i64 MemoryTracker::Bytes(std::string_view category) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = categories_.find(category);
    return it == categories_.end() ? 0 : it->second.bytes;
}

void MemoryTracker::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    categories_.clear();
}

// ---------------------------------------------------------------- profiler

f64 ProfileFrame::Counter(std::string_view name, f64 fallback) const {
    for (const auto& [n, v] : counters)
        if (n == name) return v;
    return fallback;
}

struct Profiler::ThreadBuffer {
    u32 id = 0;
    std::string name;
    std::mutex mutex;                 // zones (EndFrame takes them)
    std::vector<ProfileZone> zones;
    std::vector<ProfileZone> stack;   // open zones: this thread only
};

Profiler& Profiler::Get() {
    static Profiler profiler;
    return profiler;
}

u64 Profiler::NowNs() {
    static const auto start = std::chrono::steady_clock::now();
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
}

Profiler::ThreadBuffer& Profiler::Thread() {
    thread_local std::shared_ptr<ThreadBuffer> buffer;
    if (!buffer) {
        buffer = std::make_shared<ThreadBuffer>();
        std::lock_guard<std::mutex> lock(mutex_);
        buffer->id = static_cast<u32>(threads_.size());
        buffer->name = buffer->id == 0 ? "Main" : "Thread " + std::to_string(buffer->id);
        threads_.push_back(buffer);
    }
    return *buffer;
}

bool Profiler::BeginZone(const char* name) {
    if (!Enabled()) return false;
    ThreadBuffer& t = Thread();
    ProfileZone z;
    z.name = name;
    z.thread = t.id;
    z.depth = static_cast<u16>(t.stack.size());
    z.start_ns = NowNs();
    t.stack.push_back(z);
    return true;
}

void Profiler::EndZone() {
    ThreadBuffer& t = Thread();
    if (t.stack.empty()) return;
    ProfileZone z = t.stack.back();
    t.stack.pop_back();
    z.end_ns = NowNs();
    std::lock_guard<std::mutex> lock(t.mutex);
    t.zones.push_back(z);
    if (t.zones.size() > 1'000'000) t.zones.erase(t.zones.begin(), t.zones.begin() + 500'000); // no frames ending
}

void Profiler::BeginFrame() {
    std::lock_guard<std::mutex> lock(mutex_);
    frame_start_ = NowNs();
    in_frame_ = true;
}

void Profiler::EndFrame() {
    ProfileFrame frame;
    std::vector<std::shared_ptr<ThreadBuffer>> threads;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        frame.index = frame_index_++;
        frame.start_ns = in_frame_ ? frame_start_ : (frames_.empty() ? 0 : frames_.back().end_ns);
        frame.end_ns = NowNs();
        in_frame_ = false;
        for (const auto& [name, v] : counts_) frame.counters.emplace_back(name, v);
        for (const auto& [name, v] : gauges_) frame.counters.emplace_back(name, v);
        std::sort(frame.counters.begin(), frame.counters.end());
        counts_.clear();
        frame.gpu = std::move(gpu_);
        gpu_.clear();
        threads = threads_;
    }
    for (const auto& t : threads) {
        std::lock_guard<std::mutex> lock(t->mutex);
        frame.zones.insert(frame.zones.end(), t->zones.begin(), t->zones.end());
        t->zones.clear();
    }
    std::sort(frame.zones.begin(), frame.zones.end(), [](const ProfileZone& a, const ProfileZone& b) {
        return a.thread != b.thread ? a.thread < b.thread : a.start_ns != b.start_ns ? a.start_ns < b.start_ns : a.depth < b.depth;
    });
    std::lock_guard<std::mutex> lock(mutex_);
    if (paused_) return;
    frames_.push_back(std::move(frame));
    while (frames_.size() > std::max<usize>(history, 1)) frames_.pop_front();
}

void Profiler::Count(const char* name, f64 delta) {
    std::lock_guard<std::mutex> lock(mutex_);
    counts_[name] += delta;
}

void Profiler::Gauge(const char* name, f64 value) {
    std::lock_guard<std::mutex> lock(mutex_);
    gauges_[name] = value;
}

void Profiler::SubmitGpuTimings(std::vector<GpuTiming> timings) {
    std::lock_guard<std::mutex> lock(mutex_);
    gpu_ = std::move(timings);
}

const char* Profiler::Intern(std::string_view name) {
    std::lock_guard<std::mutex> lock(mutex_);
    return interned_.emplace(name).first->c_str(); // node-based: the pointer stays valid
}

void Profiler::SetThreadName(std::string_view name) {
    ThreadBuffer& t = Thread();
    std::lock_guard<std::mutex> lock(mutex_);
    t.name = std::string(name);
}

std::string Profiler::ThreadName(u32 thread) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return thread < threads_.size() ? threads_[thread]->name : "Thread " + std::to_string(thread);
}

std::vector<ProfileFrame> Profiler::Frames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<ProfileFrame>(frames_.begin(), frames_.end());
}

bool Profiler::Latest(ProfileFrame& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (frames_.empty()) return false;
    out = frames_.back();
    return true;
}

void Profiler::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    frames_.clear();
    counts_.clear();
    gauges_.clear();
    gpu_.clear();
}

std::vector<ZoneStat> Profiler::Aggregate(std::span<const ProfileFrame> frames) {
    std::map<std::string, ZoneStat> stats;
    for (const ProfileFrame& frame : frames) {
        // Self time: a zone's time minus its direct children's (zones are sorted by thread, then start).
        std::vector<f64> child_ms(frame.zones.size(), 0.0);
        std::vector<usize> stack;
        u32 thread = ~0u;
        for (usize i = 0; i < frame.zones.size(); ++i) {
            const ProfileZone& z = frame.zones[i];
            if (z.thread != thread) stack.clear(), thread = z.thread;
            while (!stack.empty() && frame.zones[stack.back()].end_ns <= z.start_ns) stack.pop_back();
            if (!stack.empty() && frame.zones[stack.back()].depth < z.depth) child_ms[stack.back()] += z.Ms();
            stack.push_back(i);
        }
        for (usize i = 0; i < frame.zones.size(); ++i) {
            const ProfileZone& z = frame.zones[i];
            ZoneStat& s = stats[z.name];
            s.name = z.name;
            ++s.calls;
            s.total_ms += z.Ms();
            s.self_ms += std::max(0.0, z.Ms() - child_ms[i]);
            s.max_ms = std::max(s.max_ms, z.Ms());
        }
    }
    std::vector<ZoneStat> out;
    for (auto& [name, s] : stats) {
        s.per_frame_ms = frames.empty() ? 0.0 : s.total_ms / static_cast<f64>(frames.size());
        out.push_back(std::move(s));
    }
    std::sort(out.begin(), out.end(), [](const ZoneStat& a, const ZoneStat& b) {
        return a.total_ms != b.total_ms ? a.total_ms > b.total_ms : a.name < b.name;
    });
    return out;
}

bool Profiler::WriteChromeTrace(const std::string& path, std::span<const ProfileFrame> frames, std::string* error) const {
    using nlohmann::json;
    json events = json::array();
    std::map<u32, bool> threads;
    for (const ProfileFrame& frame : frames) {
        events.push_back({{"name", "Frame " + std::to_string(frame.index)}, {"ph", "X"}, {"pid", 1}, {"tid", 1000},
                          {"ts", static_cast<f64>(frame.start_ns) / 1000.0}, {"dur", static_cast<f64>(frame.end_ns - frame.start_ns) / 1000.0}});
        for (const ProfileZone& z : frame.zones) {
            threads[z.thread] = true;
            events.push_back({{"name", z.name}, {"ph", "X"}, {"pid", 1}, {"tid", z.thread},
                              {"ts", static_cast<f64>(z.start_ns) / 1000.0}, {"dur", static_cast<f64>(z.end_ns - z.start_ns) / 1000.0}});
        }
        for (const auto& [name, value] : frame.counters)
            events.push_back({{"name", name}, {"ph", "C"}, {"pid", 1}, {"ts", static_cast<f64>(frame.end_ns) / 1000.0}, {"args", {{"value", value}}}});
    }
    events.push_back({{"name", "thread_name"}, {"ph", "M"}, {"pid", 1}, {"tid", 1000}, {"args", {{"name", "Frames"}}}});
    for (const auto& [thread, used] : threads)
        events.push_back({{"name", "thread_name"}, {"ph", "M"}, {"pid", 1}, {"tid", thread}, {"args", {{"name", ThreadName(thread)}}}});
    std::ofstream out(path);
    if (!out) {
        if (error) *error = "couldn't write " + path;
        return false;
    }
    out << json{{"traceEvents", events}, {"displayTimeUnit", "ms"}}.dump();
    return static_cast<bool>(out);
}

} // namespace aether
