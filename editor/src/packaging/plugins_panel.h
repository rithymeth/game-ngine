#pragma once

#include "aether/plugin/plugin.h"
#include "aether/project/project.h"

#include <filesystem>
#include <string>
#include <vector>

// The Plugins panel (Phase 26 step 1, docs/design/PHASE_SPECS.md §26.1):
// every plugin the project can use (the engine's and its own), with what
// each needs and whether its modules are built in; checking one enables it
// in the project (and what it depends on comes along). New Plugin makes a
// plugin folder in the project's Plugins/. Draws into the current window.

namespace aether::editor {

class PluginsPanel {
public:
    explicit PluginsPanel(std::filesystem::path project_file);
    void Draw();

    // Re-reads the project and the plugin folders.
    bool Refresh();
    // Enables or disables a plugin in the project (saving it). Plugins
    // enabled by default or needed by another can't be turned off here.
    bool SetEnabled(const std::string& name, bool enabled, std::string* error = nullptr);
    // Makes <project>/Plugins/<name>/ and refreshes.
    bool CreatePlugin(const std::string& name, std::string* error = nullptr);

    const plugin::PluginManager& Manager() const { return manager_; }
    const std::vector<std::string>& Requested() const { return settings_.plugins; }
    const std::string& Error() const { return error_; }
    const std::vector<std::string>& Warnings() const { return warnings_; }

private:
    bool Resolve();

    std::filesystem::path project_file_;
    ProjectSettings settings_;
    plugin::PluginManager manager_;
    std::string error_;
    std::vector<std::string> warnings_;
    std::string filter_;
    std::string new_name_;
};

} // namespace aether::editor
