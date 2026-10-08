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

constexpr char kContentPathPayload[] = "AETHER_CONTENT_PATH";

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
    operation_warning_.clear();
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
    DrawActionDialog();
    DrawReferenceViewer();
}

void ContentBrowserPanel::DrawToolbar() {
    if (ImGui::Button("Refresh")) Refresh();
    ImGui::SameLine();
    if (ImGui::Button("New Folder")) StartAction(Action::NewFolder, folder_);
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
    if (!operation_warning_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.70f, 0.20f, 1.0f), "%s", operation_warning_.c_str());
}

void ContentBrowserPanel::DrawTree() {
    std::string next_folder;
    bool change_folder = ImGui::Selectable("Content", folder_.empty());
    AcceptMoveDrop("");
    for (const std::string& f : folders_) {
        const usize depth = static_cast<usize>(std::count(f.begin(), f.end(), '/'));
        ImGui::PushID(f.c_str());
        ImGui::Indent(12.0f * static_cast<f32>(depth + 1));
        Badge(vcs_.available ? vcs_.OfLocalFolder(f) : VcsState::Clean);
        ImGui::SameLine();
        if (ImGui::Selectable(stdfs::path(f).filename().string().c_str(), folder_ == f)) {
            next_folder = f;
            change_folder = true;
        }
        AcceptMoveDrop(f);
        ImGui::Unindent(12.0f * static_cast<f32>(depth + 1));
        ImGui::PopID();
    }
    if (change_folder) OpenFolder(next_folder);
}

void ContentBrowserPanel::DrawListing() {
    ImGui::TextDisabled("Content/%s", folder_.c_str());
    if (!folder_.empty() && ImGui::Selectable("..")) OpenParent();
    std::string asset_to_open;
    for (usize i = 0; i < rows_.size(); ++i) {
        const assets::ContentEntry e = rows_[i];
        ImGui::PushID(static_cast<int>(i));
        Badge(e.vcs);
        ImGui::SameLine();
        const std::string label = e.is_folder ? e.name + "/" : e.name;
        const bool activated = ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) &&
                               ImGui::IsMouseDoubleClicked(0);
        const bool open_folder = activated && e.is_folder;
        if (activated && !e.is_folder) asset_to_open = e.path;
        if (ImGui::BeginPopupContextItem("content_entry_context")) {
            if (ImGui::MenuItem("Rename")) StartAction(Action::Rename, e.path);
            if (ImGui::MenuItem("Move")) StartAction(Action::Move, e.path);
            if (e.is_folder && ImGui::MenuItem("New Subfolder")) StartAction(Action::NewFolder, e.path);
            if (!e.is_folder && e.asset && ImGui::BeginMenu("References")) {
                if (ImGui::MenuItem("Show Dependencies")) {
                    reference_guid_ = e.asset->guid;
                    reference_direction_ = assets::ReferenceDirection::Dependencies;
                    references_open_ = true;
                }
                if (ImGui::MenuItem("Show Referencers")) {
                    reference_guid_ = e.asset->guid;
                    reference_direction_ = assets::ReferenceDirection::Referencers;
                    references_open_ = true;
                }
                ImGui::EndMenu();
            }
            ImGui::EndPopup();
        }
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(kContentPathPayload, e.path.c_str(), e.path.size() + 1);
            ImGui::TextUnformatted(label.c_str());
            ImGui::EndDragDropSource();
        }
        if (e.is_folder) AcceptMoveDrop(e.path);
        if (!e.is_folder && e.asset) {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 80.0f);
            ImGui::TextDisabled("%s", e.asset->importer.c_str());
        }
        ImGui::PopID();
        if (open_folder) {
            OpenFolder(e.path);
            return;
        }
    }
    if (rows_.empty()) ImGui::TextDisabled(changed_only ? "Nothing here has changed." : "Empty.");
    if (!asset_to_open.empty()) {
        if (ExtensionRegistry* ext = ExtensionRegistry::Active()) {
            ext->OpenAsset(project_file_.parent_path() / "Content" / asset_to_open);
        }
    }
}

void ContentBrowserPanel::StartAction(Action action, const std::string& source) {
    action_ = action;
    action_popup_pending_ = true;
    action_source_ = source;
    action_value_.clear();
    action_error_.clear();
    if (action == Action::Rename) action_value_ = stdfs::path(source).filename().string();
    if (action == Action::Move) action_value_ = source;
}

void ContentBrowserPanel::AcceptMoveDrop(const std::string& destination_folder) {
    if (!ImGui::BeginDragDropTarget()) return;
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kContentPathPayload)) {
        if (payload->Data && payload->DataSize > 1 && static_cast<const char*>(payload->Data)[payload->DataSize - 1] == '\0') {
            const std::string source(static_cast<const char*>(payload->Data));
            const std::string parent = stdfs::path(source).parent_path().generic_string();
            const bool into_self = destination_folder == source ||
                                   (destination_folder.size() > source.size() && destination_folder.compare(0, source.size(), source) == 0 &&
                                    destination_folder[source.size()] == '/');
            if (source != destination_folder && parent != destination_folder && !into_self) {
                StartAction(Action::Move, source);
                action_value_ = (stdfs::path(destination_folder) / stdfs::path(source).filename()).generic_string();
            }
        }
    }
    ImGui::EndDragDropTarget();
}

void ContentBrowserPanel::RebaseOpenFolder(const std::string& old_path, const std::string& new_path) {
    if (folder_ == old_path) {
        folder_ = new_path;
    } else if (folder_.size() > old_path.size() && folder_.compare(0, old_path.size(), old_path) == 0 && folder_[old_path.size()] == '/') {
        folder_ = new_path + folder_.substr(old_path.size());
    }
}

void ContentBrowserPanel::ApplyAction() {
    if (!database_) return;
    bool ok = false;
    std::string new_path;
    std::string warning;
    if (action_ == Action::NewFolder) {
        ok = assets::CreateContentFolder(*database_, action_source_, action_value_, &action_error_);
    } else if (action_ == Action::Rename) {
        std::filesystem::path path(action_source_);
        std::string new_name = action_value_;
        std::error_code ec;
        if (!stdfs::is_directory(database_->ContentRoot() / action_source_, ec) && stdfs::path(action_value_).extension().empty()) {
            new_name += path.extension().string();
        }
        new_path = (path.parent_path() / new_name).generic_string();
        const assets::MoveResult result = assets::RenameContent(*database_, action_source_, action_value_);
        ok = result.ok;
        action_error_ = result.error;
        warning = result.error;
    } else if (action_ == Action::Move) {
        new_path = stdfs::path(action_value_).lexically_normal().generic_string();
        const assets::MoveResult result = assets::MoveContent(*database_, action_source_, action_value_);
        ok = result.ok;
        action_error_ = result.error;
        warning = result.error;
    }
    if (!ok) return;
    if (action_ == Action::Rename || action_ == Action::Move) RebaseOpenFolder(action_source_, new_path);
    Refresh();
    operation_warning_ = std::move(warning);
    action_ = Action::None;
    ImGui::CloseCurrentPopup();
}

void ContentBrowserPanel::DrawActionDialog() {
    if (action_ == Action::None) return;
    if (action_popup_pending_) {
        ImGui::OpenPopup("Content Browser Action");
        action_popup_pending_ = false;
    }
    bool open = true;
    if (ImGui::BeginPopupModal("Content Browser Action", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        const char* hint = action_ == Action::Rename ? "New name" : action_ == Action::Move ? "Content-relative destination" : "Folder name";
        ImGui::TextUnformatted(action_ == Action::Rename ? "Rename" : action_ == Action::Move ? "Move" : "Create folder");
        ImGui::SetNextItemWidth(360.0f);
        ImGui::InputTextWithHint("##action_value", hint, &action_value_);
        if (!action_error_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", action_error_.c_str());
        if (ImGui::Button("Apply")) ApplyAction();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            action_ = Action::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (!open) action_ = Action::None;
}

void ContentBrowserPanel::DrawReferenceViewer() {
    if (!references_open_ || !database_) return;
    bool open = references_open_;
    if (ImGui::Begin("Asset References", &open)) {
        ImGui::TextDisabled("%s", database_->Find(reference_guid_) ? database_->Find(reference_guid_)->path.c_str() : "Unknown asset");
        const std::vector<assets::ReferenceNode> nodes = assets::CollectReferences(*database_, reference_guid_, reference_direction_);
        for (const assets::ReferenceNode& node : nodes) {
            ImGui::Indent(14.0f * static_cast<f32>(node.depth));
            ImGui::BulletText("%s%s", node.path.c_str(), node.repeated ? " (already shown)" : "");
            ImGui::Unindent(14.0f * static_cast<f32>(node.depth));
        }
        if (nodes.empty()) ImGui::TextDisabled("No references.");
    }
    ImGui::End();
    references_open_ = open;
}

} // namespace aether::editor
