#include "packaging/new_project_panel.h"

#include <imgui.h>

#include <cstdio>
#include <cstdlib>

namespace aether::editor {

namespace stdfs = std::filesystem;

stdfs::path NewProjectPanel::DefaultLocation() {
#if defined(_WIN32)
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    return (home && *home ? stdfs::path(home) : stdfs::temp_directory_path()) / "AetherProjects";
}

NewProjectPanel::NewProjectPanel() : location(DefaultLocation().string()) {}

bool NewProjectPanel::Create(std::string* error) {
    std::string why;
    templates::ProjectTemplate const* t = templates::FindProjectTemplate(template_id);
    ProjectPaths paths;
    if (!t) {
        why = "Pick a template";
    } else if (location.empty()) {
        why = "Pick a folder for the project";
    } else if (!templates::CreateProjectFromTemplate(location, name, template_id, &paths, &why)) {
        // `why` has the reason.
    } else {
        created_ = paths.file;
        status_ = "Created " + name + " in " + paths.root.string();
        status_is_error_ = false;
        if (on_created) on_created(created_);
        return true;
    }
    status_ = why;
    status_is_error_ = true;
    if (error) *error = why;
    return false;
}

void NewProjectPanel::Draw() {
    const f32 list_w = 230.0f;
    if (ImGui::BeginChild("##templates", ImVec2(list_w, 0), ImGuiChildFlags_Borders)) {
        for (const templates::ProjectTemplate& t : templates::ProjectTemplates()) {
            ImGui::BeginDisabled(!t.available);
            if (ImGui::Selectable(t.name.c_str(), template_id == t.id)) template_id = t.id;
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", t.genre.c_str());
            if (!t.available && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s", t.unavailable_reason.c_str());
            }
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginGroup();
    if (const templates::ProjectTemplate* t = templates::FindProjectTemplate(template_id)) {
        ImGui::TextUnformatted(t->name.c_str());
        ImGui::TextWrapped("%s", t->description.c_str());
        if (!t->available) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", t->unavailable_reason.c_str());
        for (const std::string& f : t->features) ImGui::BulletText("%s", f.c_str());
    }
    ImGui::Separator();
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), "%s", name.c_str());
    if (ImGui::InputText("Name", buffer, sizeof(buffer))) name = buffer;
    std::snprintf(buffer, sizeof(buffer), "%s", location.c_str());
    if (ImGui::InputText("Location", buffer, sizeof(buffer))) location = buffer;
    const templates::ProjectTemplate* selected = templates::FindProjectTemplate(template_id);
    ImGui::BeginDisabled(!selected || !selected->available);
    if (ImGui::Button("Create Project")) Create();
    ImGui::EndDisabled();
    if (!status_.empty()) {
        ImGui::TextColored(status_is_error_ ? ImVec4(1, 0.4f, 0.3f, 1) : ImVec4(0.4f, 0.9f, 0.4f, 1), "%s", status_.c_str());
    }
    ImGui::EndGroup();
}

} // namespace aether::editor
