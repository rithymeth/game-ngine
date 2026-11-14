#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace aether::editor {

// Stage 1 of docs/design/EDITOR_PARITY_AUDIT.md: Window > Layout. A preset is an .ini file in exactly the format ImGui
// writes its own settings ini (docking tree included), captured with ImGui::SaveIniSettingsToMemory and applied with
// LoadIniSettingsFromMemory, so there is no second layout format to keep in step with the docking system.
//
// The built-in names are arrangements the shell builds; saving under one of those names writes a file that then wins
// when that name is chosen, which is what lets "Save Layout As Default" mean something. Reset always rebuilds the
// built-in Default, ignoring any file of that name.
//
// No ImGui here, so the store -- naming, listing, reading and writing -- runs headless in tests.
class LayoutPresets {
public:
    // `directory`: where presets are written. Empty means the per-user config folder, redirected by
    // AETHER_EDITOR_LAYOUT like the layout file itself.
    explicit LayoutPresets(std::filesystem::path directory = {});

    // "Default", "Level design", "Scripting", "Animation": arrangements the shell builds, in menu order.
    static const std::vector<std::string>& BuiltinNames();
    static bool IsBuiltin(const std::string& name);

    const std::filesystem::path& Directory() const { return directory_; }

    // The built-ins first, then saved layouts by name (case-insensitively), without duplicates.
    std::vector<std::string> Names() const;
    // Saved layouts only, for a menu that lists the built-ins itself; a file named like a built-in is not repeated.
    std::vector<std::string> SavedNames() const;

    // `<directory>/<name>.ini`, the name's path-unfriendly characters replaced by '_'. Empty for an unusable name.
    std::filesystem::path Path(const std::string& name) const;

    // Writes `ini` under `name`, making the folder. False, with `error` set, when the name or text is empty or the
    // write fails.
    bool Save(const std::string& name, const std::string& ini, std::string* error = nullptr);
    // The text saved under `name`; no value when there is no such file or it is empty.
    std::optional<std::string> Load(const std::string& name) const;

private:
    std::filesystem::path directory_;
};

} // namespace aether::editor
