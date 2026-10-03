#pragma once

#include "aether/core/base.h"

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace aether {

// Stat overlays (Phase 23 step 3, docs/design/PHASE_SPECS.md §23.3): named
// groups of text lines - `stat fps`, `stat memory`, `stat net` - shown in a
// corner of the game or editor viewport while toggled on. Groups are
// registered by whoever has the numbers: the engine registers fps, zones,
// counters, gpu and memory (from the profiler and memory tracker); the
// network registers net for a host (net::NetStatGroup).
struct StatLine {
    std::string text;
    u32 color = 0xFFFFFFFFu; // 0xAABBGGRR
};

using StatGroupFn = std::function<std::vector<StatLine>()>;

class StatGroups {
public:
    static StatGroups& Get();

    // Replaces a group of the same name.
    void Register(const std::string& name, const std::string& help, StatGroupFn fn);
    void Unregister(const std::string& name);
    bool Has(const std::string& name) const;
    std::vector<std::string> Names() const; // sorted

    bool Toggle(const std::string& name); // false when there's no such group
    void Show(const std::string& name, bool shown);
    void HideAll();
    bool Shown(const std::string& name) const;
    std::vector<std::string> ShownNames() const; // in the order they were turned on

    // The shown groups' lines, in the order they were turned on.
    std::vector<std::pair<std::string, std::vector<StatLine>>> Collect() const;
    std::string Help(const std::string& name) const;

private:
    struct Group {
        std::string help;
        StatGroupFn fn;
    };
    mutable std::mutex mutex_;
    std::map<std::string, Group> groups_;
    std::vector<std::string> shown_;
};

// Colors the built-in groups use for good, warning and bad values.
inline constexpr u32 kStatGood = 0xFF80E080u, kStatWarn = 0xFF50C8F0u, kStatBad = 0xFF6060F0u, kStatPlain = 0xFFE8E8E8u;

} // namespace aether
