#include "content_browser_panel.h"

#include "aether/project/project.h"
#include "ui/extensions.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>

namespace aether::editor {

namespace stdfs = std::filesystem;
using assets::VcsState;

namespace {

ImVec4 StateColor(VcsState s) {
    switch (s) {
    case VcsState::Modified: return ImVec4(1.0f, 0.70f, 0.20f, 1.0f);
    case VcsState::Added: return ImVec4(0.40f, 0.80f, 0.40f, 1.0f);
    case VcsState::Untracked: return ImVec4(0.50f, 0.70f, 1.0f, 1.0f);
    case VcsState::Renamed: return ImVec4(0.70f, 0.60f, 1.0f, 1.0f);
    case VcsState::Deleted: return ImVec4(1.0f, 0.40f, 0.40f, 1.0f);
    case VcsState::Conflicted: return ImVec4(1.0f, 0.20f, 0.80f, 1.0f);
    case VcsState::Clean: break;
    }
    return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
}

void Badge(VcsState s) {
    char text[2] = {assets::VcsStateLetter(s), 0};
    ImGui::TextColored(StateColor(s), "%s", s == VcsState::Clean ? " " : text);
    if (s != VcsState::Clean && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", assets::VcsStateName(s));
}

} // namespace

ContentBrowserPanel::ContentBrowserPanel(stdfs::path project_file) : project_file_(std::move(project_file)) { Refresh(); }

bool ContentBrowserPanel::Refresh() {
    error_.clear();
    ProjectSettings settings;
    if (!LoadProject(project_file_, settings, &error_)) {
        database_.reset();
        rows_.clear();
        folders_.clear();
        return false;
    }
    const ProjectPaths paths = ProjectPaths::ForFile(project_file_);
    database_ = std::make_unique<assets::AssetDatabase>(paths.content);
    database_->Scan();
    if (!vcs_injected_) vcs_ = assets::QueryGitStatus(paths.content);
    Rebuild();
    return true;
}

void ContentBrowserPanel::SetVcsStatus(assets::VcsStatus status) {
    vcs_ = std::move(status);
    vcs_injected_ = true;
    Rebuild();
}

void ContentBrowserPanel::OpenFolder(const std::string& folder) {
    folder_ = folder;
    Rebuild();
}

void ContentBrowserPanel::OpenParent() {
    const usize slash = folder_.find_last_of('/');
    OpenFolder(slash == std::string::npos ? std::string() : folder_.substr(0, slash));
}

void ContentBrowserPanel::Rebuild() {
    rows_.clear();
    folders_.clear();
    if (!database_) return;
    folders_ = assets::ContentFolders(*database_);
    assets::ContentQuery query;
    query.folder = folder_;
    query.search = search;
    rows_ = assets::ListContent(*database_, query);
    assets::ApplyVcsStatus(rows_, vcs_);
    if (changed_only) {
        rows_.erase(std::remove_if(rows_.begin(), rows_.end(), [](const assets::ContentEntry& e) { return e.vcs == VcsState::Clean; }),
                    rows_.end());
    }
}

usize ContentBrowserPanel::ChangedRows() const {
    return static_cast<usize>(std::count_if(rows_.begin(), rows_.end(), [](const assets::ContentEntry& e) { return e.vcs != VcsState::Clean; }));
}

void ContentBrowserPanel::Draw() {
    if (!database_) {
        ImGui::TextDisabled("%s", error_.empty() ? "No project is open." : error_.c_str());
        return;
    }
    DrawToolbar();
    ImGui::Separator();
    if (ImGui::BeginChild("##folders", ImVec2(200, 0), ImGuiChildFlags_Borders)) DrawTree();
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##listing", ImVec2(0, 0), ImGuiChildFlags_Borders)) DrawListing();
    ImGui::EndChild();
}

void ContentBrowserPanel::DrawToolbar() {
    if (ImGui::Button("Refresh")) Refresh();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    if (ImGui::InputTextWithHint("##search", "Search", &search)) Rebuild();
    ImGui::SameLine();
    if (ImGui::Checkbox("Changed only", &changed_only)) Rebuild();
    ImGui::SameLine();
    if (vcs_.available) {
        ImGui::TextDisabled("git: %zu changed file%s", vcs_.ChangedCount(), vcs_.ChangedCount() == 1 ? "" : "s");
    } else {
        ImGui::TextDisabled("not under version control");
        if (!vcs_.error.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", vcs_.error.c_str());
    }
}

void ContentBrowserPanel::DrawTree() {
    if (ImGui::Selectable("Content", folder_.empty())) OpenFolder("");
    for (const std::string& f : folders_) {
        const usize depth = static_cast<usize>(std::count(f.begin(), f.end(), '/'));
        ImGui::PushID(f.c_str());
        ImGui::Indent(12.0f * static_cast<f32>(depth + 1));
        Badge(vcs_.available ? vcs_.OfLocalFolder(f) : VcsState::Clean);
        ImGui::SameLine();
        if (ImGui::Selectable(stdfs::path(f).filename().string().c_str(), folder_ == f)) OpenFolder(f);
        ImGui::Unindent(12.0f * static_cast<f32>(depth + 1));
        ImGui::PopID();
    }
}

void ContentBrowserPanel::DrawListing() {
    ImGui::TextDisabled("Content/%s", folder_.c_str());
    if (!folder_.empty() && ImGui::Selectable("..")) OpenParent();
    for (usize i = 0; i < rows_.size(); ++i) {
        const assets::ContentEntry& e = rows_[i];
        ImGui::PushID(static_cast<int>(i));
        Badge(e.vcs);
        ImGui::SameLine();
        const std::string label = e.is_folder ? e.name + "/" : e.name;
        if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) && ImGui::IsMouseDoubleClicked(0)) {
            if (e.is_folder) {
                OpenFolder(e.path);
            } else if (ExtensionRegistry* ext = ExtensionRegistry::Active()) {
                ext->OpenAsset(project_file_.parent_path() / "Content" / e.path);
            }
        }
        if (!e.is_folder && e.asset) {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 80.0f);
            ImGui::TextDisabled("%s", e.asset->importer.c_str());
        }
        ImGui::PopID();
    }
    if (rows_.empty()) ImGui::TextDisabled(changed_only ? "Nothing here has changed." : "Empty.");
}

} // namespace aether::editor
