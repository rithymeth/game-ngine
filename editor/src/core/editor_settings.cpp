#include "core/editor_settings.h"

#include "core/recent_projects.h"

#include "aether/platform/filesystem.h"
#include "aether/reflection/serialize.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <system_error>

namespace aether::editor {

namespace stdfs = std::filesystem;

stdfs::path EditorSettings::DefaultFile() {
    if (const char* override_file = std::getenv("AETHER_EDITOR_SETTINGS"); override_file != nullptr && *override_file != '\0')
        return override_file;
    return RecentProjects::DefaultConfigDir() / "editor_settings.json";
}

EditorSettings EditorSettings::Load(const stdfs::path& file) {
    EditorSettings settings;
    std::vector<u8> bytes;
    if (!fs::ReadFileBytes(file.string(), bytes)) return settings;
    const reflect::Json json = reflect::Json::parse(bytes.begin(), bytes.end(), nullptr, /*allow_exceptions=*/false);
    if (!json.is_object()) return settings;
    // A file written by a newer editor may mean things this one does not know; guessing at it is worse than the defaults.
    const auto version = json.find("version");
    if (version != json.end() && version->is_number_integer() && version->get<i32>() > kVersion) return settings;
    const auto enabled = json.find("autosave_enabled");
    if (enabled != json.end() && enabled->is_boolean()) settings.autosave_enabled = enabled->get<bool>();
    const auto interval = json.find("autosave_interval_seconds");
    if (interval != json.end() && interval->is_number()) settings.autosave_interval_seconds = interval->get<f32>();
    settings.Normalize();
    return settings;
}

bool EditorSettings::Save(const stdfs::path& file) const {
    EditorSettings normalized = *this;
    normalized.Normalize();
    const reflect::Json json{{"version", kVersion},
                             {"autosave_enabled", normalized.autosave_enabled},
                             {"autosave_interval_seconds", normalized.autosave_interval_seconds}};
    std::string text = json.dump(2);
    text.push_back('\n');
    std::error_code ec;
    stdfs::create_directories(file.parent_path(), ec);
    return fs::WriteFileBytes(file.string(), text.data(), text.size());
}

bool EditorSettings::Normalize() {
    bool changed = false;
    if (!std::isfinite(autosave_interval_seconds)) {
        autosave_interval_seconds = Defaults().autosave_interval_seconds;
        changed = true;
    }
    const f32 interval = std::clamp(autosave_interval_seconds, kMinAutosaveSeconds, kMaxAutosaveSeconds);
    if (interval != autosave_interval_seconds) {
        autosave_interval_seconds = interval;
        changed = true;
    }
    if (version != kVersion) {
        version = kVersion;
        changed = true;
    }
    return changed;
}

} // namespace aether::editor
