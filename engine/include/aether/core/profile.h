#pragma once

#include "aether/core/base.h"

#include <atomic>

// Instrumentation hooks (Phase 23). The engine marks its work with zones and
// reports memory through these; they cost one atomic load and a branch until
// something (the devtools Profiler, or Tracy through an adapter) installs
// hooks, so they can stay in shipping code. The engine does not depend on
// whatever consumes them.
//
//   void Update() {
//       AETHER_ZONE("Update");        // a zone named by a string literal
//       ...
//   }
//
// Zone names must be string literals (or otherwise live forever);
// AETHER_ZONE_DYNAMIC takes any string, such as a system's name, at the price
// of the profiler copying it.

namespace aether::prof {

struct Hooks {
    void (*zone_begin)(const char* name, bool dynamic_name) = nullptr;
    void (*zone_end)() = nullptr;
    // Bytes of `category` acquired (positive) or released (negative).
    void (*memory)(const char* category, i64 delta_bytes) = nullptr;
};

inline std::atomic<const Hooks*>& HookSlot() {
    static std::atomic<const Hooks*> slot{nullptr};
    return slot;
}

// Installs `hooks` (which must outlive its use); null removes them.
inline void SetHooks(const Hooks* hooks) { HookSlot().store(hooks, std::memory_order_release); }
inline const Hooks* CurrentHooks() { return HookSlot().load(std::memory_order_acquire); }

// Pairs end with the hooks that saw begin, even if they are replaced in between.
class ZoneScope {
public:
    explicit ZoneScope(const char* name, bool dynamic_name = false) : hooks_(CurrentHooks()) {
        if (hooks_ != nullptr && hooks_->zone_begin != nullptr) hooks_->zone_begin(name, dynamic_name);
        else hooks_ = nullptr;
    }
    ~ZoneScope() {
        if (hooks_ != nullptr && hooks_->zone_end != nullptr) hooks_->zone_end();
    }
    ZoneScope(const ZoneScope&) = delete;
    ZoneScope& operator=(const ZoneScope&) = delete;

private:
    const Hooks* hooks_;
};

inline void ReportMemory(const char* category, i64 delta_bytes) {
    const Hooks* hooks = CurrentHooks();
    if (hooks != nullptr && hooks->memory != nullptr) hooks->memory(category, delta_bytes);
}

} // namespace aether::prof

#define AETHER_PROF_CONCAT_INNER(a, b) a##b
#define AETHER_PROF_CONCAT(a, b) AETHER_PROF_CONCAT_INNER(a, b)
#define AETHER_ZONE(name) ::aether::prof::ZoneScope AETHER_PROF_CONCAT(aether_zone_, __LINE__)(name)
#define AETHER_ZONE_DYNAMIC(name) ::aether::prof::ZoneScope AETHER_PROF_CONCAT(aether_zone_, __LINE__)((name), true)
