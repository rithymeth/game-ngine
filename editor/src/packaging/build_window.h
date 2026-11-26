#pragma once

#include "core/package_runner.h"

#include <filesystem>
#include <functional>
#include <string>

// The Build and Package window and the Project Settings panel (Phase 25
// step 5). Both draw into the current
// ImGui window, like the editor's other panels.
//
// The cooking and packaging itself is PackageRunner, in the portable editor
// layer, so this window is only the drawing and a tool or test can drive the
// same runner without it.

namespace aether::editor {

// Cooks a project, or packages it (a cook staged with the player), showing the
// progress and the cook's log, then launching the packaged game.
class BuildPackageWindow : public PackageRunner {
public:
    explicit BuildPackageWindow(std::filesystem::path project_file) : PackageRunner(std::move(project_file)) {}

    // Checked on the UI thread, before reading any saved build inputs.
    void SetSourceCheck(std::function<std::string()> check) { source_check_ = std::move(check); }
    using AssetCheck = std::function<bool(const std::string&)>;
    struct TileWarningCell { usize layer; i32 x, y; };
    using AssetOpener = std::function<bool(const std::string&, std::optional<TileWarningCell>, std::string*)>;
    void SetWarningAssetActions(AssetCheck check, AssetOpener open) {
        warning_asset_check_ = std::move(check);
        warning_asset_open_ = std::move(open);
    }
    std::string WarningAsset(const LogLine& line) const;
    bool OpenWarningAsset(const LogLine& line, std::string* error = nullptr);
    bool StartCook();
    bool StartPackage();
    void Draw();

private:
    std::string SourceBlocker() const { return source_check_ ? source_check_() : std::string(); }
    std::function<std::string()> source_check_;
    AssetCheck warning_asset_check_;
    AssetOpener warning_asset_open_;
};

// The project's settings in a reflected property table, with Save and Revert.
class ProjectSettingsPanel {
public:
    explicit ProjectSettingsPanel(std::filesystem::path project_file);
    void Draw();
    bool Reload(std::string* error = nullptr);
    bool Save(std::string* error = nullptr);
    bool Dirty() const { return dirty_; }
    ProjectSettings& Settings() { return settings_; }
    const std::string& Status() const { return status_; }

private:
    std::filesystem::path project_file_;
    ProjectSettings settings_;
    bool dirty_ = false;
    std::string status_;
};

} // namespace aether::editor
