#include "core/editor_settings.h"
#include "test_framework.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

using namespace aether::editor;
namespace stdfs = std::filesystem;

namespace {

struct ScratchFolder {
    stdfs::path path;
    ScratchFolder() {
        static std::atomic<unsigned> next{0};
        path = stdfs::temp_directory_path() / "aether_editor_settings_tests" / std::to_string(++next);
        std::error_code ec;
        stdfs::remove_all(path, ec);
        stdfs::create_directories(path, ec);
    }
    ~ScratchFolder() {
        std::error_code ec;
        stdfs::remove_all(path, ec);
    }
};

void WriteText(const stdfs::path& file, const std::string& text) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
}

} // namespace

AETHER_TEST(EditorSettings_ADefaultsWhenThereIsNoFile) {
    ScratchFolder scratch;
    const EditorSettings settings = EditorSettings::Load(scratch.path / "missing.json");
    AETHER_CHECK(settings.autosave_enabled);
    AETHER_CHECK(settings.autosave_interval_seconds == 300.0f);
    AETHER_CHECK(settings.version == EditorSettings::kVersion);
}

AETHER_TEST(EditorSettings_RoundTrip) {
    ScratchFolder scratch;
    EditorSettings written;
    written.autosave_enabled = false;
    written.autosave_interval_seconds = 90.0f;
    AETHER_CHECK(written.Save(scratch.path / "nested" / "editor_settings.json")); // the folder is made
    const EditorSettings read = EditorSettings::Load(scratch.path / "nested" / "editor_settings.json");
    AETHER_CHECK(!read.autosave_enabled);
    AETHER_CHECK(read.autosave_interval_seconds == 90.0f);
    AETHER_CHECK(read.version == EditorSettings::kVersion);
}

AETHER_TEST(EditorSettings_AnImpossibleIntervalIsBroughtIntoRange) {
    ScratchFolder scratch;
    const stdfs::path file = scratch.path / "settings.json";
    WriteText(file, R"({"version": 1, "autosave_enabled": true, "autosave_interval_seconds": 0})");
    AETHER_CHECK(EditorSettings::Load(file).autosave_interval_seconds == EditorSettings::kMinAutosaveSeconds);
    WriteText(file, R"({"version": 1, "autosave_enabled": true, "autosave_interval_seconds": 999999})");
    AETHER_CHECK(EditorSettings::Load(file).autosave_interval_seconds == EditorSettings::kMaxAutosaveSeconds);
    // And saving never writes a value outside it, even when the in-memory one was set by hand.
    EditorSettings settings;
    settings.autosave_interval_seconds = 1.0f;
    AETHER_CHECK(settings.Save(file));
    AETHER_CHECK(EditorSettings::Load(file).autosave_interval_seconds == EditorSettings::kMinAutosaveSeconds);
}

AETHER_TEST(EditorSettings_ACorruptFileOrANewerOneMeansTheDefaults) {
    ScratchFolder scratch;
    const stdfs::path file = scratch.path / "settings.json";
    WriteText(file, "{ this is not json");
    EditorSettings settings = EditorSettings::Load(file);
    AETHER_CHECK(settings.autosave_enabled && settings.autosave_interval_seconds == 300.0f);
    // A file a newer editor wrote may hold meanings this one would guess wrong at.
    WriteText(file, R"({"version": 2, "autosave_enabled": false, "autosave_interval_seconds": 60})");
    settings = EditorSettings::Load(file);
    AETHER_CHECK(settings.autosave_enabled && settings.autosave_interval_seconds == 300.0f);
    // Unknown keys are ignored, and the ones this version knows are still read.
    WriteText(file, R"({"version": 1, "ui_scale": 1.5, "autosave_enabled": false, "autosave_interval_seconds": 45})");
    settings = EditorSettings::Load(file);
    AETHER_CHECK(!settings.autosave_enabled && settings.autosave_interval_seconds == 45.0f);
}

AETHER_TEST(EditorSettings_NormalizeReportsWhatItChanged) {
    EditorSettings defaults;
    AETHER_CHECK(!defaults.Normalize());
    EditorSettings broken;
    broken.autosave_interval_seconds = 5.0f;
    AETHER_CHECK(broken.Normalize());
    AETHER_CHECK(broken.autosave_interval_seconds == EditorSettings::kMinAutosaveSeconds);
}
