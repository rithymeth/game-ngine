#include "world/scene_panel.h"

#include "content/content_drag.h"
#include "core/file_dialog.h"
#include "workspace/command_registry.h"
#include "ui/entity_inspector.h"
#include "world/content_drop.h"
#include "world/scene_view_panel.h"

#include "aether/core/log.h"
#include "aether/scene/gameplay.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace aether::editor {

namespace {

constexpr const char* kEntityPayload = "AETHER_ENTITY"; // one or more Entity values, back to back

std::vector<Entity> PayloadEntities(const ImGuiPayload& payload) {
    if (payload.Data == nullptr || payload.DataSize <= 0 || payload.DataSize % static_cast<int>(sizeof(Entity)) != 0) return {};
    std::vector<Entity> entities(static_cast<usize>(payload.DataSize) / sizeof(Entity));
    std::memcpy(entities.data(), payload.Data, static_cast<usize>(payload.DataSize));
    return entities;
}

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

void Copy(std::array<char, 128>& buffer, const std::string& text) {
    std::snprintf(buffer.data(), buffer.size(), "%s", text.c_str());
}

constexpr ImVec4 kPrefabColor{0.35f, 0.65f, 1.0f, 1.0f};        // the Inspector's override blue
constexpr ImVec4 kMissingPrefabColor{0.95f, 0.4f, 0.35f, 1.0f};

// The badge and the name of a tree row whose own label is empty, drawn over it so the row keeps one item (selection,
// drag and the context menu all go on it) and its height. Returns the badge's top-left corner; it is a font size square.
ImVec2 DrawRowLabel(ImVec2 row_min, ImVec2 row_max, EntityKind kind, const PrefabRowState& prefab, const std::string& name, bool hidden) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const f32 size = ImGui::GetFontSize();
    const ImVec2 badge(row_min.x + ImGui::GetTreeNodeToLabelSpacing(), row_min.y + (row_max.y - row_min.y - size) * 0.5f);
    const ImVec2 badge_max(badge.x + size, badge.y + size);
    draw->AddRectFilled(badge, badge_max, ImGui::GetColorU32(EntityKindColor(kind)), size * 0.25f);
    const char glyph[2] = {EntityKindGlyph(kind), '\0'};
    const ImVec2 glyph_size = ImGui::CalcTextSize(glyph);
    draw->AddText(ImVec2(badge.x + (size - glyph_size.x) * 0.5f, badge.y + (size - glyph_size.y) * 0.5f), IM_COL32(20, 20, 24, 255),
                  glyph);
    ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    if (hidden) text = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    else if (prefab.role != PrefabRowState::Role::None) text = ImGui::GetColorU32(prefab.missing_source ? kMissingPrefabColor : kPrefabColor);
    draw->AddText(ImVec2(badge_max.x + ImGui::GetStyle().ItemInnerSpacing.x, badge.y), text, name.c_str());
    if (prefab.overrides > 0) // the Inspector marks an overridden field the same way
        draw->AddRectFilled(ImVec2(row_min.x - 4.0f, row_min.y), ImVec2(row_min.x - 1.0f, row_max.y), ImGui::GetColorU32(kPrefabColor));
    return badge;
}

enum class RowGlyph { Eye, Lock };

// The eye and the lock at the end of a row, drawn onto an invisible button. `on` is "shown" for the eye and "locked"
// for the lock; an off eye carries a slash, an unlocked lock an open shackle. Returns a click on it.
bool RowToggle(const ImVec2& at, f32 size, RowGlyph glyph, bool on, const char* id, const char* tooltip) {
    ImGui::SetCursorScreenPos(at);
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 color = ImGui::GetColorU32(on ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    constexpr f32 kPi = 3.14159265f;
    const f32 x = at.x, y = at.y;
    if (glyph == RowGlyph::Eye) {
        // Two arcs for the lids and a pupil: an eye. Closed, the same outline with a slash through it.
        draw->AddBezierQuadratic(ImVec2(x + 1.0f, y + size * 0.5f), ImVec2(x + size * 0.5f, y - size * 0.08f), ImVec2(x + size - 1.0f, y + size * 0.5f),
                                 color, 1.4f, 12);
        draw->AddBezierQuadratic(ImVec2(x + 1.0f, y + size * 0.5f), ImVec2(x + size * 0.5f, y + size * 1.08f), ImVec2(x + size - 1.0f, y + size * 0.5f),
                                 color, 1.4f, 12);
        if (on) draw->AddCircleFilled(ImVec2(x + size * 0.5f, y + size * 0.5f), size * 0.13f, color);
        else draw->AddLine(ImVec2(x + 2.0f, y + size - 2.0f), ImVec2(x + size - 2.0f, y + 2.0f), color, 1.4f);
    } else {
        const f32 body_top = y + size * 0.46f;
        draw->AddRectFilled(ImVec2(x + size * 0.2f, body_top), ImVec2(x + size * 0.8f, y + size * 0.92f), color, size * 0.08f);
        if (on) draw->PathArcTo(ImVec2(x + size * 0.5f, body_top), size * 0.2f, kPi, 2.0f * kPi, 12); // a closed shackle
        else draw->PathArcTo(ImVec2(x + size * 0.56f, y + size * 0.4f), size * 0.2f, kPi, 1.7f * kPi, 12); // swung up and open
        draw->PathStroke(color, 0, 1.4f);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    return pressed;
}

// A small drawn dropdown chip for the Inspector's header ("Gameplay v"), like the toolbar's look chips: an invisible
// button, its label, a caret and a hover fill. `muted` reads as a placeholder. Returns a click on it.
bool DrawHeaderChip(const char* id, const std::string& label, bool muted, ImVec2& min, ImVec2& max) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImGui::InvisibleButton(id, ImVec2(ImGui::CalcTextSize(label.c_str()).x + 30.0f, ImGui::GetFrameHeight()));
    min = ImGui::GetItemRectMin();
    max = ImGui::GetItemRectMax();
    if (ImGui::IsItemHovered()) draw->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_HeaderHovered), 4.0f);
    const ImVec2 text = ImGui::CalcTextSize(label.c_str());
    const ImU32 color = ImGui::GetColorU32(muted ? ImGuiCol_TextDisabled : ImGuiCol_Text);
    draw->AddText(ImVec2(min.x + 8.0f, (min.y + max.y - text.y) * 0.5f), color, label.c_str());
    const f32 caret_x = max.x - 12.0f;
    const f32 caret_y = (min.y + max.y) * 0.5f;
    draw->AddTriangleFilled(ImVec2(caret_x - 4.0f, caret_y - 2.0f), ImVec2(caret_x + 4.0f, caret_y - 2.0f), ImVec2(caret_x, caret_y + 3.0f),
                            ImGui::GetColorU32(ImGuiCol_TextDisabled));
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return ImGui::IsItemClicked();
}

// Every distinct tag the scene's entities carry, in first-seen order: what the header's tag dropdown offers.
std::vector<std::string> SceneTags(const SceneDocument& document) {
    std::vector<std::string> tags;
    const World& world = document.GetWorld();
    for (const Entity entity : document.Entities()) {
        const Tags* component = world.GetComponent<Tags>(entity);
        if (component == nullptr) continue;
        for (const std::string& name : component->names) {
            if (std::find(tags.begin(), tags.end(), name) == tags.end()) tags.push_back(name);
        }
    }
    return tags;
}

} // namespace

// One colour per kind, shared by the hierarchy badges and the viewport's icons and legend, so an entity reads as the
// same thing in both panels.
ImVec4 EntityKindColor(EntityKind kind) {
    switch (kind) {
        case EntityKind::Camera: return {0.38f, 0.62f, 0.92f, 1.0f};
        case EntityKind::Light: return {0.95f, 0.8f, 0.3f, 1.0f};
        case EntityKind::Model: return {0.32f, 0.76f, 0.7f, 1.0f};
        case EntityKind::Particles: return {0.95f, 0.56f, 0.3f, 1.0f};
        case EntityKind::Audio: return {0.7f, 0.52f, 0.92f, 1.0f};
        case EntityKind::Ui: return {0.9f, 0.46f, 0.66f, 1.0f};
        case EntityKind::Physics: return {0.48f, 0.8f, 0.4f, 1.0f};
        case EntityKind::Script: return {0.78f, 0.72f, 0.46f, 1.0f};
        case EntityKind::Volume: return {0.54f, 0.56f, 0.95f, 1.0f};
        case EntityKind::Other: return {0.62f, 0.62f, 0.62f, 1.0f};
        case EntityKind::Empty: return {0.42f, 0.42f, 0.42f, 1.0f};
    }
    return {0.62f, 0.62f, 0.62f, 1.0f};
}

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
    const HierarchyQuery query = ParseHierarchyQuery(filter);
    std::vector<Entity> out;
    for (const Entity entity : document.Entities())
        if (query.Matches(document, entity)) out.push_back(entity);
    std::stable_sort(out.begin(), out.end(), [&](Entity a, Entity b) {
        const std::string left = Lower(document.Name(a)), right = Lower(document.Name(b));
        return left != right ? left < right : a.index < b.index;
    });
    return out;
}

HierarchyQuery ParseHierarchyQuery(std::string_view text) {
    constexpr std::string_view kType = "type:";
    constexpr std::string_view kName = "name:";
    HierarchyQuery query;
    usize pos = 0;
    while (pos < text.size()) {
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
        const usize start = pos;
        while (pos < text.size() && !std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
        if (start == pos) break;
        const std::string_view term = text.substr(start, pos - start);
        // Directives are recognized in any case; their value is lowercased like a bare term.
        const std::string lowered = Lower(std::string(term));
        if (lowered.compare(0, kType.size(), kType) == 0) {
            const std::string value = lowered.substr(kType.size());
            if (!value.empty()) query.type_terms.push_back(value); // an empty directive is no term
        } else if (lowered.compare(0, kName.size(), kName) == 0) {
            const std::string value = lowered.substr(kName.size());
            if (!value.empty()) query.name_terms.push_back(value);
        } else {
            query.name_terms.push_back(lowered);
        }
    }
    return query;
}

bool HierarchyQuery::Matches(const SceneDocument& document, Entity entity) const {
    const std::string name = Lower(document.Name(entity));
    for (const std::string& term : name_terms) {
        if (name.find(term) == std::string::npos) return false;
    }
    if (type_terms.empty()) return true;
    const World& world = document.GetWorld();
    for (const std::string& term : type_terms) {
        bool found = false;
        for (ComponentId id = 0; id < RegisteredComponentCount() && !found; ++id) {
            if (!world.HasComponentRaw(entity, id)) continue;
            found = Lower(GetComponentInfo(id).name).find(term) != std::string::npos;
        }
        if (!found) return false;
    }
    return true;
}

std::unordered_set<Entity> VisibleEntities(const SceneDocument& document, const std::vector<Entity>& matches) {
    std::unordered_set<Entity> shown;
    for (const Entity match : matches) {
        Entity current = match;
        for (int depth = 0; depth < kMaxHierarchyDepth && !current.IsNull(); ++depth) {
            if (!shown.insert(current).second) break; // the rest of this chain is already in
            current = GetParent(document.GetWorld(), document.Guids(), current);
        }
    }
    return shown;
}

EntityKind ComponentKind(std::string_view component_name) {
    const usize colons = component_name.rfind("::");
    if (colons != std::string_view::npos) component_name.remove_prefix(colons + 2);
    else if (const usize space = component_name.rfind(' '); space != std::string_view::npos) component_name.remove_prefix(space + 1);
    const std::string name = Lower(std::string(component_name));
    const auto has = [&](std::string_view part) { return name.find(part) != std::string::npos; };
    const auto starts = [&](std::string_view part) { return name.compare(0, part.size(), part) == 0; };
    const auto ends = [&](std::string_view part) {
        return name.size() >= part.size() && name.compare(name.size() - part.size(), part.size(), part) == 0;
    };
    static constexpr std::string_view kBookkeeping[] = {"entityname", "parent",    "idcomponent", "active",        "tags",
                                                        "layer",      "datalayer", "prefablink",  "prefabinstance"};
    for (const std::string_view core : kBookkeeping)
        if (name == core) return EntityKind::Empty;
    if (ends("transform")) return EntityKind::Empty; // Transform, PreviousTransform
    if (ends("camera")) return EntityKind::Camera;
    if (has("light")) return EntityKind::Light;
    if (has("collider") || has("rigidbody") || has("charactermovement") || has("physics")) return EntityKind::Physics;
    if (name == "modelrenderer" || starts("sprite") || has("mesh") || has("terrain") || has("decal") || has("tilemap"))
        return EntityKind::Model;
    if (has("particle")) return EntityKind::Particles;
    if (starts("audio")) return EntityKind::Audio;
    if (has("widget")) return EntityKind::Ui;
    if (has("script") || has("blueprint")) return EntityKind::Script;
    if (has("volume") || ends("zone") || has("trigger")) return EntityKind::Volume;
    return EntityKind::Other;
}

EntityKind ClassifyEntity(const World& world, Entity entity) {
    EntityKind best = EntityKind::Empty;
    if (!world.IsAlive(entity)) return best;
    for (ComponentId id = 0; id < RegisteredComponentCount(); ++id) {
        if (!world.HasComponentRaw(entity, id)) continue;
        best = std::min(best, ComponentKind(GetComponentInfo(id).name));
    }
    return best;
}

const char* EntityKindName(EntityKind kind) {
    switch (kind) {
        case EntityKind::Camera: return "Camera";
        case EntityKind::Light: return "Light";
        case EntityKind::Model: return "Model";
        case EntityKind::Particles: return "Particles";
        case EntityKind::Audio: return "Audio";
        case EntityKind::Ui: return "UI";
        case EntityKind::Physics: return "Physics";
        case EntityKind::Script: return "Script";
        case EntityKind::Volume: return "Volume";
        case EntityKind::Other: return "Entity";
        case EntityKind::Empty: return "Empty";
    }
    return "Entity";
}

char EntityKindGlyph(EntityKind kind) {
    switch (kind) {
        case EntityKind::Camera: return 'C';
        case EntityKind::Light: return 'L';
        case EntityKind::Model: return 'M';
        case EntityKind::Particles: return 'F';
        case EntityKind::Audio: return 'A';
        case EntityKind::Ui: return 'U';
        case EntityKind::Physics: return 'P';
        case EntityKind::Script: return 'S';
        case EntityKind::Volume: return 'V';
        case EntityKind::Other: return 'E';
        case EntityKind::Empty: return 'G';
    }
    return 'E';
}

PrefabRowState PrefabRowStateOf(const World& world, const GuidIndex& guids, const PrefabLookup& find_prefab, Entity entity) {
    PrefabRowState state;
    const Entity root = FindInstanceRoot(world, guids, entity);
    if (root.IsNull()) return state;
    state.role = root == entity ? PrefabRowState::Role::Root : PrefabRowState::Role::Part;
    const PrefabInstance* instance = world.GetComponent<PrefabInstance>(root);
    const PrefabLink* link = world.HasComponent<PrefabLink>(entity) ? world.GetComponent<PrefabLink>(entity) : nullptr;
    if (link != nullptr) {
        state.overrides = static_cast<usize>(std::count_if(instance->overrides.begin(), instance->overrides.end(),
                                                           [&](const PropertyOverride& o) { return o.entity == link->local_id; }));
    }
    if (find_prefab) state.missing_source = find_prefab(instance->source.guid) == nullptr;
    return state;
}

std::string HierarchyRowTooltip(EntityKind kind, const PrefabRowState& prefab) {
    std::string text = EntityKindName(kind);
    if (prefab.role == PrefabRowState::Role::None) return text;
    text += prefab.role == PrefabRowState::Role::Root ? " - prefab instance" : " - part of a prefab instance";
    if (prefab.missing_source) text += " whose prefab is missing";
    if (prefab.overrides == 1) text += " (1 override)";
    else if (prefab.overrides > 1) text += " (" + std::to_string(prefab.overrides) + " overrides)";
    return text;
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
    return ReparentEntities(std::span<const Entity>(&entity, 1), parent) == 1;
}

usize ScenePanel::ReparentEntities(std::span<const Entity> entities, Entity parent) {
    const World& world = document_.GetWorld();
    const auto carried = [&](Entity entity) { return std::find(entities.begin(), entities.end(), entity) != entities.end(); };
    std::vector<Entity> moving;
    for (const Entity entity : entities) {
        if (!world.IsAlive(entity) || std::find(moving.begin(), moving.end(), entity) != moving.end()) continue;
        if (document_.ParentOf(entity) == parent) continue;
        bool under_carried = false; // it travels inside a carried ancestor's subtree and keeps its own parent
        usize depth = 0;
        for (Entity up = document_.ParentOf(entity); !up.IsNull() && depth < world.EntityCount(); up = document_.ParentOf(up), ++depth) {
            if (carried(up)) { under_carried = true; break; }
        }
        if (!under_carried) moving.push_back(entity);
    }
    const bool group = moving.size() > 1;
    if (group) {
        const std::string count = std::to_string(moving.size()) + " entities";
        document_.Commands().BeginTransaction(parent.IsNull() ? "Move " + count + " to the root" : "Parent " + count);
    }
    usize moved = 0;
    for (const Entity entity : moving) moved += document_.ReparentEntity(entity, parent) ? 1 : 0;
    if (group) document_.Commands().EndTransaction();
    if (moved < moving.size()) {
        status_ = moved == 0 ? "An entity cannot go under itself or one of its own children."
                             : std::to_string(moving.size() - moved) + " of the dragged entities could not go under one of their own children; the rest moved.";
    }
    return moved;
}

usize ScenePanel::SetHidden(std::span<const Entity> entities, bool hidden) {
    std::vector<Entity> changing;
    for (const Entity entity : entities) {
        if (!document_.GetWorld().IsAlive(entity) || document_.IsActive(entity) == !hidden) continue;
        if (std::find(changing.begin(), changing.end(), entity) != changing.end()) continue;
        changing.push_back(entity);
    }
    if (changing.empty()) return 0;
    if (changing.size() > 1) {
        document_.Commands().BeginTransaction((hidden ? "Hide " : "Show ") + std::to_string(changing.size()) + " entities");
    }
    for (const Entity entity : changing) document_.SetActive(entity, !hidden);
    if (changing.size() > 1) document_.Commands().EndTransaction();
    return changing.size();
}

usize ScenePanel::SetLocked(std::span<const Entity> entities, bool locked) {
    usize changed = 0;
    for (const Entity entity : entities) {
        if (!document_.GetWorld().IsAlive(entity) || document_.IsLocked(entity) == locked) continue;
        document_.SetLocked(entity, locked);
        ++changed;
    }
    return changed;
}

std::vector<Entity> ScenePanel::DragSet(Entity entity) const {
    if (!document_.IsSelected(entity)) return {entity};
    std::vector<Entity> set;
    for (const Entity selected : document_.Selection()) {
        if (document_.GetWorld().IsAlive(selected)) set.push_back(selected);
    }
    return set;
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
    layout_ = Layout{};
    const float total = ImGui::GetContentRegionAvail().x;
    auto record_pane = [](ImVec2& min, ImVec2& max) { // a child window's rectangle, for the layout a test or a host reads
        min = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        max = ImVec2(min.x + size.x, min.y + size.y);
    };
    if (view_ != nullptr) {
        // The concept's level editor: the hierarchy, the viewport and the Inspector side by side, the viewport taking the room
        // the other two leave. The view draws without its own toolbar row, which the centre column is too narrow for; the
        // picture's chips and W, E and R carry those controls there.
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float left = std::clamp(total * 0.20f, 190.0f, 320.0f);
        const float right = std::clamp(total * 0.26f, 250.0f, 440.0f);
        const float middle = std::max(total - left - right - spacing * 2.0f, 160.0f);
        if (ImGui::BeginChild("##scene_hierarchy", ImVec2(left, 0.0f), true)) {
            record_pane(layout_.hierarchy_min, layout_.hierarchy_max);
            DrawHierarchy();
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("##scene_viewport", ImVec2(middle, 0.0f), true)) {
            record_pane(layout_.viewport_min, layout_.viewport_max);
            view_->Draw(document_, &bounds_, content_root_, false);
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("##scene_inspector", ImVec2(right, 0.0f), true)) {
            record_pane(layout_.inspector_min, layout_.inspector_max);
            DrawInspector();
        }
        ImGui::EndChild();
        DrawDiscardPrompt();
        return;
    }
    const float left = std::clamp(total * 0.38f, 220.0f, 460.0f);
    if (ImGui::BeginChild("##scene_hierarchy", ImVec2(left, 0.0f), true)) {
        record_pane(layout_.hierarchy_min, layout_.hierarchy_max);
        DrawHierarchy();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##scene_inspector", ImVec2(0.0f, 0.0f), true)) {
        record_pane(layout_.inspector_min, layout_.inspector_max);
        DrawInspector();
    }
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
    DrawCreateItems(parent);
    ImGui::EndMenu();
}

// The kinds the Create menu, the header's + and the Actor menu all add. One list, so a kind offered in one place is
// offered in the others.
void ScenePanel::DrawCreateItems(Entity parent) {
    struct Kind { const char* name; SceneEntityKind kind; };
    for (const Kind& k : {Kind{"Empty", SceneEntityKind::Empty}, Kind{"Camera", SceneEntityKind::Camera}, Kind{"Cine Camera", SceneEntityKind::CineCamera}}) {
        if (ImGui::MenuItem(k.name)) {
            const Entity made = document_.CreateEntity(k.name, k.kind, {}, parent);
            if (!made.IsNull()) document_.SetSelection({made});
        }
    }
}

// The concept's hierarchy header: "Hierarchy | Place" at the left, + and ... at the right. The tabs are drawn rather
// than ImGui's, which would take the whole column and cannot report separate rectangles for a test's click.
void ScenePanel::DrawHierarchyHeader() {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    hierarchy_header_ = HierarchyHeader{};
    const auto tab = [&](const char* label, HierarchyTab page, ImVec2& min, ImVec2& max) {
        const ImVec2 text = ImGui::CalcTextSize(label);
        ImGui::InvisibleButton(label, ImVec2(text.x + 16.0f, ImGui::GetFrameHeight()));
        min = ImGui::GetItemRectMin();
        max = ImGui::GetItemRectMax();
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const bool active = hierarchy_tab_ == page;
        if (ImGui::IsItemClicked()) hierarchy_tab_ = page;
        draw->AddText(ImVec2(std::floor((min.x + max.x - text.x) * 0.5f), std::floor((min.y + max.y - text.y) * 0.5f)),
                      ImGui::GetColorU32(active ? ImGuiCol_Text : ImGuiCol_TextDisabled), label);
        if (active) {
            draw->AddRectFilled(ImVec2(min.x + 4.0f, max.y - 2.0f), ImVec2(max.x - 4.0f, max.y),
                                ImGui::GetColorU32(ImGuiCol_HeaderActive), 1.0f);
        }
    };
    tab("Hierarchy", HierarchyTab::Hierarchy, hierarchy_header_.hierarchy_min, hierarchy_header_.hierarchy_max);
    ImGui::SameLine();
    tab("Place", HierarchyTab::Place, hierarchy_header_.place_min, hierarchy_header_.place_max);

    // The + and ... glyphs are drawn (the + as two lines, the ... as three dots) so they need no glyph the default font
    // may lack, and they sit at the header's right end as the concept draws them.
    const f32 size = 18.0f;
    const auto glyph_button = [&](const char* id, ImVec2& min, ImVec2& max, const std::function<void(const ImVec2&, const ImVec2&)>& glyph) {
        ImGui::InvisibleButton(id, ImVec2(size, size));
        min = ImGui::GetItemRectMin();
        max = ImGui::GetItemRectMax();
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            draw->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_HeaderHovered), 3.0f);
        }
        glyph(min, max);
        return ImGui::IsItemClicked();
    };
    const ImU32 glyph_color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const f32 spacing = ImGui::GetStyle().ItemSpacing.x;
    const f32 right = ImGui::GetWindowContentRegionMax().x - size * 2.0f - spacing;
    ImGui::SameLine(std::max(right, ImGui::GetCursorPosX() + 8.0f));
    const bool add_clicked = glyph_button("##hierarchy_add", hierarchy_header_.add_min, hierarchy_header_.add_max,
                                          [&](const ImVec2& min, const ImVec2& max) {
                                              const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
                                              draw->AddLine(ImVec2(center.x - 4.0f, center.y), ImVec2(center.x + 4.0f, center.y), glyph_color);
                                              draw->AddLine(ImVec2(center.x, center.y - 4.0f), ImVec2(center.x, center.y + 4.0f), glyph_color);
                                          });
    ImGui::SetItemTooltip("Add an actor to the scene");
    ImGui::SameLine();
    const bool more_clicked = glyph_button("##hierarchy_more", hierarchy_header_.more_min, hierarchy_header_.more_max,
                                           [&](const ImVec2& min, const ImVec2& max) {
                                               const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
                                               for (int dot = -1; dot <= 1; ++dot) {
                                                   draw->AddCircleFilled(ImVec2(center.x + static_cast<f32>(dot) * 4.0f, center.y), 1.4f, glyph_color);
                                               }
                                           });
    ImGui::SetItemTooltip("Paste, or clear the selection");
    if (add_clicked) ImGui::OpenPopup("##hierarchy_add");
    if (more_clicked) ImGui::OpenPopup("##hierarchy_more");
    if (ImGui::BeginPopup("##hierarchy_add")) {
        DrawCreateItems(kNullEntity);
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##hierarchy_more")) {
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, document_.HasClipboard())) document_.PasteClipboard();
        if (ImGui::MenuItem("Deselect All", nullptr, false, !document_.Selection().empty())) document_.ClearSelection();
        ImGui::EndPopup();
    }
    hierarchy_header_.active = hierarchy_tab_;
}

// The Place page: the actor kinds the scene can add, each row one click away. Clicking adds at the root and selects the
// new actor, the same call the Create menu makes, so the two cannot disagree about what a kind contains.
void ScenePanel::DrawPlacePanel() {
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##place_filter", "Search actors", &place_filter_);
    struct Entry { const char* label; const char* description; SceneEntityKind kind; };
    static constexpr Entry kEntries[] = {
        {"Empty Actor", "A transform only: a parent for other entities", SceneEntityKind::Empty},
        {"Camera", "A perspective camera", SceneEntityKind::Camera},
        {"Cine Camera", "A camera with focus and film settings", SceneEntityKind::CineCamera},
    };
    const std::string query = Lower(place_filter_);
    const f32 line = ImGui::GetTextLineHeight();
    const f32 width = ImGui::GetContentRegionAvail().x;
    usize shown = 0;
    for (const Entry& entry : kEntries) {
        if (!query.empty() && Lower(entry.label).find(query) == std::string::npos) continue;
        ++shown;
        const f32 height = line * 2.0f + 6.0f;
        const ImVec2 start = ImGui::GetCursorScreenPos();
        ImGui::PushID(entry.label);
        ImGui::InvisibleButton("##place_row", ImVec2(width, height));
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::GetWindowDrawList()->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_HeaderHovered), 3.0f);
        }
        const auto fit = [&](const std::string& text) { // the column is narrow: cut with an ellipsis rather than spilling out
            if (ImGui::CalcTextSize(text.c_str()).x <= width - 12.0f) return text;
            std::string cut = text;
            while (!cut.empty() && ImGui::CalcTextSize((cut + "...").c_str()).x > width - 12.0f) cut.pop_back();
            return cut + "...";
        };
        ImGui::GetWindowDrawList()->AddText(ImVec2(min.x + 6.0f, min.y + 2.0f), ImGui::GetColorU32(ImGuiCol_Text), fit(entry.label).c_str());
        ImGui::GetWindowDrawList()->AddText(ImVec2(min.x + 6.0f, min.y + 2.0f + line), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                            fit(entry.description).c_str());
        ImGui::SetItemTooltip("%s: click to add it to the scene", fit(entry.description).c_str());
        placeables_.push_back(Placeable{entry.label, entry.description, entry.kind, min, max});
        if (ImGui::IsItemClicked()) PlaceActor(entry.kind);
        ImGui::PopID();
        ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + height));
    }
    if (shown == 0) ImGui::TextDisabled("No actors match.");
}

Entity ScenePanel::PlaceActor(SceneEntityKind kind) {
    const char* name = kind == SceneEntityKind::Camera   ? "Camera"
                       : kind == SceneEntityKind::CineCamera ? "Cine Camera"
                       : kind == SceneEntityKind::Model      ? "Model"
                                                             : "Actor";
    const Entity made = document_.CreateEntity(name, kind);
    if (!made.IsNull()) {
        document_.SetSelection({made});
        status_ = std::string("Added ") + name;
    }
    return made;
}

void ScenePanel::DrawEntityMenu(Entity entity) {
    if (!document_.IsSelected(entity)) Select(entity, false); // a right click acts on what it landed on
    DrawCreateMenu(entity);
    if (ImGui::MenuItem("Rename", "F2")) BeginRename(entity);
    if (ImGui::MenuItem("Duplicate", "Ctrl+D")) DuplicateSelection();
    if (ImGui::MenuItem("Copy", "Ctrl+C")) document_.CopySelection();
    if (ImGui::MenuItem("Paste", "Ctrl+V", false, document_.HasClipboard())) document_.PasteClipboard();
    const std::vector<Entity> acted_on = DragSet(entity); // the selection when this row is part of it, as Delete
    const bool any_hidden = std::any_of(acted_on.begin(), acted_on.end(), [&](Entity e) { return !document_.IsActive(e); });
    if (ImGui::MenuItem(any_hidden ? "Show" : "Hide")) SetHidden(acted_on, !any_hidden);
    const bool any_unlocked = std::any_of(acted_on.begin(), acted_on.end(), [&](Entity e) { return !document_.IsLocked(e); });
    if (ImGui::MenuItem(any_unlocked ? "Lock" : "Unlock")) SetLocked(acted_on, any_unlocked);
    ImGui::BeginDisabled(std::all_of(acted_on.begin(), acted_on.end(), [&](Entity e) { return document_.ParentOf(e).IsNull(); }));
    if (ImGui::MenuItem("Move to the scene root")) ReparentEntities(acted_on, kNullEntity);
    ImGui::EndDisabled();
    ImGui::Separator();
    if (ImGui::MenuItem("Delete", "Del")) DeleteSelection();
}

void ScenePanel::DrawNode(Entity entity, const HierarchyIndex& index, const std::unordered_set<Entity>* shown) {
    if (!document_.GetWorld().IsAlive(entity)) return;
    if (shown != nullptr && shown->count(entity) == 0) return;
    bool has_children = index.HasChildren(entity);
    if (shown != nullptr) {
        has_children = std::any_of(index.Children(entity).begin(), index.Children(entity).end(),
                                   [&](Entity child) { return shown->count(child) != 0; });
    }
    ImGui::PushID(static_cast<int>(entity.index));
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
    if (!has_children) flags |= ImGuiTreeNodeFlags_Leaf;
    if (document_.IsSelected(entity)) flags |= ImGuiTreeNodeFlags_Selected;
    if (shown != nullptr && has_children) ImGui::SetNextItemOpen(true, ImGuiCond_Always); // a match is never behind a collapse
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
        open = ImGui::TreeNodeEx("##node", flags);
        const ImVec2 row_min = ImGui::GetItemRectMin(), row_max = ImGui::GetItemRectMax();
        const bool row_hovered = ImGui::IsItemHovered();
        // The eye and the lock at the end of the row are not part of the row's click: pressing there acts on them, not
        // on the selection.
        const f32 toggle = ImGui::GetFontSize() + 2.0f;
        const f32 strip = toggle * 2.0f + 4.0f;
        const bool in_strip = ImGui::GetIO().MousePos.x >= row_max.x - strip;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen() && !in_strip) {
            // Pressing a row of a multi-selection keeps the selection until release, so that the press can start dragging all of it.
            const bool additive = ImGui::GetIO().KeyCtrl;
            if (!additive && document_.IsSelected(entity) && document_.Selection().size() > 1) press_target_ = entity;
            else Select(entity, additive);
        }
        if (press_target_ == entity && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            if (ImGui::IsItemHovered()) Select(entity, false);
            press_target_ = kNullEntity;
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !in_strip) BeginRename(entity);
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            if (press_target_ == entity) press_target_ = kNullEntity; // a drag, not a click: the selection stays
            const std::vector<Entity> carried = DragSet(entity);
            ImGui::SetDragDropPayload(kEntityPayload, carried.data(), carried.size() * sizeof(Entity));
            if (carried.size() > 1) ImGui::Text("%zu entities", carried.size());
            else ImGui::TextUnformatted(document_.Name(entity).c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload); payload != nullptr) {
                const std::vector<Entity> dragged = PayloadEntities(*payload);
                ReparentEntities(dragged, entity);
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
        const World& world = document_.GetWorld();
        if (world.IsAlive(entity)) { // the menu can delete it
            const EntityKind kind = ClassifyEntity(world, entity);
            const PrefabRowState prefab = PrefabRowStateOf(world, document_.Guids(), document_.Context().find_prefab, entity);
            const ImVec2 badge = DrawRowLabel(row_min, row_max, kind, prefab, document_.Name(entity), document_.IsHidden(entity));
            const f32 size = ImGui::GetFontSize();
            if (row_hovered && ImGui::IsMouseHoveringRect(badge, ImVec2(badge.x + size, badge.y + size)))
                ImGui::SetTooltip("%s", HierarchyRowTooltip(kind, prefab).c_str());
            // The eye and the lock, on the whole selection when this row is part of it. The cursor is put back afterwards
            // so the buttons, which sit inside the row, do not push the next row down.
            const ImVec2 row_end = ImGui::GetCursorScreenPos();
            const f32 top = row_min.y + (row_max.y - row_min.y - toggle) * 0.5f;
            const bool own_active = document_.IsActive(entity);
            if (RowToggle(ImVec2(row_max.x - strip, top), toggle, RowGlyph::Eye, own_active, "##eye",
                          own_active ? (document_.IsHidden(entity) ? "Its own flag is on; a parent hides it" : "Hide (and everything under it)")
                                     : "Show"))
                SetHidden(DragSet(entity), /*hidden=*/own_active);
            const bool locked = document_.IsLocked(entity);
            if (RowToggle(ImVec2(row_max.x - strip + toggle + 4.0f, top), toggle, RowGlyph::Lock, locked, "##lock",
                          locked ? "Unlock" : "Lock: not picked or moved in the viewport"))
                SetLocked(DragSet(entity), !locked);
            ImGui::SetCursorScreenPos(row_end);
            ImGui::Dummy(ImVec2(0.0f, 0.0f)); // the buttons moved the cursor inside the row; this closes the row without growing it
        }
    }
    if (open) {
        for (const Entity child : index.Children(entity)) {
            if (shown != nullptr && shown->count(child) == 0) continue;
            DrawNode(child, index, shown);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void ScenePanel::DrawHierarchy() {
    placeables_.clear();
    DrawHierarchyHeader();
    if (hierarchy_tab_ == HierarchyTab::Place) {
        DrawPlacePanel();
        return;
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##scene_filter", "Filter: name, name:term, type:Transform", &filter_);
    const std::string filter = filter_;
    const HierarchyQuery query = ParseHierarchyQuery(filter);
    // Dropping on the heading makes an entity a root again.
    // An item with an ID of its own and the full width: a drop is delivered only when the same target took it the frame before, and plain text has no ID (its
    // target flickered between two, so a drop on the heading was never delivered), and a heading as wide as the list is also easier to hit.
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    const std::string heading = std::to_string(document_.Entities().size()) + " entities";
    ImGui::Selectable((heading + "##hierarchy_heading").c_str(), false, ImGuiSelectableFlags_DontClosePopups);
    ImGui::PopStyleColor();
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayload); payload != nullptr) {
            const std::vector<Entity> dragged = PayloadEntities(*payload);
            ReparentEntities(dragged, kNullEntity);
        }
        // An asset dropped on the heading is placed at the root, at the scene's origin.
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kContentPathPayload); payload != nullptr && payload->Data != nullptr) {
            status_ = PlaceContentAsset(document_, content_root_, static_cast<const char*>(payload->Data), Vec3(0.0f, 0.0f, 0.0f));
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::Separator();
    if (query.Empty()) {
        const HierarchyIndex index(document_);
        for (const Entity root : index.Children(kNullEntity)) DrawNode(root, index);
    } else {
        // Matches stay in the tree with their ancestors rather than flattening into a list, so a match still reads as
        // where it lives ("Leaf B under Zebra"), and the chain is opened so it is on screen.
        const std::vector<Entity> matches = MatchingEntities(document_, filter);
        if (matches.empty()) {
            ImGui::TextDisabled("No entities match.");
        } else {
            const std::unordered_set<Entity> shown = VisibleEntities(document_, matches);
            const HierarchyIndex index(document_);
            for (const Entity root : index.Children(kNullEntity)) {
                if (shown.count(root) != 0) DrawNode(root, index, &shown);
            }
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) press_target_ = kNullEntity; // its row may not have been drawn on the release frame
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

// The Inspector's heading, where the concept puts the entity's identity: the name as the title (the rename field, so
// clicking it edits it) and what the entity is under it -- its kind, "Blueprint instance" for a prefab instance, and the
// first tag. Several entities get their count instead; the shared name stays the row below.
void ScenePanel::DrawInspectorHeader(std::span<const Entity> shown) {
    inspector_header_ = InspectorHeader{};
    if (shown.empty()) return;
    if (shown.size() == 1) {
        const Entity entity = shown.front();
        const World& world = document_.GetWorld();
        inspector_header_.title = document_.Name(entity);
        const PrefabRowState prefab = PrefabRowStateOf(world, document_.Guids(), document_.Context().find_prefab, entity);
        std::string subtitle =
            prefab.role != PrefabRowState::Role::None ? "Blueprint instance" : EntityKindName(ClassifyEntity(world, entity));
        if (const Tags* tags = world.GetComponent<Tags>(entity); tags != nullptr && !tags->names.empty())
            subtitle += " · " + tags->names.front();
        inspector_header_.subtitle = std::move(subtitle);
        inspector_header_.editable = true;

        ImGui::PushFont(ImGui::GetFont(), ImGui::GetFontSize() * 1.3f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3.0f, 4.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(1.0f, 1.0f, 1.0f, 0.05f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(1.0f, 1.0f, 1.0f, 0.08f));
        char name[128];
        std::snprintf(name, sizeof(name), "%s", inspector_header_.title.c_str());
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("##inspector_title", name, sizeof(name))) document_.Rename(entity, name, false);
        if (ImGui::IsItemDeactivatedAfterEdit()) document_.Rename(entity, name, true);
        if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) ImGui::SetTooltip("Click to rename");
        inspector_header_.title_min = ImGui::GetItemRectMin();
        inspector_header_.title_max = ImGui::GetItemRectMax();
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
        ImGui::PopFont();
    } else {
        inspector_header_.title = std::to_string(shown.size()) + " entities";
        inspector_header_.subtitle = "Multiple selection";
        ImGui::PushFont(ImGui::GetFont(), ImGui::GetFontSize() * 1.3f);
        ImGui::TextUnformatted(inspector_header_.title.c_str());
        inspector_header_.title_min = ImGui::GetItemRectMin();
        inspector_header_.title_max = ImGui::GetItemRectMax();
        ImGui::PopFont();
    }
    ImGui::TextDisabled("%s", inspector_header_.subtitle.c_str());
    inspector_header_.subtitle_min = ImGui::GetItemRectMin();
    inspector_header_.subtitle_max = ImGui::GetItemRectMax();
    if (shown.size() == 1) {
        // The header's controls, where the concept puts them: the enabled box (the Active component), the tag
        // dropdown and the layer dropdown.
        const Entity entity = shown.front();
        bool active = document_.IsActive(entity);
        if (ImGui::Checkbox("Enabled", &active)) document_.SetActive(entity, active);
        inspector_header_.enabled_min = ImGui::GetItemRectMin();
        inspector_header_.enabled_max = ImGui::GetItemRectMax();
        inspector_header_.controls = true;
        ImGui::SetItemTooltip("An inactive entity and everything under it is not drawn, picked or updated");
        ImGui::SameLine();
        DrawInspectorTags(entity);
        ImGui::SameLine();
        DrawInspectorLayer(entity);
    }
    ImGui::Separator();
}

// The tag dropdown: the tags the scene uses, each a checkbox on this entity's Tags component. With the menu closed it
// reads the entity's first tag (or "Tags" while it has none). Toggling adds or removes one name and is one undo step,
// adding the Tags component when the entity had none.
void ScenePanel::DrawInspectorTags(Entity entity) {
    const Tags* tags = document_.GetWorld().GetComponent<Tags>(entity);
    inspector_header_.tag_label = tags != nullptr && !tags->names.empty() ? tags->names.front() : "Tags";
    if (DrawHeaderChip("##inspector_tags", inspector_header_.tag_label, tags == nullptr || tags->names.empty(), inspector_header_.tag_min,
                       inspector_header_.tag_max))
        ImGui::OpenPopup("##inspector_tags_menu");
    ImGui::SetItemTooltip("The entity's gameplay tags");
    if (!ImGui::BeginPopup("##inspector_tags_menu")) return;
    const std::vector<std::string> used = SceneTags(document_);
    for (const std::string& name : used) {
        const Tags* current = document_.GetWorld().GetComponent<Tags>(entity); // re-read: a toggle may have added the component
        bool member = current != nullptr && std::find(current->names.begin(), current->names.end(), name) != current->names.end();
        if (ImGui::Checkbox(name.c_str(), &member)) {
            std::vector<std::string> names = current != nullptr ? current->names : std::vector<std::string>{};
            if (member) names.push_back(name);
            else names.erase(std::remove(names.begin(), names.end(), name), names.end());
            SetEntityTags(entity, std::move(names));
        }
        inspector_header_.tag_items.push_back(name);
        inspector_header_.tag_item_min.push_back(ImGui::GetItemRectMin());
        inspector_header_.tag_item_max.push_back(ImGui::GetItemRectMax());
    }
    if (used.empty()) ImGui::TextDisabled("No entity in this scene has a tag yet.");
    ImGui::EndPopup();
}

// The layer dropdown: the project's layer names (ProjectSettings::layers), the current one ticked, or "Layer N" for an
// index the project has no name for. Choosing one sets the Layer component, adding it when the entity had none; one
// undo step.
void ScenePanel::DrawInspectorLayer(Entity entity) {
    const Layer* layer = document_.GetWorld().GetComponent<Layer>(entity);
    const u8 index = layer != nullptr ? layer->index : 0;
    const std::vector<std::string> fallback{"Default"};
    const std::vector<std::string>& names = layer_names_ != nullptr && !layer_names_->empty() ? *layer_names_ : fallback;
    inspector_header_.layer_label = index < names.size() ? names[index] : "Layer " + std::to_string(index);
    if (DrawHeaderChip("##inspector_layer", inspector_header_.layer_label, false, inspector_header_.layer_min, inspector_header_.layer_max))
        ImGui::OpenPopup("##inspector_layer_menu");
    ImGui::SetItemTooltip("The entity's collision layer");
    if (!ImGui::BeginPopup("##inspector_layer_menu")) return;
    const auto row = [&](const std::string& label, u8 value) {
        const bool chosen = ImGui::Selectable(label.c_str(), index == value);
        inspector_header_.layer_items.push_back(label);
        inspector_header_.layer_item_min.push_back(ImGui::GetItemRectMin());
        inspector_header_.layer_item_max.push_back(ImGui::GetItemRectMax());
        if (chosen) {
            SetEntityLayer(entity, value);
            ImGui::CloseCurrentPopup();
        }
    };
    for (usize i = 0; i < names.size(); ++i) row(names[i], static_cast<u8>(i));
    for (usize i = names.size(); i <= index && i < kMaxLayers; ++i) row("Layer " + std::to_string(i), static_cast<u8>(i)); // the scene's own index, even unnamed
    ImGui::EndPopup();
}

void ScenePanel::SetEntityTags(Entity entity, std::vector<std::string> names) {
    const ComponentId id = GetComponentId<Tags>();
    CommandStack& commands = document_.Commands();
    commands.BeginTransaction("Set Tags");
    if (!document_.GetWorld().HasComponent<Tags>(entity)) document_.AddComponent(entity, id);
    document_.SetComponentField(entity, id, "names", reflect::Any(names), true);
    commands.EndTransaction();
}

void ScenePanel::SetEntityLayer(Entity entity, u8 index) {
    const ComponentId id = GetComponentId<Layer>();
    CommandStack& commands = document_.Commands();
    commands.BeginTransaction("Set Layer");
    if (!document_.GetWorld().HasComponent<Layer>(entity)) document_.AddComponent(entity, id);
    document_.SetComponentField(entity, id, "index", reflect::Any(index), true);
    commands.EndTransaction();
}

void ScenePanel::DrawInspector() {
    std::vector<Entity> shown;
    for (const Entity entity : document_.Selection())
        if (document_.GetWorld().IsAlive(entity)) shown.push_back(entity);
    if (!shown.empty()) DrawInspectorHeader(shown);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##property_search", "Search properties", &property_search_);
    if (!property_search_.empty()) ImGui::SetItemTooltip("Only properties whose name or component matches are shown");
    if (shown.empty()) {
        ImGui::TextDisabled("Select an entity in the hierarchy to edit it.");
        return;
    }
    if (multi_renaming_ && shown.size() < 2) { // the name box went away mid-rename
        document_.Commands().EndTransaction();
        multi_renaming_ = false;
    }
    if (shown.size() > 1) {
        // One name for all of them: the shared one, or nothing when they differ. Typing is one undo step for the
        // whole selection (a transaction around the keystrokes), not one per entity.
        bool mixed = false;
        for (usize i = 1; i < shown.size() && !mixed; ++i) mixed = document_.Name(shown[i]) != document_.Name(shown[0]);
        char name[128] = {};
        if (!mixed) std::snprintf(name, sizeof(name), "%s", document_.Name(shown[0]).c_str());
        if (mixed) ImGui::InputTextWithHint("Name", "(multiple values)", name, sizeof(name));
        else ImGui::InputText("Name", name, sizeof(name));
        if (ImGui::IsItemEdited()) {
            if (!multi_renaming_) {
                document_.Commands().BeginTransaction("Rename " + std::to_string(shown.size()) + " entities");
                multi_renaming_ = true;
            }
            document_.RenameSelection(name, false);
        }
        if (ImGui::IsItemDeactivated()) {
            if (multi_renaming_) {
                if (ImGui::IsItemDeactivatedAfterEdit()) document_.RenameSelection(name, true);
                document_.Commands().EndTransaction();
                multi_renaming_ = false;
            }
        }
    }
    ImGui::Separator();
    if (shown.size() == 1) InspectEntity(document_.Context(), document_.Commands(), shown[0], property_search_);
    else InspectEntities(document_.Context(), document_.Commands(), shown, property_search_);
}

} // namespace aether::editor
