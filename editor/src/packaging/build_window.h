#pragma once

#include "aether/cook/package.h"
#include "aether/platform/process.h"
#include "aether/project/project.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The Build and Package window and the Project Settings panel (Phase 25
// step 5, docs/design/PHASE_SPECS.md §25.5). Both draw into the current
// ImGui window, like the editor's other panels.

namespace aether::editor {

// Cooks a project, or packages it (a cook staged with the player), on a
// worker thread, showing the progress and the cook's log; then launches the
// packaged game.
class BuildPackageWindow {
public:
    enum class Result { None, Succeeded, Failed, Cancelled };
    struct LogLine {
        bool warning = false;
        std::string text;
    };

    explicit BuildPackageWindow(std::filesystem::path project_file);
    ~BuildPackageWindow(); // cancels a running build and waits for it

    void Draw();

    // The buttons. Start* are false while a build is running.
    bool StartCook();    // into <project>/Saved/Cooked/<Configuration>
    bool StartPackage(); // into output_dir
    void Cancel();
    // Runs the last packaged game; false (and `error`) if there's none or it won't start.
    bool Launch(std::string* error = nullptr);

    bool Busy() const { return busy_.load(); }
    // Blocks until the running build ends (tests, and closing the editor).
    void WaitForBuild();
    f32 Progress() const;
    std::string Stage() const;
    std::vector<LogLine> Log() const;
    Result LastResult() const;
    const std::filesystem::path& ProjectFile() const { return project_file_; }
    std::filesystem::path StagedGame() const;
    platform::ChildProcess& Game() { return game_; }

    // Settings (the window's controls).
    cook::BuildConfiguration configuration = cook::BuildConfiguration::Development;
    pak::CompressionPolicy compression = pak::CompressionPolicy::Auto;
    u32 texture_quality = 2;
    std::string output_dir;  // empty: <project>/Saved/Packaged/<Configuration>
    std::string player_path; // empty: FindPlayerExecutable()
    // Extra player arguments for Launch, e.g. "--quality Low".
    std::string launch_args;
    // Encrypts the paks with `encryption_key` (64 hex digits); Launch passes
    // the key to the game.
    bool encrypt = false;
    std::string encryption_key;

    std::filesystem::path OutputDir() const;

private:
    bool Start(bool package);
    void AddLog(bool warning, std::string text);

    std::filesystem::path project_file_;
    std::thread worker_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> cancel_{false};
    mutable std::mutex mutex_;
    f32 progress_ = 0.0f;
    std::string stage_;
    std::vector<LogLine> log_;
    Result result_ = Result::None;
    std::filesystem::path staged_game_;
    platform::ChildProcess game_;
    std::string launch_error_;
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
