#include "content_browser_panel.h"

#include "aether/project/project.h"
#include "content/content_drag.h"
#include "ui/extensions.h"
#include "ui/image_texture.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <unordered_set>

namespace aether::editor {

namespace stdfs = std::filesystem;
using assets::VcsState;

namespace {

constexpr f32 kTile = 96.0f;
constexpr f32 kTileGap = 12.0f;

std::string LowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}

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

// The fill of a tile with no picture: a folder or an asset kind, so the grid reads by colour as well as by name.
ImU32 KindColor(const assets::ContentEntry& entry) {
    if (entry.is_folder) return IM_COL32(96, 86, 58, 255);
    const std::string importer = entry.asset ? entry.asset->importer : std::string();
    if (importer == "Texture") return IM_COL32(58, 78, 112, 255);
    if (importer == "Material") return IM_COL32(96, 66, 108, 255);
    if (importer == "Model" || importer == "Mesh") return IM_COL32(58, 96, 78, 255);
    if (importer == "Scene" || importer == "Prefab") return IM_COL32(78, 70, 108, 255);
    if (importer == "Script") return IM_COL32(96, 76, 52, 255);
    if (importer == "Animation" || importer == "AnimationGraph") return IM_COL32(88, 84, 108, 255);
    if (importer == "Audio" || importer == "AudioCue" || importer == "AudioMixer") return IM_COL32(58, 92, 96, 255);
    return IM_COL32(66, 66, 66, 255);
}

// `text` cut with "..." to fit `width` pixels at the current font size.
std::string Elide(const std::string& text, f32 width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
    std::string cut = text;
    while (!cut.empty() && ImGui::CalcTextSize((cut + "...").c_str()).x > width) cut.pop_back();
    return cut + "...";
}

} // namespace

ContentBrowserPanel::ContentBrowserPanel(stdfs::path project_file)
    : project_file_(std::move(project_file)), thumbnails_(project_file_) {
    Refresh();
}

ContentBrowserPanel::~ContentBrowserPanel() = default;

bool ContentBrowserPanel::Refresh() {
    error_.clear();
    operation_warning_.clear();
    ProjectSettings settings;
    if (!LoadProject(project_file_, settings, &error_)) {
        database_.reset();
        rows_.clear();
        folders_.clear();
        issues_.clear();
        return false;
    }
    const ProjectPaths paths = ProjectPaths::ForFile(project_file_);
    auto database = std::make_shared<assets::AssetDatabase>(paths.content);
    database->Scan();
    // Phase 43.6: validation runs with the scan, so the panel can show what is wrong with
    // the assets without a second pass over them.
    issues_ = assets::ValidateAssets(*database);
    database_ = std::move(database);
    thumbnails_.SetDatabase(database_);
    if (!vcs_injected_) vcs_ = assets::QueryGitStatus(paths.content);
    Rebuild();
    return true;
}

void ContentBrowserPanel::SetVcsStatus(assets::VcsStatus status) {
    vcs_ = std::move(status);
    vcs_injected_ = true;
    Rebuild();
}

bool ContentBrowserPanel::Commit(const std::string& message, std::string* error) {
    if (!database_) {
        if (error != nullptr) *error = "No project is open.";
        return false;
    }
    // Stage and commit from the content root: that is the folder the badges describe, and it is
    // inside the project's repository.
    const std::filesystem::path content = database_->ContentRoot();
    if (!assets::GitStageAll(content, error)) return false;
    if (!assets::GitCommit(content, message, error)) return false;
    Refresh(); // the badges and the issue list describe the new state
    return true;
}

void ContentBrowserPanel::OpenFolder(const std::string& folder) {
    if (folder_ != folder) {
        selection_.clear(); // a selection belongs to the listing it was made in
        selection_anchor_.clear();
    }
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
    chips_.clear();
    base_count_ = 0;
    if (!database_) return;
    folders_ = assets::ContentFolders(*database_);
    assets::ContentQuery query;
    query.folder = folder_;
    query.search = search;
    query.recursive = scope_recursive;
    rows_ = assets::ListContent(*database_, query);
    assets::ApplyVcsStatus(rows_, vcs_);
    base_count_ = rows_.size();

    // The chips describe what is in scope before they filter it; a selected chip stays listed even after its entries are
    // filtered out, so it can be switched off again.
    std::map<std::string, usize> counts;
    for (const assets::ContentEntry& e : rows_) {
        if (e.is_folder || e.asset == nullptr || e.asset->importer.empty()) continue;
        ++counts[e.asset->importer];
    }
    for (const std::string& selected : type_filter) counts.emplace(selected, 0);
    for (const auto& [importer, count] : counts) chips_.push_back({importer, count});

    const std::unordered_set<std::string> selected_types(type_filter.begin(), type_filter.end());
    rows_.erase(std::remove_if(rows_.begin(), rows_.end(),
                               [&](const assets::ContentEntry& e) {
                                   const bool wrong_type = !selected_types.empty() && !e.is_folder &&
                                                           (e.asset == nullptr || selected_types.count(e.asset->importer) == 0);
                                   return wrong_type || (changed_only && e.vcs == VcsState::Clean);
                               }),
                rows_.end());

    const auto key = [&](const assets::ContentEntry& e) {
        switch (sort_column) {
        case SortColumn::Name: return LowerAscii(e.name);
        case SortColumn::Type: return e.asset != nullptr ? LowerAscii(e.asset->importer) : std::string();
        }
        return LowerAscii(e.name);
    };
    std::stable_sort(rows_.begin(), rows_.end(), [&](const assets::ContentEntry& a, const assets::ContentEntry& b) {
        if (a.is_folder != b.is_folder) return a.is_folder; // folders stay on top, always by name
        if (a.is_folder) return LowerAscii(a.name) < LowerAscii(b.name);
        const std::string left = key(a), right = key(b);
        if (left != right) return sort_descending ? left > right : left < right;
        return a.path < b.path;
    });
}

void ContentBrowserPanel::ClearFilters() {
    search.clear();
    type_filter.clear();
    changed_only = false;
    Rebuild();
}

void ContentBrowserPanel::SortBy(SortColumn column) {
    if (sort_column == column) {
        sort_descending = !sort_descending;
    } else {
        sort_column = column;
        sort_descending = false;
    }
    Rebuild();
}

usize ContentBrowserPanel::ChangedRows() const {
    return static_cast<usize>(std::count_if(rows_.begin(), rows_.end(), [](const assets::ContentEntry& e) { return e.vcs != VcsState::Clean; }));
}

void ContentBrowserPanel::Draw() {
    if (!database_) {
        ImGui::TextDisabled("%s", error_.empty() ? "No project is open." : error_.c_str());
        return;
    }
    thumbnails_.Pump(); // pictures the worker finished since the last frame
    DrawToolbar();
    ImGui::Separator();
    if (ImGui::BeginChild("##folders", ImVec2(200, 0), ImGuiChildFlags_Borders)) DrawTree();
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##listing", ImVec2(0, 0), ImGuiChildFlags_Borders)) DrawListing();
    ImGui::EndChild();
    DrawActionDialog();
    DrawReferenceViewer();
    DrawValidation();
}

void ContentBrowserPanel::DrawToolbar() {
    if (ImGui::Button("Refresh")) Refresh();
    ImGui::SameLine();
    if (ImGui::Button("Create")) ImGui::OpenPopup("##create_menu");
    if (ImGui::BeginPopup("##create_menu")) {
        DrawCreateMenu(folder_);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("New Folder")) StartAction(Action::NewFolder, folder_);
    ImGui::SameLine();
    if (ImGui::Button("Duplicate") && !selection_.empty()) pending_ = Pending::Duplicate;
    ImGui::SameLine();
    if (ImGui::Button("Delete") && !selection_.empty()) pending_ = Pending::Delete;
    ImGui::SameLine();
    if (!selection_.empty()) {
        ImGui::TextDisabled("%zu selected", selection_.size());
    } else if (!search.empty() || !type_filter.empty() || changed_only) {
        ImGui::TextDisabled("%zu of %zu item%s", rows_.size(), base_count_, rows_.size() == 1 ? "" : "s");
    } else {
        ImGui::TextDisabled("%zu item%s", rows_.size(), rows_.size() == 1 ? "" : "s");
    }
    ImGui::SameLine();
    // The grid shows a thumbnail (or a type tile) per asset; the list is the denser view.
    if (ImGui::Button(grid_view ? "[Grid]" : "Grid")) grid_view = true;
    ImGui::SameLine();
    if (ImGui::Button(grid_view ? "List" : "[List]")) grid_view = false;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    if (search_focus_) {
        ImGui::SetKeyboardFocusHere(); // Ctrl+F from the listing lands here one frame later
        search_focus_ = false;
    }
    if (ImGui::InputTextWithHint("##search", "Search (type: path:)", &search)) Rebuild();
    ImGui::SameLine();
    if (ImGui::Checkbox("Subfolders", &scope_recursive)) Rebuild();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Include the subfolders' entries; the entries are counted like the\nlisting shows them (a search always looks in them)");
    ImGui::SameLine();
    if (ImGui::Checkbox("Changed only", &changed_only)) Rebuild();
    ImGui::SameLine();
    if (vcs_.available) {
        ImGui::TextDisabled("git: %zu changed file%s", vcs_.ChangedCount(), vcs_.ChangedCount() == 1 ? "" : "s");
        // Phase 43.7: record the editor's own edits without a terminal. The message box is plain
        // state (see `commit_message`), so a host or a test can drive the commit directly.
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220);
        ImGui::InputTextWithHint("##commit_message", "Commit message", &commit_message);
        ImGui::SameLine();
        if (ImGui::Button("Commit") && !commit_message.empty()) {
            std::string error;
            if (Commit(commit_message, &error)) {
                commit_message.clear();
            } else {
                operation_warning_ = error;
            }
        }
        if (!operation_warning_.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", operation_warning_.c_str());
        }
    } else {
        ImGui::TextDisabled("not under version control");
        if (!vcs_.error.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", vcs_.error.c_str());
    }
    // Phase 43.6: only shown when there is something to say, so a healthy project has no
    // extra button. The list itself is a window the host can also open directly.
    if (!issues_.empty()) {
        ImGui::SameLine();
        const std::string label = "Validation (" + std::to_string(issues_.size()) + ")";
        if (ImGui::Button(label.c_str())) validation_open = true;
    }
    if (!operation_warning_.empty()) ImGui::TextColored(ImVec4(1.0f, 0.70f, 0.20f, 1.0f), "%s", operation_warning_.c_str());
    DrawTypeChips();
}

void ContentBrowserPanel::DrawTypeChips() {
    if (chips_.empty()) return; // no typed assets in scope: nothing to filter
    std::string toggled;
    bool all = false;
    if (ImGui::SmallButton(type_filter.empty() ? "[All]" : "All")) all = true;
    ImGui::SameLine();
    ImGui::TextDisabled("Type:");
    for (const TypeChip& chip : chips_) {
        ImGui::SameLine();
        const bool on = std::find(type_filter.begin(), type_filter.end(), chip.importer) != type_filter.end();
        const std::string label = chip.importer + " (" + std::to_string(chip.count) + ")";
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.52f, 0.80f, 1.0f));
        if (ImGui::SmallButton(label.c_str())) toggled = chip.importer;
        if (on) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s entries%s", chip.importer.c_str(), on ? " -- click to stop filtering by it" : "");
    }
    if (all && !type_filter.empty()) {
        type_filter.clear();
        Rebuild();
    } else if (!toggled.empty()) {
        const auto found = std::find(type_filter.begin(), type_filter.end(), toggled);
        if (found != type_filter.end()) {
            type_filter.erase(found);
        } else {
            type_filter.push_back(toggled);
        }
        Rebuild(); // after the loop: chips_ is rebuilt
    }
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

bool ContentBrowserPanel::DrawBreadcrumbs() {
    bool changed = false;
    std::string target;
    if (folder_.empty()) {
        ImGui::TextUnformatted("Content");
    } else if (ImGui::SmallButton("Content")) {
        changed = true;
    }
    std::string prefix;
    usize start = 0;
    while (start < folder_.size()) {
        const usize slash = folder_.find('/', start);
        const std::string segment = folder_.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        prefix = prefix.empty() ? segment : prefix + "/" + segment;
        const std::string crumb = prefix;
        ImGui::SameLine();
        ImGui::TextDisabled("/");
        ImGui::SameLine();
        ImGui::PushID(crumb.c_str());
        if (slash == std::string::npos) {
            ImGui::TextUnformatted(segment.c_str()); // where we are, not a way back
        } else if (ImGui::SmallButton(segment.c_str())) {
            changed = true;
            target = crumb;
        }
        ImGui::PopID();
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    if (scope_recursive) {
        ImGui::SameLine();
        ImGui::TextDisabled("(with subfolders)");
    }
    if (changed) OpenFolder(target);
    return changed;
}

void ContentBrowserPanel::DrawListing() {
    if (DrawBreadcrumbs()) return; // the folder changed: the rest of this frame's listing is stale
    tiles_.clear();
    std::string folder_to_open, asset_to_open;
    if (grid_view) {
        DrawGrid(folder_to_open, asset_to_open);
    } else {
        DrawRows(folder_to_open, asset_to_open);
    }
    DrawBoxSelect(); // after the items: a press belongs to the box only when no tile claimed it
    if (box_active_) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const ImVec2 low(std::min(box_x0_, mouse.x), std::min(box_y0_, mouse.y));
        const ImVec2 high(std::max(box_x0_, mouse.x), std::max(box_y0_, mouse.y));
        ImGui::GetWindowDrawList()->AddRectFilled(low, high, IM_COL32(255, 170, 60, 40));
        ImGui::GetWindowDrawList()->AddRect(low, high, IM_COL32(255, 170, 60, 180), 0.0f, 0, 1.5f);
    }
    if (ImGui::BeginPopupContextWindow("##content_list", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::MenuItem("Select All", "Ctrl+A")) SelectAll();
        if (ImGui::MenuItem("New Folder")) StartAction(Action::NewFolder, folder_);
        if (ImGui::BeginMenu("Create")) {
            DrawCreateMenu(folder_);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, !selection_.empty())) pending_ = Pending::Duplicate;
        if (ImGui::MenuItem("Delete", "Del", false, !selection_.empty())) pending_ = Pending::Delete;
        ImGui::EndPopup();
    }
    if (rows_.empty()) {
        if (!search.empty() || !type_filter.empty() || changed_only) {
            ImGui::TextDisabled("No entries match.");
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear filters")) ClearFilters();
            if (!search.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("(a search looks in subfolders too)");
            }
        } else {
            ImGui::TextDisabled("Empty.");
        }
    }
    DrawSelectionKeys();
    if (commit_rename_) {
        commit_rename_ = false;
        CommitRename();
    }
    if (!folder_to_open.empty()) {
        OpenFolder(folder_to_open);
        return;
    }
    if (!asset_to_open.empty()) {
        if (ExtensionRegistry* ext = ExtensionRegistry::Active()) {
            ext->OpenAsset(project_file_.parent_path() / "Content" / asset_to_open);
        }
    }
    ApplyPending(); // after the listing has drawn: an action must not clear rows_ under its own loop
}

void ContentBrowserPanel::DrawEntryMenu(const assets::ContentEntry& e) {
    if (!ImGui::BeginPopupContextItem("content_entry_context")) return;
    const bool multiple = selection_.size() > 1 && IsSelected(e.path);
    if (ImGui::MenuItem("Rename", "F2", false, !multiple)) BeginRename(e.path);
    if (ImGui::MenuItem(multiple ? "Duplicate selection" : "Duplicate", "Ctrl+D")) {
        if (!IsSelected(e.path)) Select(e.path);
        pending_ = Pending::Duplicate;
    }
    if (ImGui::MenuItem(multiple ? "Delete selection" : "Delete", "Del")) {
        if (!IsSelected(e.path)) Select(e.path);
        pending_ = Pending::Delete;
    }
    if (ImGui::MenuItem(multiple ? "Move selection..." : "Move...")) {
        if (!IsSelected(e.path)) Select(e.path);
        StartAction(Action::Move);
    }
    if (e.is_folder) {
        if (ImGui::BeginMenu("Create")) {
            DrawCreateMenu(e.path);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("New Subfolder")) StartAction(Action::NewFolder, e.path);
    }
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

void ContentBrowserPanel::DrawCreateMenu(const std::string& folder) {
    ExtensionRegistry* ext = ExtensionRegistry::Active();
    if (ext == nullptr || ext->AssetTypes().empty()) {
        ImGui::MenuItem("No asset types registered", nullptr, false, false);
        return;
    }
    for (const ExtensionAssetType& type : ext->AssetTypes()) {
        if (ImGui::MenuItem(type.name.c_str())) {
            pending_ = Pending::Create;
            pending_create_extension_ = type.extension;
            pending_create_folder_ = folder;
        }
    }
}

void ContentBrowserPanel::DrawBoxSelect() {
    if (!grid_view || !renaming_.empty() || !ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) return;
    ImGuiIO& io = ImGui::GetIO();
    if (!box_active_) {
        if (ImGui::IsMouseClicked(0) && !ImGui::IsAnyItemHovered()) {
            box_active_ = true;
            box_x0_ = io.MousePos.x;
            box_y0_ = io.MousePos.y;
            box_add_ = io.KeyCtrl;
            if (!box_add_) ClearSelection();
        }
        return;
    }
    if (!ImGui::IsMouseDown(0)) {
        const float x1 = io.MousePos.x, y1 = io.MousePos.y;
        if (std::abs(x1 - box_x0_) > 3.0f || std::abs(y1 - box_y0_) > 3.0f) {
            SelectInRect(std::min(box_x0_, x1), std::min(box_y0_, y1), std::max(box_x0_, x1), std::max(box_y0_, y1), box_add_);
        }
        box_active_ = false;
    }
}

void ContentBrowserPanel::DrawSelectionKeys() {
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;
    ImGuiIO& io = ImGui::GetIO();
    if (renaming_.empty() && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F)) search_focus_ = true;
    if (!renaming_.empty() || io.WantTextInput) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Delete)) pending_ = Pending::Delete;
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) pending_ = Pending::Duplicate;
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A)) SelectAll();
    if (ImGui::IsKeyPressed(ImGuiKey_F2) && selection_.size() == 1) BeginRename(selection_[0]);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ClearSelection();
}

bool ContentBrowserPanel::InlineRename(const assets::ContentEntry& e) {
    if (renaming_ != e.path) return false;
    ImGui::SetNextItemWidth(kTile);
    if (rename_focus_) {
        ImGui::SetKeyboardFocusHere();
        rename_focus_ = false;
    }
    if (ImGui::InputText("##rename", &rename_buffer_, ImGuiInputTextFlags_EnterReturnsTrue)) commit_rename_ = true;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) renaming_.clear();
    return true;
}

void ContentBrowserPanel::DrawGrid(std::string& folder_to_open, std::string& asset_to_open) {
    const f32 columns_width = std::max(ImGui::GetContentRegionAvail().x, kTile);
    const int columns = std::max(1, static_cast<int>((columns_width + kTileGap) / (kTile + kTileGap)));
    int column = 0;
    for (usize i = 0; i < rows_.size(); ++i) {
        const assets::ContentEntry e = rows_[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::BeginGroup();
        const bool renaming = renaming_ == e.path;
        bool hovered_tile = false;
        bool activated = false;
        if (!renaming) {
            ImGui::InvisibleButton("##tile", ImVec2(kTile, kTile));
            hovered_tile = ImGui::IsItemHovered();
            if (hovered_tile) {
                ImGui::SetTooltip("%s\n%s", e.path.c_str(), e.is_folder ? "Folder" : (e.asset ? e.asset->importer.c_str() : "Asset"));
            }
            const ImGuiIO& io = ImGui::GetIO();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) Select(e.path, io.KeyCtrl, io.KeyShift);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !IsSelected(e.path)) Select(e.path);
            activated = !box_active_ && hovered_tile && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
            DrawEntryMenu(e);
            if (ImGui::BeginDragDropSource()) {
                const bool many = selection_.size() > 1 && IsSelected(e.path);
                const std::string label = many ? std::to_string(selection_.size()) + " assets"
                                               : (e.is_folder ? e.name + "/" : e.name);
                ImGui::SetDragDropPayload(kContentPathPayload, e.path.c_str(), e.path.size() + 1);
                ImGui::TextUnformatted(label.c_str());
                ImGui::EndDragDropSource();
            }
        } else {
            ImGui::Dummy(ImVec2(kTile, kTile)); // keeps the tile's place while the name is edited
        }
        const ImVec2 low = ImGui::GetItemRectMin(), high = ImGui::GetItemRectMax();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(low, high, KindColor(e), 6.0f);
        tiles_.push_back(TileRect{e.path, low.x, low.y, high.x, high.y});
        bool pictured = false;
        if (!e.is_folder && e.asset) {
            const ThumbnailStore::Picture picture = thumbnails_.Get(e);
            if (picture.image != nullptr) {
                TileTexture& shown = textures_[assets::ToString(e.asset->guid)];
                if (!shown.texture || shown.key != picture.key) { // a new version needs new pixels; a resized one needs a new texture
                    shown.texture = std::make_unique<ImageTexture>();
                    shown.key = picture.key;
                }
                if (!shown.texture->Valid()) {
                    shown.texture->Set(picture.image->width, picture.image->height, picture.image->rgba);
                }
                if (shown.texture->Valid()) {
                    const f32 width = static_cast<f32>(picture.image->width), height = static_cast<f32>(picture.image->height);
                    const f32 scale = std::min(kTile / width, kTile / height);
                    const ImVec2 fit(width * scale, height * scale);
                    const ImVec2 image_low((low.x + high.x - fit.x) * 0.5f, (low.y + high.y - fit.y) * 0.5f);
                    draw->AddImage(shown.texture->Ref(), image_low, ImVec2(image_low.x + fit.x, image_low.y + fit.y));
                    pictured = true;
                }
            }
        }
        if (!pictured && !renaming) { // no picture (yet, or for this kind): the kind's initial, on the kind's colour
            const std::string initial = e.name.empty() ? "?" : std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(e.name[0]))));
            const ImVec2 text = ImGui::CalcTextSize(initial.c_str());
            draw->AddText(ImVec2((low.x + high.x - text.x) * 0.5f, (low.y + high.y - text.y) * 0.5f), IM_COL32(225, 225, 225, 220), initial.c_str());
        }
        const bool selected = IsSelected(e.path);
        const ImU32 border = selected                    ? IM_COL32(255, 170, 60, 255)
                             : e.vcs != VcsState::Clean ? ImGui::GetColorU32(StateColor(e.vcs))
                                                        : IM_COL32(255, 255, 255, hovered_tile ? 120 : 40);
        draw->AddRect(low, high, border, 6.0f, 0, selected ? 2.5f : 1.0f);
        if (e.vcs != VcsState::Clean) {
            char letter[2] = {assets::VcsStateLetter(e.vcs), 0};
            draw->AddText(ImVec2(high.x - 12.0f, low.y + 3.0f), border, letter);
        }
        if (!InlineRename(e)) ImGui::TextUnformatted(Elide(e.is_folder ? e.name + "/" : e.name, kTile).c_str());
        ImGui::EndGroup();
        if (e.is_folder && !renaming) AcceptMoveDrop(e.path);
        if (activated) {
            if (e.is_folder) folder_to_open = e.path;
            else asset_to_open = e.path;
        }
        ImGui::PopID();
        if (++column % columns != 0 && i + 1 < rows_.size()) ImGui::SameLine(0, kTileGap);
    }
}

void ContentBrowserPanel::DrawRows(std::string& folder_to_open, std::string& asset_to_open) {
    // Clicking a column sorts by it; clicking the sorted one again flips the order.
    bool sort_clicked = false;
    SortColumn clicked = SortColumn::Name;
    const auto header_label = [&](const char* text, SortColumn column) {
        if (sort_column != column) return std::string(text);
        return std::string(text) + (sort_descending ? " v" : " ^");
    };
    const std::string name_label = header_label("Name", SortColumn::Name);
    if (ImGui::Selectable(name_label.c_str(), false, ImGuiSelectableFlags_None, ImVec2(240.0f, 0.0f))) {
        sort_clicked = true;
        clicked = SortColumn::Name;
    }
    ImGui::SameLine();
    const std::string type_label = header_label("Type", SortColumn::Type);
    if (ImGui::Selectable(type_label.c_str(), false, ImGuiSelectableFlags_None, ImVec2(140.0f, 0.0f))) {
        sort_clicked = true;
        clicked = SortColumn::Type;
    }
    ImGui::Separator();
    if (sort_clicked) SortBy(clicked);

    for (usize i = 0; i < rows_.size(); ++i) {
        const assets::ContentEntry e = rows_[i];
        ImGui::PushID(static_cast<int>(i));
        const bool renaming = renaming_ == e.path;
        if (renaming) {
            ImGui::SetNextItemWidth(240.0f);
            if (rename_focus_) {
                ImGui::SetKeyboardFocusHere();
                rename_focus_ = false;
            }
            if (ImGui::InputText("##rename", &rename_buffer_, ImGuiInputTextFlags_EnterReturnsTrue)) commit_rename_ = true;
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) renaming_.clear();
        } else {
            Badge(e.vcs);
            ImGui::SameLine();
            const std::string label = e.is_folder ? e.name + "/" : e.name;
            const bool selected = IsSelected(e.path);
            const bool clicked = ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick);
            const ImGuiIO& io = ImGui::GetIO();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) Select(e.path, io.KeyCtrl, io.KeyShift);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !selected) Select(e.path);
            if (clicked && ImGui::IsMouseDoubleClicked(0)) {
                if (e.is_folder) folder_to_open = e.path;
                else asset_to_open = e.path;
            }
            DrawEntryMenu(e);
            if (ImGui::BeginDragDropSource()) {
                const bool many = selection_.size() > 1 && selected;
                const std::string drag_label = many ? std::to_string(selection_.size()) + " assets" : label;
                ImGui::SetDragDropPayload(kContentPathPayload, e.path.c_str(), e.path.size() + 1);
                ImGui::TextUnformatted(drag_label.c_str());
                ImGui::EndDragDropSource();
            }
            if (e.is_folder) AcceptMoveDrop(e.path);
            if (!e.is_folder && e.asset) {
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 80.0f);
                ImGui::TextDisabled("%s", e.asset->importer.c_str());
            }
        }
        ImGui::PopID();
    }
}

bool ContentBrowserPanel::IsSelected(const std::string& path) const {
    return std::find(selection_.begin(), selection_.end(), path) != selection_.end();
}

void ContentBrowserPanel::Select(const std::string& path, bool toggle, bool extend) {
    const auto index_of = [&](const std::string& wanted) -> int {
        for (usize i = 0; i < rows_.size(); ++i) {
            if (rows_[i].path == wanted) return static_cast<int>(i);
        }
        return -1;
    };
    if (extend && !selection_anchor_.empty()) {
        const int anchor = index_of(selection_anchor_);
        const int index = index_of(path);
        if (anchor >= 0 && index >= 0) {
            const int low = std::min(anchor, index), high = std::max(anchor, index);
            selection_.clear();
            for (int i = low; i <= high; ++i) selection_.push_back(rows_[static_cast<usize>(i)].path);
            return;
        }
    }
    if (toggle) {
        const auto found = std::find(selection_.begin(), selection_.end(), path);
        if (found != selection_.end()) {
            selection_.erase(found);
        } else {
            selection_.push_back(path);
        }
        selection_anchor_ = path;
        return;
    }
    selection_.assign(1, path);
    selection_anchor_ = path;
}

void ContentBrowserPanel::SelectAll() {
    selection_.clear();
    for (const assets::ContentEntry& e : rows_) selection_.push_back(e.path);
    selection_anchor_ = selection_.empty() ? std::string() : selection_.front();
}

void ContentBrowserPanel::ClearSelection() {
    selection_.clear();
    selection_anchor_.clear();
}

void ContentBrowserPanel::SelectInRect(float x0, float y0, float x1, float y1, bool add) {
    std::vector<std::string> hits;
    for (const TileRect& tile : tiles_) {
        if (tile.x1 >= x0 && tile.x0 <= x1 && tile.y1 >= y0 && tile.y0 <= y1) hits.push_back(tile.path);
    }
    if (!add) {
        selection_ = std::move(hits);
    } else {
        for (std::string& hit : hits) {
            if (!IsSelected(hit)) selection_.push_back(std::move(hit));
        }
    }
    selection_anchor_ = selection_.empty() ? std::string() : selection_.front();
}

bool ContentBrowserPanel::MovePaths(const std::vector<std::string>& paths, const std::string& destination, std::string* error) {
    if (!database_ || paths.empty()) return false;
    std::vector<std::string> moved;
    std::string first_error;
    for (const std::string& path : paths) {
        const bool nested = std::any_of(paths.begin(), paths.end(), [&](const std::string& other) {
            return other != path && path.size() > other.size() && path.compare(0, other.size(), other) == 0 &&
                   path[other.size()] == '/';
        });
        if (nested) continue; // it goes with the folder above it
        const std::string parent = stdfs::path(path).parent_path().generic_string();
        if (parent == destination) {
            moved.push_back(path);
            continue;
        }
        const std::string target = destination.empty() ? stdfs::path(path).filename().string()
                                                       : destination + "/" + stdfs::path(path).filename().string();
        const assets::MoveResult result = assets::MoveContent(*database_, path, target);
        if (!result.ok) {
            if (first_error.empty()) first_error = result.error;
            continue;
        }
        moved.push_back(target);
        RebaseOpenFolder(path, target); // the open folder may have been this one, or below it
    }
    selection_ = moved;
    selection_anchor_ = moved.empty() ? std::string() : moved.front();
    Refresh();
    if (!first_error.empty()) {
        operation_warning_ = first_error;
        if (error != nullptr) *error = first_error;
    }
    return first_error.empty() && !moved.empty();
}

bool ContentBrowserPanel::MoveSelection(const std::string& destination, std::string* error) {
    return MovePaths(selection_, destination, error);
}

bool ContentBrowserPanel::DuplicateSelection(std::string* error) {
    if (!database_ || selection_.empty()) return false;
    std::vector<std::string> copies;
    std::string first_error;
    for (const std::string& path : selection_) {
        std::string created, message;
        if (assets::DuplicateContent(*database_, path, &created, &message)) {
            copies.push_back(created);
        } else if (first_error.empty()) {
            first_error = message;
        }
    }
    if (!copies.empty()) {
        selection_ = copies;
        selection_anchor_ = copies.front();
    }
    Refresh();
    if (!first_error.empty()) {
        operation_warning_ = first_error;
        if (error != nullptr) *error = first_error;
    }
    return first_error.empty() && !copies.empty();
}

bool ContentBrowserPanel::DeleteSelection(std::string* error) {
    if (!database_ || selection_.empty()) return false;
    const std::vector<std::string> paths = selection_;
    std::string message;
    usize deleted = 0;
    const bool ok = assets::DeleteContents(*database_, paths, &message, &deleted);
    if (ok) {
        selection_.clear();
        selection_anchor_.clear();
    }
    Refresh(); // after a partial failure too: the view must match the disk
    if (!ok) {
        operation_warning_ = message;
        if (error != nullptr) *error = message;
    }
    return ok;
}

void ContentBrowserPanel::BeginRename(const std::string& path) {
    if (!database_ || path.empty()) return;
    std::error_code ec;
    const bool folder = stdfs::is_directory(database_->ContentRoot() / path, ec);
    renaming_ = path;
    rename_buffer_ = folder ? stdfs::path(path).filename().string() : stdfs::path(path).stem().string();
    rename_focus_ = true;
}

void ContentBrowserPanel::CommitRename() {
    if (renaming_.empty()) return;
    const std::string path = renaming_;
    renaming_.clear();
    std::string error;
    if (!RenameEntry(path, rename_buffer_, &error) && !error.empty()) operation_warning_ = error;
}

bool ContentBrowserPanel::RenameEntry(const std::string& path, const std::string& new_name, std::string* error) {
    if (!database_ || path.empty()) return false;
    std::error_code ec;
    const bool folder = stdfs::is_directory(database_->ContentRoot() / path, ec);
    const std::string old_name = stdfs::path(path).filename().string();
    const std::string old_stem = stdfs::path(path).stem().string();
    if (new_name.empty() || new_name == old_name || new_name == old_stem) return false; // nothing typed
    const assets::MoveResult result = assets::RenameContent(*database_, path, new_name);
    if (!result.ok) {
        if (error != nullptr) *error = result.error;
        return false;
    }
    std::string final_name = new_name;
    if (!folder && stdfs::path(final_name).extension().empty()) final_name += stdfs::path(path).extension().string();
    const std::string parent = stdfs::path(path).parent_path().generic_string();
    const std::string created = parent.empty() ? final_name : parent + "/" + final_name;
    selection_.assign(1, created);
    selection_anchor_ = created;
    RebaseOpenFolder(path, created); // renaming the folder being shown follows it
    Refresh();
    return true;
}

void ContentBrowserPanel::ApplyPending() {
    const Pending pending = pending_;
    pending_ = Pending::None;
    std::string error;
    switch (pending) {
    case Pending::Delete: DeleteSelection(&error); break;
    case Pending::Duplicate: DuplicateSelection(&error); break;
    case Pending::Move: MoveSelection(pending_destination_, &error); break;
    case Pending::Create: {
        ExtensionRegistry* ext = ExtensionRegistry::Active();
        if (ext == nullptr || !database_) break;
        const ExtensionAssetType* type = ext->FindAssetTypeByExtension(pending_create_extension_);
        if (type == nullptr) break;
        std::string message;
        const stdfs::path file = ext->CreateAsset(*type, database_->ContentRoot() / pending_create_folder_, &message);
        if (file.empty()) {
            Refresh();
            operation_warning_ = message;
            break;
        }
        const std::string path = stdfs::relative(file, database_->ContentRoot()).generic_string();
        Refresh();
        selection_.assign(1, path);
        selection_anchor_ = path;
        BeginRename(path); // a new asset starts with its name ready to type
        break;
    }
    case Pending::None: break;
    }
}

void ContentBrowserPanel::StartAction(Action action, const std::string& source) {
    action_ = action;
    action_popup_pending_ = true;
    action_source_ = source;
    action_value_.clear();
    action_error_.clear();
    if (action == Action::Move) action_value_ = folder_; // a starting point; the whole selection moves there
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
                if (!IsSelected(source)) Select(source); // a plain drag moves just the tile under the cursor
                pending_ = Pending::Move; // the drop lands mid-loop; the batch runs after the listing has drawn
                pending_destination_ = destination_folder;
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
    if (action_ == Action::NewFolder) {
        ok = assets::CreateContentFolder(*database_, action_source_, action_value_, &action_error_);
        if (ok) {
            const std::string created = action_source_.empty() ? action_value_ : action_source_ + "/" + action_value_;
            Refresh();
            selection_.assign(1, created);
            selection_anchor_ = created;
        }
    } else if (action_ == Action::Move) {
        std::string error;
        ok = MoveSelection(stdfs::path(action_value_).lexically_normal().generic_string(), &error);
        action_error_ = error;
    }
    if (!ok) return;
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
        if (action_ == Action::Move) {
            ImGui::Text("%d item%s to a Content-relative folder", static_cast<int>(selection_.size()),
                        selection_.size() == 1 ? "" : "s");
            ImGui::SetNextItemWidth(360.0f);
            ImGui::InputTextWithHint("##action_value", "Content-relative destination", &action_value_);
        } else {
            ImGui::TextUnformatted("Create folder");
            ImGui::SetNextItemWidth(360.0f);
            ImGui::InputTextWithHint("##action_value", "Folder name", &action_value_);
        }
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

void ContentBrowserPanel::DrawValidation() {
    if (!validation_open) return;
    bool open = validation_open;
    if (ImGui::Begin("Asset Validation", &open)) {
        if (issues_.empty()) {
            ImGui::TextDisabled("No asset problems.");
        }
        for (const assets::AssetIssue& issue : issues_) {
            if (issue.kind == assets::AssetIssueKind::MissingSource) {
                ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.2f, 1.0f), "missing source: %s", issue.path.c_str());
            } else {
                ImGui::BulletText("%s refers to %s, which is gone", issue.path.c_str(),
                                  assets::ToString(issue.dependency).c_str());
            }
        }
    }
    ImGui::End();
    validation_open = open;
}

} // namespace aether::editor
