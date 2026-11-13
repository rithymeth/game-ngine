#pragma once

#include "aether/core/base.h"

#include <filesystem>

namespace aether::editor {

// The editor's own preferences, kept per user beside the recent-project list
// (editor_settings.json). A missing or unreadable file means the defaults: a
// settings file is a convenience, never a reason the editor won't start.
struct EditorSettings {
    static constexpr i32 kVersion = 1;
    // The editor refuses to autosave more often than this, or less often than an
    // hour: a save every frame would cost the frame rate, and one an hour means
    // a crash loses most of an afternoon.
    static constexpr f32 kMinAutosaveSeconds = 30.0f;
    static constexpr f32 kMaxAutosaveSeconds = 3600.0f;

    i32 version = kVersion;
    bool autosave_enabled = true;
    f32 autosave_interval_seconds = 300.0f;

    static EditorSettings Defaults() { return EditorSettings{}; }
    // %APPDATA%/Aether/editor_settings.json (or the platform's config folder).
    static std::filesystem::path DefaultFile();

    // Never fails: an absent, corrupt or newer-versioned file gives the defaults.
    static EditorSettings Load(const std::filesystem::path& file);
    bool Save(const std::filesystem::path& file) const;

    // Brings every value into its usable range; true when anything had to change.
    bool Normalize();
};

} // namespace aether::editor
