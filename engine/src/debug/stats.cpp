#include "aether/debug/stats.h"

#include "aether/core/console.h"
#include "aether/core/profiler.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace aether {

StatGroups& StatGroups::Get() {
    static StatGroups groups;
    return groups;
}

void StatGroups::Register(const std::string& name, const std::string& help, StatGroupFn fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    groups_[name] = {help, std::move(fn)};
}

void StatGroups::Unregister(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    groups_.erase(name);
    std::erase(shown_, name);
}

bool StatGroups::Has(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return groups_.count(name) > 0;
}

std::vector<std::string> StatGroups::Names() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& [name, g] : groups_) out.push_back(name);
    return out;
}

bool StatGroups::Toggle(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!groups_.count(name)) return false;
    auto it = std::find(shown_.begin(), shown_.end(), name);
    if (it == shown_.end()) shown_.push_back(name);
    else shown_.erase(it);
    return true;
}

void StatGroups::Show(const std::string& name, bool shown) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find(shown_.begin(), shown_.end(), name);
    if (shown && it == shown_.end() && groups_.count(name)) shown_.push_back(name);
    if (!shown && it != shown_.end()) shown_.erase(it);
}

void StatGroups::HideAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    shown_.clear();
}

bool StatGroups::Shown(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::find(shown_.begin(), shown_.end(), name) != shown_.end();
}

std::vector<std::string> StatGroups::ShownNames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return shown_;
}

std::string StatGroups::Help(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = groups_.find(name);
    return it == groups_.end() ? std::string() : it->second.help;
}

std::vector<std::pair<std::string, std::vector<StatLine>>> StatGroups::Collect() const {
    std::vector<std::pair<std::string, StatGroupFn>> shown;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const std::string& name : shown_)
            if (auto it = groups_.find(name); it != groups_.end()) shown.emplace_back(name, it->second.fn);
    }
    std::vector<std::pair<std::string, std::vector<StatLine>>> out;
    for (auto& [name, fn] : shown) out.emplace_back(name, fn ? fn() : std::vector<StatLine>{});
    return out;
}

// ---------------------------------------------------------------- the built-in groups

namespace {

std::string Format(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return buf;
}

std::vector<ProfileFrame> RecentFrames(usize n) {
    std::vector<ProfileFrame> frames = Profiler::Get().Frames();
    if (frames.size() > n) frames.erase(frames.begin(), frames.end() - static_cast<std::ptrdiff_t>(n));
    return frames;
}

u32 FrameColor(f64 ms) { return ms > 1000.0 / 30.0 ? kStatBad : ms > 1000.0 / 60.0 ? kStatWarn : kStatGood; }

std::vector<StatLine> FpsLines() {
    const std::vector<ProfileFrame> frames = RecentFrames(60);
    if (frames.empty()) return {{"fps: no frames (the loop calls Profiler::BeginFrame/EndFrame)", kStatPlain}};
    f64 sum = 0, lo = 1e300, hi = 0;
    for (const ProfileFrame& f : frames) sum += f.Ms(), lo = std::min(lo, f.Ms()), hi = std::max(hi, f.Ms());
    const f64 avg = sum / static_cast<f64>(frames.size());
    return {{Format("%.0f fps  %.2f ms", avg > 0 ? 1000.0 / avg : 0.0, avg), FrameColor(avg)},
            {Format("min %.2f  max %.2f ms (%zu frames)", lo, hi, frames.size()), FrameColor(hi)}};
}

std::vector<StatLine> ZoneLines() {
    const std::vector<ProfileFrame> frames = RecentFrames(60);
    std::vector<StatLine> out;
    const std::vector<ZoneStat> stats = Profiler::Aggregate(frames);
    for (usize i = 0; i < stats.size() && i < 10; ++i)
        out.push_back({Format("%-24s %6.2f ms  self %6.2f", stats[i].name.c_str(), stats[i].per_frame_ms,
                              frames.empty() ? 0.0 : stats[i].self_ms / static_cast<f64>(frames.size())),
                       kStatPlain});
    if (out.empty()) out.push_back({"no zones", kStatPlain});
    return out;
}

std::vector<StatLine> CounterLines() {
    ProfileFrame f;
    std::vector<StatLine> out;
    if (Profiler::Get().Latest(f))
        for (const auto& [name, value] : f.counters) out.push_back({Format("%-24s %.0f", name.c_str(), value), kStatPlain});
    if (out.empty()) out.push_back({"no counters", kStatPlain});
    return out;
}

std::vector<StatLine> GpuLines() {
    ProfileFrame f;
    std::vector<StatLine> out;
    if (Profiler::Get().Latest(f)) {
        f64 total = 0;
        for (const GpuTiming& g : f.gpu) out.push_back({Format("%-24s %6.2f ms", g.name.c_str(), g.ms), kStatPlain}), total += g.ms;
        if (!f.gpu.empty()) out.insert(out.begin(), {Format("GPU %.2f ms", total), FrameColor(total)});
    }
    if (out.empty()) out.push_back({"no GPU timings (the render backend reports them)", kStatPlain});
    return out;
}

std::vector<StatLine> MemoryLines() {
    std::vector<StatLine> out;
    i64 total = 0;
    for (const MemoryCategoryStats& m : MemoryTracker::Get().Categories()) {
        total += m.bytes;
        out.push_back({Format("%-28s %8.1f KB (peak %.1f)", m.name.c_str(), static_cast<f64>(m.bytes) / 1024.0,
                              static_cast<f64>(m.peak) / 1024.0),
                       kStatPlain});
    }
    out.insert(out.begin(), {Format("tracked %.2f MB", static_cast<f64>(total) / (1024.0 * 1024.0)), kStatPlain});
    return out;
}

const bool g_builtin_groups = [] {
    StatGroups& g = StatGroups::Get();
    g.Register("fps", "Frame rate and frame times over the last 60 frames", FpsLines);
    g.Register("zones", "The 10 most expensive profiler zones per frame", ZoneLines);
    g.Register("counters", "The last frame's counters and gauges", CounterLines);
    g.Register("gpu", "The last frame's GPU pass times", GpuLines);
    g.Register("memory", "Memory in use by category", MemoryLines);
    return true;
}();

AutoConsoleCommand g_stat_command(
    "stat", "Toggles a stat overlay: stat fps|zones|counters|gpu|memory|net|...; 'stat none' hides them all; 'stat' lists them",
    [](const std::vector<std::string>& args, Console& c) {
        StatGroups& g = StatGroups::Get();
        if (args.empty()) {
            for (const std::string& name : g.Names())
                c.Print(std::string(g.Shown(name) ? "* " : "  ") + name + "  " + g.Help(name));
            return;
        }
        for (const std::string& name : args) {
            if (name == "none") g.HideAll();
            else if (!g.Toggle(name)) c.Print("no stat group named '" + name + "'", LogLevel::Error);
        }
    });

} // namespace

} // namespace aether
