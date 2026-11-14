#include "world/scene_panel.h"

#include "content/content_drag.h"
#include "core/file_dialog.h"
#include "workspace/command_registry.h"
#include "ui/entity_inspector.h"
#include "world/content_drop.h"

#include "aether/core/log.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace aether::editor {

namespace {

constexpr const char* kEntityPayload = "AETHER_ENTITY";

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

void Copy(std::array<char, 128>& buffer, const std::string& text) {
    std::snprintf(buffer.data(), buffer.size(), "%s", text.c_str());
}

} // namespace

HierarchyIndex::HierarchyIndex(const SceneDocument& document) {
    const std::vector<Entity> entities = document.Entities();
    for (const Entity entity : entities) children_[Key(document.ParentOf(entity))].push_back(entity);
    for (auto& [parent, list] : children_) {
        std::stable_sort(list.begin(), list.end(), [&](Entity a, Entity b) {
            const std::string left = Lower(document.Name(a)), right = Lower(document.Name(b));
            return left != right ? left < right : a.index < b.index;
        });
    }
}

const std::vector<Entity>& HierarchyIndex::Children(Entity parent) const {
    const auto found = children_.find(Key(parent));
    return found == children_.end() ? none_ : found->second;
}

std::vector<Entity> MatchingEntities(const SceneDocument& document, const std::string& filter) {
    const std::string needle = Lower(filter);
    std::vector<Entity> out;
    for (const Entity entity : document.Entities())
        if (needle.empty() || Lower(document.Name(entity)).find(needle) != std::string::npos) out.push_back(entity);
    std::stable_sort(out.begin(), out.end(), [&](Entity a, Entity b) {
        const std::string left = Lower(document.Name(a)), right = Lower(document.Name(b));
        return left != right ? left < right : a.index < b.index;
    });
    return out;
}

ScenePanel::ScenePanel(std::filesystem::path content_root) : content_root_(std::move(content_root)), bounds_(content_root_) {
    document_.NewScene();
}

std::filesystem::path ScenePanel::Resolve(const std::string& path) const {
    const std::filesystem::path given(path);
    return given.is_absolute() ? given : content_root_ / given;
}

// A picked file is shown the way the path box shows one: relative to the content folder when it is inside it (that is
// what a scene file in the project should look like), and absolute otherwise (a scene opened from elsewhere).
std::string ScenePanel::DisplayPath(const std::filesystem::path& absolute) const {
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::absolute(content_root_, ec);
    std::filesystem::path given = absolute;
    if (!ec && given.is_relative()) { // the content folder can be relative too (a project opened by a relative path)
        const std::filesystem::path full = std::filesystem::absolute(given, ec);
        if (!ec) given = full;
    }
    if (!ec) {
        const std::string relative = given.lexically_relative(root).generic_string();
        if (!relative.empty() && relative.rfind("..", 0) != 0) return relative;
    }
    return given.is_absolute() ? given.generic_string() : absolute.generic_string();
}

FileDialogOptions ScenePanel::SceneFileDialogOptions(bool save) const {
    FileDialogOptions options;
    options.save = save;
    options.title = save ? "Save scene" : "Open scene";
    options.filters = {{"Aether scene", "ascene;aesc"}, {"All files", "*"}};
    options.default_extension = "ascene";
    options.start_path = document_.FilePath().empty() ? Resolve(path_) : document_.FilePath();
    options.default_name = options.start_path.filename().generic_string();
    return options;
}

std::optional<std::string> ScenePanel::PickSceneFile(bool save) {
    if (dialog_ == nullptr || !dialog_->Available()) return std::nullopt;
    const std::filesystem::path chosen = dialog_->Show(SceneFileDialogOptions(save));
    if (chosen.empty()) return std::string();
    return DisplayPath(chosen);
}

void ScenePanel::SetRecentScenesFile(const std::filesystem::path& file) {
    recent_scenes_file_ = file;
    recent_scenes_.Load(file);
}

void ScenePanel::RecordRecent(const std::string& path) {
    if (path.empty()) return;
    recent_scenes_.Add(path);
    if (!recent_scenes_file_.empty() && !recent_scenes_.Save(recent_scenes_file_))
        AETHER_LOG_WARN("Editor", "Couldn't save the recent-scenes list to %s", recent_scenes_file_.string().c_str());
}

std::size_t ScenePanel::RemoveMissingRecentScenes() {
    const std::size_t gone = recent_scenes_.RemoveMissing(content_root_);
    if (gone == 0) return 0;
    if (!recent_scenes_file_.empty()) recent_scenes_.Save(recent_scenes_file_);
    status_ = "Removed " + std::to_string(gone) +
              (gone == 1 ? " recent scene that no longer exists." : " recent scenes that no longer exist.");
    return gone;
}

bool ScenePanel::NewScene(bool discard) {
    if (document_.IsDirty() && !discard) {
        status_ = "There are unsaved changes: save them, or discard them to start a new scene.";
        return false;
    }
    document_.NewScene();
    bounds_.Clear();
    status_ = "A new scene.";
    if (on_document_replaced) on_document_replaced();
    return true;
}

bool ScenePanel::OpenScene(const std::string& path, bool discard) {
    if (document_.IsDirty() && !discard) {
        status_ = "There are unsaved changes: save them, or discard them to open another scene.";
        return false;
    }
    std::string error;
    const std::filesystem::path file = Resolve(path);
    if (!document_.Load(file, &error)) {
        status_ = error;
        return false;
    }
    path_ = path;
    bounds_.Clear(); // the models may have changed since they were last looked at
    status_ = "Opened " + file.string() + " (" + std::to_string(document_.Entities().size()) + " entities).";
    RecordRecent(DisplayPath(file));
    if (on_document_replaced) on_document_replaced();
    return true;
}

bool ScenePanel::LoadRecovered(const std::filesystem::path& autosave, const std::filesystem::path& source, bool discard) {
    if (document_.IsDirty() && !discard) {
        status_ = "There are unsaved changes: save them, or discard them to recover the last session's autosave.";
        return false;
    }
    std::string error;
    if (!document_.LoadAutosave(autosave, source, &error)) {
        status_ = error.empty() ? "The recovery autosave could not be opened." : error;
        return false;
    }
    bounds_.Clear();
    std::error_code ec;
    const std::string relative = source.empty() ? std::string() : std::filesystem::relative(source, content_root_, ec).generic_string();
    if (!relative.empty() && relative.rfind("..", 0) != 0) path_ = relative;
    else if (!source.empty()) path_ = source.string();
    status_ = "Recovered unsaved changes from the last session. Save to keep them.";
    if (!source.empty()) RecordRecent(DisplayPath(source));
    if (on_document_replaced) on_document_replaced();
    return true;
}

std::vector<std::string> ScenePanel::ListScenes() const {
    std::vector<std::string> scenes;
    std::error_code ec;
    if (!std::filesystem::is_directory(content_root_, ec)) return scenes;
    for (std::filesystem::recursive_directory_iterator it(content_root_, std::filesystem::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string extension = it->path().extension().string();
        if (extension != ".ascene" && extension != ".aesc") continue;
        scenes.push_back(std::filesystem::relative(it->path(), content_root_, ec).generic_string());
        if (scenes.size() >= 500) break; // a project with more has the path box
    }
    std::sort(scenes.begin(), scenes.end());
    return scenes;
}

bool ScenePanel::SaveScene(const std::string& path, bool overwrite) {
    std::string error;
    const bool to_new_path = !path.empty();
    const std::filesystem::path file = to_new_path ? Resolve(path) : (document_.FilePath().empty() ? Resolve(path_) : document_.FilePath());
    std::error_code ec;
    const bool is_open_file = !document_.FilePath().empty() &&
                              (std::filesystem::equivalent(file, document_.FilePath(), ec) || file.lexically_normal() == document_.FilePath().lexically_normal());
    if (!is_open_file && std::filesystem::exists(file, ec)) {
        if (!overwrite && !(to_new_path && pending_overwrite_ == file)) {
            pending_overwrite_ = file;
            status_ = "\"" + file.string() + "\" already exists and is not the scene that is open, so it was not replaced. Open it, save under another name, or choose Save As again to replace it.";
            return false;
        }
    }
    pending_overwrite_.clear();
    if (!document_.Save(file, &error)) {
        status_ = error.empty() ? "The scene could not be saved to " + file.string() + "." : error;
        return false;
    }
    if (to_new_path) path_ = path;
    status_ = "Saved " + file.string() + ".";
    RecordRecent(DisplayPath(file));
    return true;
}

bool ScenePanel::Reparent(Entity entity, Entity parent) {
    if (!document_.GetWorld().IsAlive(entity)) return false;
    if (document_.ParentOf(entity) == parent) return false;
    if (!document_.ReparentEntity(entity, parent)) {
        status_ = "An entity cannot go under itself or one of its own children.";
        return false;
    }
    return true;
}

usize ScenePanel::DeleteSelection() {
    const std::vector<Entity> selected = document_.Selection(); // a copy: deleting changes the selection
    usize deletable = 0;
    for (const Entity entity : selected) {
        if (document_.GetWorld().IsAlive(entity) && document_.GetWorld().HasComponent<IdComponent>(entity)) ++deletable;
    }
    const bool group = deletable > 1; // deleting several entities is one action: one undo brings them all back
    if (group) document_.Commands().BeginTransaction("Delete " + std::to_string(deletable) + " entities");
    usize deleted = 0;
    for (const Entity entity : selected)
        if (document_.GetWorld().IsAlive(entity) && document_.DestroyEntity(entity)) ++deleted;
    if (group) document_.Commands().EndTransaction();
    document_.ClearSelection();
    return deleted;
}

usize ScenePanel::DuplicateSelection() {
    const std::vector<Entity> selected = document_.Selection();
    usize duplicable = 0;
    for (const Entity entity : selected) duplicable += document_.GetWorld().IsAlive(entity) ? 1 : 0;
    const bool group = duplicable > 1; // several copies are one action, like several deletes
    if (group) document_.Commands().BeginTransaction("Duplicate " + std::to_string(duplicable) + " entities");
    std::vector<Entity> made;
    for (const Entity entity : selected)
        if (document_.GetWorld().IsAlive(entity)) {
            const Entity copy = document_.DuplicateEntity(entity);
            if (!copy.IsNull()) made.push_back(copy);
        }
    if (group) document_.Commands().EndTransaction();
    if (!made.empty()) document_.SetSelection(made);
    return made.size();
}

void ScenePanel::BeginRename(Entity entity) {
    if (!document_.GetWorld().IsAlive(entity)) return;
    rename_target_ = entity;
    Copy(rename_buffer_, document_.Name(entity));
    rename_focus_ = true;
}

void ScenePanel::Select(Entity entity, bool additive) {
    if (additive) document_.ToggleSelection(entity);
    else document_.SetSelection({entity});
}

void ScenePanel::Draw() {
    DrawToolbar();
    ImGui::Separator();
    const float total = ImGui::GetContentRegionAvail().x;
    const float left = std::clamp(total * 0.38f, 220.0f, 460.0f);
    if (ImGui::BeginChild("##scene_hierarchy", ImVec2(left, 0.0f), true)) DrawHierarchy();
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##scene_inspector", ImVec2(0.0f, 0.0f), true)) DrawInspector();
    ImGui::EndChild();
    DrawDiscardPrompt();
}

void ScenePanel::DrawToolbar() {
    if (ImGui::Button("New")) {
        if (document_.IsDirty()) { pending_ = Pending::New; ImGui::OpenPopup("Unsaved changes"); }
        else NewScene(true);
    }
    ImGui::SetItemTooltip("Start an empty scene");
    ImGui::SameLine();
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "%s", path_.c_str());
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputText("##scene_path", buffer, sizeof(buffer))) path_ = buffer;
    ImGui::SetItemTooltip("Scene file, relative to the content folder");
    ImGui::SameLine();
    if (ImGui::Button("Scenes")) ImGui::OpenPopup("##scene_list");
    ImGui::SetItemTooltip("Pick one of the scenes in the content folder");
    std::string chosen;
    if (ImGui::BeginPopup("##scene_list")) {
        const std::vector<std::string> scenes = ListScenes();
        if (scenes.empty()) ImGui::TextDisabled("No scenes in %s", content_root_.string().c_str());
        for (const std::string& scene : scenes) {
            if (ImGui::Selectable(scene.c_str(), scene == path_)) chosen = scene;
        }
        ImGui::EndPopup();
    }
    if (!chosen.empty()) { // after the list closes, so the unsaved-changes prompt is not a child of it
        path_ = chosen;
        if (document_.IsDirty()) { pending_ = Pending::Open; ImGui::OpenPopup("Unsaved changes"); }
        else OpenScene(chosen, true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Recent")) ImGui::OpenPopup("##recent_scenes");
    ImGui::SetItemTooltip("Scenes opened or saved recently");
    std::string recent_chosen;
    bool remove_missing_recent = false;
    if (ImGui::BeginPopup("##recent_scenes")) {
        const std::vector<std::string>& recent = recent_scenes_.Entries();
        if (recent.empty()) ImGui::TextDisabled("No recent scenes yet; open or save one");
        bool any_missing = false;
        for (const std::string& scene : recent) {
            std::error_code ec;
            const bool missing = !std::filesystem::exists(Resolve(scene), ec);
            any_missing = any_missing || missing;
            const std::string label = missing ? scene + "  (missing)" : scene;
            if (ImGui::Selectable(label.c_str(), scene == path_)) recent_chosen = scene;
        }
        if (any_missing) {
            ImGui::Separator();
            remove_missing_recent = ImGui::MenuItem("Remove missing scenes");
        }
        ImGui::EndPopup();
    }
    if (remove_missing_recent) RemoveMissingRecentScenes();
    if (!recent_chosen.empty()) {
        path_ = recent_chosen;
        if (document_.IsDirty()) { pending_ = Pending::Open; ImGui::OpenPopup("Unsaved changes"); }
        else OpenScene(recent_chosen, true);
    }
    ImGui::SameLine();
    if (ImGui::Button("...")) {
        const std::optional<std::string> picked = PickSceneFile(false);
        if (picked && !picked->empty()) {
            path_ = *picked;
            if (document_.IsDirty()) { pending_ = Pending::Open; ImGui::OpenPopup("Unsaved changes"); }
            else OpenScene(*picked, true);
        }
    }
    ImGui::SetItemTooltip("Choose a scene file with the system's file picker");
    ImGui::SameLine();
    if (ImGui::Button("Open")) {
        if (document_.IsDirty()) { pending_ = Pending::Open; ImGui::OpenPopup("Unsaved changes"); }
        else OpenScene(path_, true);
    }
    ImGui::SetItemTooltip("Open the scene named in the box");
    ImGui::SameLine();
    if (ImGui::Button("Save")) SaveScene();
    ImGui::SetItemTooltip("Save (Ctrl+S)");
    ImGui::SameLine();
    if (ImGui::Button("Save As")) SaveScene(path_);
    ImGui::SetItemTooltip("Save to the name in the box (File > Save Scene As opens a file picker)");
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::BeginDisabled(!document_.CanUndo());
    if (ImGui::Button("Undo")) document_.Undo();
    ImGui::EndDisabled();
    if (document_.CanUndo()) ImGui::SetItemTooltip("Undo %s (Ctrl+Z)", document_.UndoLabel().c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(!document_.CanRedo());
    if (ImGui::Button("Redo")) document_.Redo();
    ImGui::EndDisabled();
    if (document_.CanRedo()) ImGui::SetItemTooltip("Redo %s (Ctrl+Y)", document_.RedoLabel().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    const PlaySession::State state = document_.PlayState();
    if (state == PlaySession::State::Editing) {
        if (ImGui::Button("Play")) document_.Play();
    } else {
        if (ImGui::Button(state == PlaySession::State::Playing ? "Pause" : "Resume")) {
            if (state == PlaySession::State::Playing) document_.Pause();
            else document_.Play();
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop")) document_.Stop();
    }
    const std::string title = document_.FilePath().empty() ? std::string("(unsaved scene)") : document_.FilePath().filename().string();
    ImGui::TextDisabled("%s%s%s", title.c_str(), document_.IsDirty() ? " *" : "", state == PlaySession::State::Editing ? "" : "   [playing: edits are discarded on Stop]");
    if (document_.IsDirty()) ImGui::SetItemTooltip("Unsaved changes");
    if (!status_.empty()) ImGui::TextWrapped("%s", status_.c_str());
    // Undo, redo and save from the keyboard, when no text box has it. The chords come from the command table, so
    // the menu, the palette and this check cannot drift apart.
    const ImGuiIO& io = ImGui::GetIO();
    if (commands_ != nullptr && !io.WantTextInput && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
        commands_->RunPressedChord(CommandScope::Scene);
}

void ScenePanel::DrawDiscardPrompt() {
    if (!ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted("The scene has changes that are not saved.");
    if (ImGui::Button("Save first")) {
        if (SaveScene()) {
            if (pending_ == Pending::New) NewScene(true);
            else if (pending_ == Pending::Open) OpenScene(path_, true);
            pending_ = Pending::None;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard them")) {
        if (pending_ == Pending::New) NewScene(true);
        else if (pending_ == Pending::Open) OpenScene(path_, true);
        pending_ = Pending::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        pending_ = Pending::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void ScenePanel::DrawCreateMenu(Entity parent) {
    const char* label = parent.IsNull() ? "Create" : "Create child";
    if (!ImGui::BeginMenu(label)) return;
    struct Kind { const char* name; SceneEntityKind kind; };
    for (const Kind& k : {Kind{"Empty", SceneEntityKind::Empty}, Kind{"Camera", SceneEntityKind::Camera}, Kind{"Cine Camera", SceneEntityKind::CineCamera}}) {
        if (ImGui::MenuItem(k.name)) {
            const Entity made = document_.CreateEntity(k.name, k.kind, {}, parent);
            if (!made.IsNull()) document_.SetSelection({made});
        }
    }
    ImGui::EndMenu();
}

void ScenePanel::DrawEntityMenu(Entity entity) {
    if (!document_.IsSelected(entity)) Select(entity, false); // a right click acts on what it landed on
    DrawCreateMenu(entity);
    if (ImGui::MenuItem("Rename", "F2")) BeginRename(entity);
    if (ImGui::MenuItem("Duplicate", "Ctrl+D")) DuplicateSelection();
    if (ImGui::MenuItem("Copy", "Ctrl+C")) document_.CopySelection();
    if (ImGui::MenuItem("Paste", "Ctrl+V", false, document_.HasClipboard())) document_.PasteClipboard();
    ImGui::BeginDisabled(document_.ParentOf(entity).IsNull());
    if (ImGui::MenuItem("Move to the scene root")) Reparent(entity, kNullEntity);
    ImGui::EndDisabled();
    ImGui::Separator();
    if (ImGui::MenuItem("Delete", "Del")) DeleteSelection();
}

void ScenePanel::DrawNode(Entity entity, const HierarchyIndex& index) {
    if (!document_.GetWorld().IsAlive(entity)) return;
    const bool has_children = index.HasChildren(entity);
    ImGui::PushID(static_cast<int>(entity.index));
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
    if (!has_children) flags |= ImGuiTreeNodeFlags_Leaf;
    if (document_.IsSelected(entity)) flags |= ImGuiTreeNodeFlags_Selected;
    bool open = false;
    if (rename_target_ == entity) {
        // The name as a text box, until Enter or a click elsewhere; typing is one undo step.
        if (rename_focus_) { ImGui::SetKeyboardFocusHere(); rename_focus_ = false; }
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText("##rename", rename_buffer_.data(), rename_buffer_.size(), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            document_.Rename(entity, rename_buffer_.data(), true);
            rename_target_ = kNullEntity;
        } else if (ImGui::IsItemDeactivated()) {
            document_.Rename(entity, rename_buffer_.data(), true);
            rename_target_ = kNullEntity;
        }
    } else {
        open = ImGui::TreeNodeEx("##node", flags, "%s", document_.Name(entity).c_str());
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) Select(entity, ImGui::GetIO().KeyCtrl);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) BeginRename(entity);
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            ImGui::SetDragDropPayload(kEntityPayload, &entity, sizeof(Entity));
            ImGui::TextUnformatted(document_.Name(entity).c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload); payload != nullptr && payload->DataSize == sizeof(Entity)) {
                Entity dragged;
                std::memcpy(&dragged, payload->Data, sizeof(Entity));
                Reparent(dragged, entity);
            }
            // A model or a prefab from the Content Browser goes in under this entity, at its origin, as one undo step.
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kContentPathPayload); payload != nullptr && payload->Data != nullptr) {
                status_ = PlaceContentAsset(document_, content_root_, static_cast<const char*>(payload->Data), Vec3(0.0f, 0.0f, 0.0f), nullptr, entity);
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::BeginPopupContextItem("##entity_menu")) {
            DrawEntityMenu(entity);
            ImGui::EndPopup();
        }
    }
    if (open) {
        for (const Entity child : index.Children(entity)) DrawNode(child, index);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void ScenePanel::DrawHierarchy() {
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##scene_filter", "Filter by name", filter_.data(), filter_.size());
    const std::string filter = filter_.data();
    // Dropping on the heading makes an entity a root again.
    // An item with an ID of its own and the full width: a drop is delivered only when the same target took it the frame before, and plain text has no ID (its
    // target flickered between two, so a drop on the heading was never delivered), and a heading as wide as the list is also easier to hit.
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    const std::string heading = std::to_string(document_.Entities().size()) + " entities";
    ImGui::Selectable((heading + "##hierarchy_heading").c_str(), false, ImGuiSelectableFlags_DontClosePopups);
    ImGui::PopStyleColor();
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload); payload != nullptr && payload->DataSize == sizeof(Entity)) {
            Entity dragged;
            std::memcpy(&dragged, payload->Data, sizeof(Entity));
            Reparent(dragged, kNullEntity);
        }
        // An asset dropped on the heading is placed at the root, at the scene's origin.
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kContentPathPayload); payload != nullptr && payload->Data != nullptr) {
            status_ = PlaceContentAsset(document_, content_root_, static_cast<const char*>(payload->Data), Vec3(0.0f, 0.0f, 0.0f));
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::Separator();
    if (filter.empty()) {
        const HierarchyIndex index(document_);
        for (const Entity root : index.Children(kNullEntity)) DrawNode(root, index);
    } else {
        for (const Entity entity : MatchingEntities(document_, filter)) {
            ImGui::PushID(static_cast<int>(entity.index));
            if (ImGui::Selectable(document_.Name(entity).c_str(), document_.IsSelected(entity))) Select(entity, ImGui::GetIO().KeyCtrl);
            if (ImGui::BeginPopupContextItem("##entity_menu")) {
                DrawEntityMenu(entity);
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }
    // A right click on the empty part makes something at the root.
    if (ImGui::BeginPopupContextWindow("##hierarchy_menu", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        DrawCreateMenu(kNullEntity);
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, document_.HasClipboard())) document_.PasteClipboard();
        ImGui::EndPopup();
    }
    // Keyboard, when this list has the focus and no text box has the keys. Same command table as the Edit menu.
    const ImGuiIO& io = ImGui::GetIO();
    if (commands_ != nullptr && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !io.WantTextInput && !Renaming())
        commands_->RunPressedChord(CommandScope::SceneTree);
}

void ScenePanel::DrawInspector() {
    Entity shown = kNullEntity;
    for (const Entity entity : document_.Selection())
        if (document_.GetWorld().IsAlive(entity)) { shown = entity; break; }
    if (shown.IsNull()) {
        ImGui::TextDisabled("Select an entity in the hierarchy to edit it.");
        return;
    }
    const usize selected = document_.Selection().size();
    if (selected > 1) ImGui::TextDisabled("%zu entities selected; showing the first.", selected);
    // The name, with typing merged into one undo step.
    char name[128];
    std::snprintf(name, sizeof(name), "%s", document_.Name(shown).c_str());
    if (ImGui::InputText("Name", name, sizeof(name))) document_.Rename(shown, name, false);
    if (ImGui::IsItemDeactivatedAfterEdit()) document_.Rename(shown, name, true);
    ImGui::Separator();
    InspectEntity(document_.Context(), document_.Commands(), shown);
}

} // namespace aether::editor
