#include "aether/dev/profiler.h"

#include <algorithm>
#include <chrono>

namespace aether::dev {

namespace {

constexpr u32 kNoZone = 0xFFFFFFFFu; // a zone begun while nothing was recording: kept on the stack, never stored

std::atomic<Profiler*>& Active() {
    static std::atomic<Profiler*> active{nullptr};
    return active;
}

std::atomic<u64>& Generations() {
    static std::atomic<u64> next{1};
    return next;
}

u64 SteadyNowNs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace

struct Profiler::ThreadBuffer {
    std::mutex mutex; // guards `done`: the owning thread appends, EndFrame takes
    u32 thread = 0;
    std::vector<Open> stack; // owning thread only
    std::vector<ZoneRecord> done;
    std::unordered_map<const char*, u32> static_names; // owning thread only: literal pointer -> name id
};

namespace {
struct ThreadCache {
    u64 generation = 0;
    std::shared_ptr<void> buffer; // a Profiler::ThreadBuffer, type-erased so this can live outside the class
};
thread_local ThreadCache t_cache;
} // namespace

Profiler::Profiler(usize history_frames) : generation_(Generations().fetch_add(1)), history_(std::max<usize>(history_frames, 1)) {
    hooks_.zone_begin = &Profiler::HookBegin;
    hooks_.zone_end = &Profiler::HookEnd;
    hooks_.memory = &Profiler::HookMemory;
}

Profiler::~Profiler() { Uninstall(); }

void Profiler::Install() {
    Active().store(this);
    prof::SetHooks(&hooks_);
}

void Profiler::Uninstall() {
    Profiler* self = this;
    if (Active().compare_exchange_strong(self, nullptr)) prof::SetHooks(nullptr);
}

bool Profiler::Installed() const { return Active().load() == this; }

void Profiler::SetClock(std::function<u64()> clock) { clock_ = std::move(clock); }

void Profiler::SetHistory(usize frames) {
    history_ = std::max<usize>(frames, 1);
    if (frames_.size() > history_) frames_.erase(frames_.begin(), frames_.begin() + static_cast<std::ptrdiff_t>(frames_.size() - history_));
}

u64 Profiler::Now() const { return clock_ ? clock_() : SteadyNowNs(); }

void Profiler::HookBegin(const char* name, bool dynamic) {
    if (Profiler* p = Active().load(std::memory_order_acquire)) p->BeginZone(name, dynamic);
}
void Profiler::HookEnd() {
    if (Profiler* p = Active().load(std::memory_order_acquire)) p->EndZone();
}
void Profiler::HookMemory(const char* category, i64 delta) {
    if (Profiler* p = Active().load(std::memory_order_acquire)) p->AddMemory(category, delta);
}

Profiler::ThreadBuffer& Profiler::Buffer() {
    if (t_cache.generation != generation_ || !t_cache.buffer) {
        auto buffer = std::make_shared<ThreadBuffer>();
        {
            std::lock_guard lock(buffers_mutex_);
            buffer->thread = static_cast<u32>(buffers_.size());
            buffers_.push_back(buffer);
        }
        t_cache.generation = generation_;
        t_cache.buffer = buffer;
    }
    return *static_cast<ThreadBuffer*>(t_cache.buffer.get());
}

u32 Profiler::Intern(const char* name, bool dynamic) {
    std::lock_guard lock(names_mutex_);
    auto make = [&](const std::string& s) {
        auto it = name_ids_.find(s);
        if (it != name_ids_.end()) return it->second;
        const u32 id = static_cast<u32>(names_.size());
        names_.push_back(s);
        name_ids_.emplace(s, id);
        return id;
    };
    (void)dynamic;
    return make(name != nullptr ? name : "");
}

void Profiler::BeginZone(const char* name, bool dynamic_name) {
    ThreadBuffer& buf = Buffer();
    if (!Enabled() || !InFrame()) {
        buf.stack.push_back({kNoZone, 0});
        return;
    }
    u32 id;
    if (!dynamic_name) {
        auto it = buf.static_names.find(name);
        if (it == buf.static_names.end()) it = buf.static_names.emplace(name, Intern(name, false)).first;
        id = it->second;
    } else {
        id = Intern(name, true); // a temporary string: always by content
    }
    buf.stack.push_back({id, Now()});
}

void Profiler::EndZone() {
    ThreadBuffer& buf = Buffer();
    if (buf.stack.empty()) return;
    const Open open = buf.stack.back();
    buf.stack.pop_back();
    if (open.name == kNoZone) return;
    ZoneRecord z;
    z.name = open.name;
    z.thread = buf.thread;
    z.depth = static_cast<u32>(buf.stack.size());
    z.start_ns = open.start;
    z.end_ns = std::max(Now(), open.start);
    std::lock_guard lock(buf.mutex);
    buf.done.push_back(z);
}

void Profiler::SetCounter(const std::string& name, f64 value) {
    std::lock_guard lock(counters_mutex_);
    counters_[name] = value;
}

void Profiler::AddCounter(const std::string& name, f64 delta) {
    std::lock_guard lock(counters_mutex_);
    counters_[name] += delta;
}

void Profiler::BeginFrame() {
    frame_start_ = Now();
    in_frame_.store(true, std::memory_order_relaxed);
}

void Profiler::EndFrame() {
    if (!InFrame()) return;
    const u64 end = std::max(Now(), frame_start_);
    in_frame_.store(false, std::memory_order_relaxed);

    FrameProfile frame;
    frame.index = frame_index_++;
    frame.start_ns = frame_start_;
    frame.end_ns = end;
    {
        std::lock_guard lock(buffers_mutex_);
        for (auto& buf : buffers_) {
            std::lock_guard buffer_lock(buf->mutex);
            // Zones still running on another thread end in a later frame; keep the rest for it.
            auto split = std::stable_partition(buf->done.begin(), buf->done.end(), [&](const ZoneRecord& z) { return z.end_ns <= end; });
            frame.zones.insert(frame.zones.end(), buf->done.begin(), split);
            buf->done.erase(buf->done.begin(), split);
        }
    }
    std::sort(frame.zones.begin(), frame.zones.end(), [](const ZoneRecord& a, const ZoneRecord& b) {
        if (a.thread != b.thread) return a.thread < b.thread;
        if (a.start_ns != b.start_ns) return a.start_ns < b.start_ns;
        return a.depth < b.depth;
    });
    {
        std::lock_guard lock(counters_mutex_);
        frame.counters = std::move(counters_);
        counters_.clear();
    }
    frames_.push_back(std::move(frame));
    if (frames_.size() > history_) frames_.erase(frames_.begin());
}

const FrameProfile* Profiler::Frame(usize back) const {
    return back < frames_.size() ? &frames_[frames_.size() - 1 - back] : nullptr;
}

std::string Profiler::NameOf(u32 id) const {
    std::lock_guard lock(names_mutex_);
    return id < names_.size() ? names_[id] : std::string();
}

std::vector<ZoneStat> Profiler::Stats(usize back) const {
    const FrameProfile* frame = Frame(back);
    if (frame == nullptr) return {};

    struct Accum {
        f64 inclusive = 0, self = 0, max = 0;
        u32 count = 0;
    };
    std::map<u32, Accum> by_name;
    // Zones are sorted by (thread, start, depth): a stack of the ones still open on the thread gives each its parent.
    struct Frame_ {
        u64 end;
        f64 child_ns;
        u32 name;
        f64 duration;
    };
    std::vector<Frame_> stack;
    u32 thread = ~0u;
    auto close = [&](const Frame_& f) {
        Accum& a = by_name[f.name];
        a.inclusive += f.duration;
        a.self += f.duration - f.child_ns;
        a.max = std::max(a.max, f.duration);
        ++a.count;
    };
    for (const ZoneRecord& z : frame->zones) {
        if (z.thread != thread) {
            for (auto it = stack.rbegin(); it != stack.rend(); ++it) close(*it);
            stack.clear();
            thread = z.thread;
        }
        while (!stack.empty() && stack.back().end <= z.start_ns) {
            close(stack.back());
            stack.pop_back();
        }
        const f64 duration = static_cast<f64>(z.DurationNs());
        if (!stack.empty()) stack.back().child_ns += duration;
        stack.push_back({z.end_ns, 0.0, z.name, duration});
    }
    for (auto it = stack.rbegin(); it != stack.rend(); ++it) close(*it);

    std::vector<ZoneStat> out;
    for (const auto& [id, a] : by_name) {
        ZoneStat s;
        s.name = NameOf(id);
        s.inclusive_ms = a.inclusive / 1.0e6;
        s.self_ms = a.self / 1.0e6;
        s.max_ms = a.max / 1.0e6;
        s.count = a.count;
        out.push_back(std::move(s));
    }
    std::sort(out.begin(), out.end(), [](const ZoneStat& a, const ZoneStat& b) {
        return a.inclusive_ms != b.inclusive_ms ? a.inclusive_ms > b.inclusive_ms : a.name < b.name;
    });
    return out;
}

std::vector<f32> Profiler::FrameTimesMs(usize count) const {
    std::vector<f32> out;
    const usize n = std::min(count, frames_.size());
    for (usize i = frames_.size() - n; i < frames_.size(); ++i) out.push_back(static_cast<f32>(frames_[i].DurationMs()));
    return out;
}

f64 Profiler::AverageFrameMs(usize count) const {
    const std::vector<f32> t = FrameTimesMs(count);
    if (t.empty()) return 0.0;
    f64 sum = 0.0;
    for (f32 v : t) sum += v;
    return sum / static_cast<f64>(t.size());
}

f64 Profiler::PercentileFrameMs(f64 p, usize count) const {
    std::vector<f32> t = FrameTimesMs(count);
    if (t.empty()) return 0.0;
    std::sort(t.begin(), t.end());
    const f64 clamped = std::clamp(p, 0.0, 1.0);
    return t[static_cast<usize>(clamped * static_cast<f64>(t.size() - 1) + 0.5)];
}

f64 Profiler::Fps(usize count) const {
    const f64 avg = AverageFrameMs(count);
    return avg > 0.0 ? 1000.0 / avg : 0.0;
}

void Profiler::AddMemory(const std::string& category, i64 delta) {
    std::lock_guard lock(memory_mutex_);
    MemoryCategory& c = memory_[category];
    if (c.name.empty()) c.name = category;
    c.bytes += delta;
    c.peak = std::max(c.peak, c.bytes);
}

std::vector<MemoryCategory> Profiler::Memory() const {
    std::vector<MemoryCategory> out;
    {
        std::lock_guard lock(memory_mutex_);
        for (const auto& [name, c] : memory_) out.push_back(c);
    }
    std::sort(out.begin(), out.end(), [](const MemoryCategory& a, const MemoryCategory& b) {
        return a.bytes != b.bytes ? a.bytes > b.bytes : a.name < b.name;
    });
    return out;
}

i64 Profiler::TotalMemory() const {
    std::lock_guard lock(memory_mutex_);
    i64 total = 0;
    for (const auto& [name, c] : memory_) total += c.bytes;
    return total;
}

} // namespace aether::dev
