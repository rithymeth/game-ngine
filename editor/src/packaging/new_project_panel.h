#pragma once

#include "aether/templates/templates.h"

#include <filesystem>
#include <functional>
#include <string>

// The New Project panel (Phase 26 step 2, docs/design/PHASE_SPECS.md §26.2):
// the templates with what each holds, a name and a folder, and Create.
// Draws into the current ImGui window.

namespace aether::editor {

class NewProjectPanel {
public:
    // Where new projects go by default: AetherProjects in the user's home.
    static std::filesystem::path DefaultLocation();

    NewProjectPanel();
    void Draw();

    // Creates the project from the selected template, in `location`/`name`.
    // On success `created` is its .aproject and `on_created` runs.
    bool Create(std::string* error = nullptr);

    std::string name = "MyGame";
    std::string location;
    // The selected template's id.
    std::string template_id = "first_person";

    std::function<void(const std::filesystem::path& project_file)> on_created;
    const std::filesystem::path& Created() const { return created_; }
    const std::string& Status() const { return status_; }
    bool StatusIsError() const { return status_is_error_; }

private:
    std::filesystem::path created_;
    std::string status_;
    bool status_is_error_ = false;
};

} // namespace aether::editor
