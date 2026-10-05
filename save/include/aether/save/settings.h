#pragma once

#include "aether/core/base.h"
#include "aether/input/bindings.h"
#include "aether/reflection/reflection.h"
#include "aether/reflection/serialize.h"
#include "aether/save/envelope.h"
#include "aether/save/save_result.h"

#include <cmath>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// Settings (Phase 28 step 3, docs/design/PHASE_SPECS.md §28.3): what a player
// sets in the options menu (graphics quality, audio volumes, key rebinds,
// language), kept apart from the game's saves: its own file
// (`<dir>/settings.asettings`, an "aether.settings" file, so a save slot can't
// be mistaken for it), written atomically with a backup, and loaded at start
// up with defaults whenever it is missing or damaged. The same envelope as
// saves (a reflected struct, a version, a checksum, migration hooks).
//
// `SettingsStore<T>` works for any reflected struct, so a game can keep its
// own settings struct; `GameSettings` is the engine's. A store belongs to one
// thread (the main one).

namespace aether::save {

struct GameSettings {
    std::string quality = "High"; // a project quality preset's name (ProjectSettings::quality_presets)
    u32 width = 1280;
    u32 height = 720;
    bool fullscreen = false;
    bool vsync = true;
    // Volumes as the player sees them, 0 (silent) to 1 (full); BusVolumeDb turns one into the mixer's dB.
    f32 master = 1.0f;
    f32 music = 1.0f;
    f32 sfx = 1.0f;
    f32 voice = 1.0f;
    std::string language = "en";
    input::UserBindings bindings; // the player's key rebinds
};

// Clamps what a hand-edited or damaged file can get wrong (volumes to 0..1, a
// NaN to the default, a window size to 320..16384, an empty or over-long
// language or quality name back to the default), adding a line to `warnings`
// for each. SettingsStore calls it on load and save; the ADL name is how a
// game's own settings struct gets validated too.
void Validate(GameSettings& settings, std::vector<std::string>* warnings = nullptr);

// A 0..1 volume as dB for the mixer: 20 * log10(v), with 0 (and anything
// below -80 dB) the floor of -80.
f32 BusVolumeDb(f32 linear);

template <typename T = GameSettings>
class SettingsStore {
public:
    using Observer = std::function<void(const T& now, const T& before)>;

    // The file is `<directory>/<file>.asettings`.
    explicit SettingsStore(std::filesystem::path directory, std::string file = "settings")
        : directory_(std::move(directory)), file_(std::move(file)) {}

    std::filesystem::path Path() const { return directory_ / (file_ + ".asettings"); }

    // Reads the file (or its backup). `ok` means Get() is usable: a missing
    // file is normal (the defaults, no error, no warning); a damaged, wrong,
    // or newer-version file gives the defaults, `error` says what was wrong
    // with it and `warnings` has the message. The bad file is left alone until
    // the next Save. Values are validated. Observers aren't called.
    SaveResult Load() {
        T loaded{};
        SaveResult result = envelope::ReadWithBackup(Path(), kKind, reflect::Reflect<T>(), &loaded);
        SaveResult out = envelope::Ok();
        if (result.ok) {
            value_ = std::move(loaded);
            out.warnings = std::move(result.warnings);
        } else {
            value_ = T{};
            if (result.error != SaveError::NotFound) {
                out.error = result.error;
                out.message = result.message;
                out.warnings.push_back(result.message + " (using the default settings)");
            }
        }
        Fix(out.warnings);
        return out;
    }

    // Validates and writes the file atomically (keeping the previous one as `.bak`).
    SaveResult Save() {
        std::vector<std::string> warnings;
        Fix(warnings);
        SaveResult r = envelope::Write(Path(), envelope::Make(kKind, file_, reflect::Reflect<T>(), &value_));
        r.warnings = std::move(warnings);
        return r;
    }

    const T& Get() const { return value_; }

    // Replaces the settings (validated); observers are told if anything changed. Returns whether it did.
    bool Set(const T& next) {
        T before = value_;
        value_ = next;
        std::vector<std::string> warnings;
        Fix(warnings);
        return Notify(before);
    }
    bool ResetToDefaults() { return Set(T{}); }

    // `fn(now, before)` after every change made by Set / ResetToDefaults. A
    // Set made from inside an observer takes effect but doesn't notify again.
    u64 AddObserver(Observer fn) {
        observers_.push_back({++next_id_, std::move(fn)});
        return next_id_;
    }
    void RemoveObserver(u64 id) {
        for (usize i = 0; i < observers_.size(); ++i) {
            if (observers_[i].first == id) {
                observers_.erase(observers_.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
        }
    }

private:
    static constexpr const char* kKind = "aether.settings";

    void Fix(std::vector<std::string>& warnings) {
        if constexpr (requires(T& t, std::vector<std::string>* w) { Validate(t, w); }) Validate(value_, &warnings);
    }
    bool Notify(const T& before) {
        if (reflect::ToJson(value_).dump() == reflect::ToJson(before).dump()) return false;
        if (notifying_) return true;
        notifying_ = true;
        const std::vector<std::pair<u64, Observer>> copy = observers_; // an observer may add or remove observers
        for (const auto& [id, fn] : copy) {
            if (fn) fn(value_, before);
        }
        notifying_ = false;
        return true;
    }

    std::filesystem::path directory_;
    std::string file_;
    T value_{};
    std::vector<std::pair<u64, Observer>> observers_;
    u64 next_id_ = 0;
    bool notifying_ = false;
};

} // namespace aether::save

AETHER_REFLECT(aether::save::GameSettings, 1,
    AETHER_FIELD(quality, Field_EditAnywhere, {.tooltip = "A quality preset's name"}),
    AETHER_FIELD(width, Field_EditAnywhere, {.range_min = 320, .range_max = 16384, .units = "px"}),
    AETHER_FIELD(height, Field_EditAnywhere, {.range_min = 320, .range_max = 16384, .units = "px"}),
    AETHER_FIELD(fullscreen, Field_EditAnywhere),
    AETHER_FIELD(vsync, Field_EditAnywhere),
    AETHER_FIELD(master, Field_EditAnywhere, {.range_min = 0, .range_max = 1}),
    AETHER_FIELD(music, Field_EditAnywhere, {.range_min = 0, .range_max = 1}),
    AETHER_FIELD(sfx, Field_EditAnywhere, {.range_min = 0, .range_max = 1}),
    AETHER_FIELD(voice, Field_EditAnywhere, {.range_min = 0, .range_max = 1}),
    AETHER_FIELD(language, Field_EditAnywhere, {.tooltip = "A language code: en, fr, ja ..."}),
    AETHER_FIELD(bindings, Field_EditAnywhere)
)
