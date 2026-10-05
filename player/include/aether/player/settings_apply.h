#pragma once

#include "aether/save/settings.h"

#include <functional>
#include <string>
#include <vector>

namespace aether::player {

// Where a GameSettings goes (Phase 28 step 6, §28.7). Each hook is optional:
// the host that owns the mixer, the quality presets or the window supplies
// its own, so the game runtime needs none of them (tests pass recorders).
struct SettingsTargets {
    // A mixer bus's volume in dB. Bus names: Master, Music, SFX, Voice.
    std::function<void(const std::string& bus, f32 db)> set_bus_volume_db;
    // A quality preset by name (the host resolves it, e.g. with ChooseQuality).
    std::function<void(const std::string& quality)> set_quality;
    std::function<void(u32 width, u32 height, bool fullscreen, bool vsync)> set_window;
};

// Pushes every setting to its hook (volumes through save::BusVolumeDb).
// With `before`, only what changed is pushed. Returns what it did, one line
// each, for the log.
std::vector<std::string> ApplySettings(const save::GameSettings& now, const SettingsTargets& targets, const save::GameSettings* before = nullptr);

} // namespace aether::player
