#pragma once

#include "core/recent_scenes.h"
#include "core/scene_document.h"
#include "world/model_bounds.h"

#include "aether/scene/prefab.h"

#include <imgui.h>

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aether::editor {

class CommandRegistry;
class FileDialog;
class SceneViewPanel;
struct FileDialogOptions;

// The entities of a scene as a tree, for the hierarchy: who is directly under whom (by the Parent component), each level by name and then by creation order. Built once from a document
// and read many times, so drawing a thousand entities does not scan the scene for every row.
class HierarchyIndex {
public:
    explicit HierarchyIndex(const SceneDocument& document);
    // The entities directly under `parent`; kNullEntity gives the roots.
    const std::vector<Entity>& Children(Entity parent) const;
    bool HasChildren(Entity parent) const { return !Children(parent).empty(); }

private:
    static u64 Key(Entity entity) { return (static_cast<u64>(entity.generation) << 32) | entity.index; }
    std::unordered_map<u64, std::vector<Entity>> children_;
    std::vector<Entity> none_;
};

// The entities whose name contains `filter` (without regard to case), by name; an empty filter matches all of them.
// The filter is also a small search grammar, like the Content Browser's: whitespace-separated terms all have to match,
// `name:<text>` (or a bare term) is part of the entity's name, and `type:<text>` matches a component's type name.
// Matching and the directives themselves ignore case; an empty directive (`type:`) is ignored.
struct HierarchyQuery {
    std::vector<std::string> name_terms;
    std::vector<std::string> type_terms;

    bool Empty() const { return name_terms.empty() && type_terms.empty(); }
    bool Matches(const SceneDocument& document, Entity entity) const;
};
HierarchyQuery ParseHierarchyQuery(std::string_view text);
std::vector<Entity> MatchingEntities(const SceneDocument& document, const std::string& filter);
// Every match plus its ancestors: what a filtered hierarchy has to draw so a match is still seen where it hangs.
std::unordered_set<Entity> VisibleEntities(const SceneDocument& document, const std::vector<Entity>& matches);

// The hierarchy column's two pages, as the concept's "Hierarchy | Place" header switches them: the open scene's tree, or
// the actor kinds that can be added to it.
enum class HierarchyTab : u8 { Hierarchy, Place };

// The Inspector column's two pages, as the concept's "Inspector | World" header switches them: the selected entity's properties, or the
// scene's own (its sun, sky and wind, under "World").
enum class InspectorTab : u8 { Inspector, World };

// The World page's parts: the scene's first DirectionalLight (the sun), SkyLight (the sky) and Wind (the wind).
enum class WorldPart : u8 { Sun, Sky, Wind };

// What a hierarchy row's badge says the entity is, judged by its component type names. When several apply, the earliest
// in this order wins (a camera with a light on it reads as a camera). Empty: nothing but name, transform, parenting,
// tags and prefab bookkeeping; Other: components none of the kinds claim.
enum class EntityKind : u8 { Camera, Light, Model, Particles, Audio, Ui, Physics, Script, Volume, Other, Empty };
EntityKind ClassifyEntity(const World& world, Entity entity);
EntityKind ComponentKind(std::string_view component_name); // "aether::PointLight" -> Light
const char* EntityKindName(EntityKind kind);
char EntityKindGlyph(EntityKind kind); // the letter in the badge
// The badge's colour, which the Scene View uses for the same kind too, so a colour means one thing in both.
ImVec4 EntityKindColor(EntityKind kind);

// A hierarchy row's prefab state. `missing_source` needs `find_prefab`; without one it is never set.
struct PrefabRowState {
    enum class Role : u8 { None, Root, Part };
    Role role = Role::None;
    usize overrides = 0;  // overrides of this entity (not of the rest of its instance)
    bool missing_source = false; // the instance's prefab cannot be found
};
PrefabRowState PrefabRowStateOf(const World& world, const GuidIndex& guids, const PrefabLookup& find_prefab, Entity entity);
// "Light", "Light - prefab instance (2 overrides)", "Empty - part of a prefab instance whose prefab is missing".
std::string HierarchyRowTooltip(EntityKind kind, const PrefabRowState& prefab);

// The scene editor of the portable (ImGui) editor: a toolbar (new, open, save, undo and redo, play), the hierarchy (filter, select, rename, create, duplicate, delete, copy and paste, and
// drag one entity onto another to make it a child) and the inspector for the selected entity (every reflected component, add and remove, all undoable). It edits a SceneDocument, the same
// document the MCP tools use, so what it saves is what they open. Fills the current ImGui window and runs headless in tests.
class ScenePanel {
public:
    // `content_root`: relative scene paths in the path box are under it.
    explicit ScenePanel(std::filesystem::path content_root = "Content");
    void Draw();

    SceneDocument& Document() { return document_; }
    const SceneDocument& Document() const { return document_; }
    // What the last action said ("Saved Scenes/Main.ascene", "The scene was not opened: ...").
    const std::string& Status() const { return status_; }
    // The path box's text.
    // The scenes under the content folder (.ascene and .aesc), as paths relative to it with forward
    // slashes, sorted; what the toolbar's list offers instead of a path to type.
    std::vector<std::string> ListScenes() const;
    // The folder scene paths are relative to (the open project's Content folder). Changing it leaves the open scene alone.
    void SetContentRoot(std::filesystem::path content_root) { content_root_ = std::move(content_root); bounds_.SetContentRoot(content_root_); }
    const std::filesystem::path& ContentRoot() const { return content_root_; }
    // The extents of the models the scene names, read from the content folder (for picking and outlines). Refreshed when a scene is opened.
    ModelBoundsCache& ModelBounds() { return bounds_; }
    const ModelBoundsCache& ModelBounds() const { return bounds_; }
    const std::string& Path() const { return path_; }
    void SetPath(const std::string& path) { path_ = path; }

    // Called after NewScene() or OpenScene() (or LoadRecovered()) actually replaced the document, so a host can
    // drop state that belonged to the scene that was open: the recovery autosave above all.
    std::function<void()> on_document_replaced;

    // The command table the panel's own keys come from (undo, redo and save while the panel has focus; duplicate,
    // delete, copy, paste and rename while the hierarchy does), so a chord is defined in one place. The registry
    // must outlive the panel; with none set the panel has no key handling.
    void SetCommands(const CommandRegistry* commands) { commands_ = commands; }

    // The native file picker the toolbar's "..." button and the File menu's Open and Save As use. Not owned; without
    // one, or where the platform has none, those actions keep their typed-path behaviour.
    void SetFileDialog(FileDialog* dialog) { dialog_ = dialog; }

    // The scene chosen in the native picker, as the path box would hold it (relative to the content folder when it is
    // under it). An empty string is a cancel; no value at all means there is no picker here, so the caller falls back
    // to the typed path rather than reporting a cancel. `save` asks for a name to write instead of a file to read.
    std::optional<std::string> PickSceneFile(bool save);

    // The scenes opened or saved recently, newest first, as the path box holds them. `file` is where they are kept
    // (Saved/recent_scenes.json in a project); setting it reads the list, and each change writes it back.
    void SetRecentScenesFile(const std::filesystem::path& file);
    const std::vector<std::string>& RecentScenePaths() const { return recent_scenes_.Entries(); }
    // Drops recent entries whose scene file no longer exists; returns how many went.
    std::size_t RemoveMissingRecentScenes();

    // The actions the toolbar and the hierarchy run, public so that a test (and a command palette) can run them without clicking. Each one that would discard unsaved changes is
    // refused with a status line unless `discard` is true (the toolbar asks first and then passes true).
    bool NewScene(bool discard = false);
    bool OpenScene(const std::string& path, bool discard = false);
    // Puts a recovery autosave on screen as the open scene, under the name of the scene it was saved from (`source`,
    // empty when that scene had never been saved). The document stays dirty: the recovered work is not on disk
    // until it is saved. Refused, with a status line, when the open scene has unsaved changes.
    bool LoadRecovered(const std::filesystem::path& autosave, const std::filesystem::path& source, bool discard = false);
    // Saves the open scene, or (with `path`) to another name. A file that already exists and is not the open scene is never replaced by a
    // plain save: it is refused with a message (Status()), because an untitled scene saved to the default name would otherwise overwrite
    // the project's real one. Saving to a name again right after that refusal (Save As twice), or with `overwrite`, replaces it.
    bool SaveScene(const std::string& path = {}, bool overwrite = false);
    // Drops `entity` under `parent` (kNullEntity: the scene root). False, and a status line, when that would put an entity under its own descendant, or it is already there.
    bool Reparent(Entity entity, Entity parent);
    // Drops several entities under `parent` as one undo step. One whose ancestor is also among them moves with that ancestor and keeps its
    // parent; one already there, or that would go under itself, is skipped. Returns how many moved; none moving with a cycle is a status line.
    usize ReparentEntities(std::span<const Entity> entities, Entity parent);
    // What dragging the hierarchy row of `entity` carries: the whole selection when the row is part of it, else just that entity.
    std::vector<Entity> DragSet(Entity entity) const;
    // Deletes everything selected, one undo step each.
    usize DeleteSelection();
    usize DuplicateSelection();
    // Renames while typing is merged into one undo step, and committed when it ends.
    void BeginRename(Entity entity);
    bool Renaming() const { return !rename_target_.IsNull(); }
    // The Inspector's property search: only properties whose field or component name contains it are drawn. Kept
    // across selections; empty shows everything.
    void SetPropertySearch(std::string text) { property_search_ = std::move(text); }
    const std::string& PropertySearch() const { return property_search_; }
    // The hierarchy filter (the grammar above). Kept across scenes; empty shows the whole tree.
    void SetHierarchyFilter(std::string text) { filter_ = std::move(text); }
    const std::string& HierarchyFilter() const { return filter_; }
    // The hierarchy column's header, where the concept puts its "Hierarchy | Place" tabs and the + and ... buttons.
    // Place shows the actor kinds the scene can add; a row adds one at the scene root and selects it (one undo step,
    // as the Actor menu and the Create menu do). The page, the header's rectangles and the rows' are what a host
    // aligns its chrome to and a test clicks.
    HierarchyTab ActiveHierarchyTab() const { return hierarchy_tab_; }
    void SetHierarchyTab(HierarchyTab tab) { hierarchy_tab_ = tab; }
    struct HierarchyHeader {
        HierarchyTab active = HierarchyTab::Hierarchy;
        ImVec2 hierarchy_min{}, hierarchy_max{}, place_min{}, place_max{};
        ImVec2 add_min{}, add_max{}, more_min{}, more_max{};
        // The footer's row: "+ Add entity" at the left, the entity count at the right (which also takes a drop for the scene root),
        // and the line under them that names where a dragged entity would land.
        ImVec2 footer_min{}, footer_max{};
        ImVec2 add_entity_min{}, add_entity_max{}, count_min{}, count_max{};
        std::string count_label; // "24 entities"
        std::string drop_hint;   // while an entity is dragged: "Drop to parent: <name>", or "Drop to the scene root"
    };
    const HierarchyHeader& LastHierarchyHeader() const { return hierarchy_header_; }
    struct Placeable {
        std::string label;
        std::string description;
        SceneEntityKind kind = SceneEntityKind::Empty;
        ImVec2 min{}, max{}; // the row's rectangle, so a test clicks it where a person would
    };
    const std::vector<Placeable>& LastPlaceables() const { return placeables_; }
    Entity PlaceActor(SceneEntityKind kind);
    // The eye and the lock on a hierarchy row, also on the row menu. Hiding sets the engine's Active flag (one undo step
    // for the whole set; a hidden entity is not drawn, picked or updated, and neither is anything under it). Locking is
    // editor-only and records no undo step: the viewport will not pick or move a locked entity.
    usize SetHidden(std::span<const Entity> entities, bool hidden);
    bool SetHidden(Entity entity, bool hidden) { return SetHidden(std::span<const Entity>(&entity, 1), hidden) != 0; }
    usize SetLocked(std::span<const Entity> entities, bool locked);
    bool SetLocked(Entity entity, bool locked) { return SetLocked(std::span<const Entity>(&entity, 1), locked) != 0; }
    bool IsHidden(Entity entity) const { return document_.IsHidden(entity); }
    bool IsLocked(Entity entity) const { return document_.IsLocked(entity); }

    // The view drawn between the hierarchy and the Inspector, as the editor concept lays the level editor out. Not owned; null
    // keeps the two-pane layout (the panel on its own, as tests and the portable editor use it).
    void SetSceneView(SceneViewPanel* view) { view_ = view; }
    // The three panes' rectangles last frame, in screen coordinates; the viewport is empty when no view is set.
    struct Layout {
        ImVec2 hierarchy_min{}, hierarchy_max{};
        ImVec2 viewport_min{}, viewport_max{};
        ImVec2 inspector_min{}, inspector_max{};
    };
    const Layout& LastLayout() const { return layout_; }

    // The Inspector's header, as the concept draws it ("BP_Player" over "Blueprint instance · Gameplay"): the
    // selection's name as the title -- the rename field itself, for a single entity -- and what it is under it: the
    // entity's kind from its components (the hierarchy badge's name), a prefab instance as "Blueprint instance", and
    // its first tag after a middle dot. Under one entity's identity sit its controls: the enabled box (the Active
    // component), a tag dropdown over the tags the scene uses, and a layer dropdown over the project's layers. Several
    // entities get their count instead of a name, and no controls.
    struct InspectorHeader {
        std::string title;
        std::string subtitle;
        ImVec2 title_min{}, title_max{};
        ImVec2 subtitle_min{}, subtitle_max{};
        bool editable = false; // the title is the name field, for a single selection
        bool controls = false; // the enabled, tag and layer controls were drawn (one entity)
        ImVec2 enabled_min{}, enabled_max{};
        ImVec2 tag_min{}, tag_max{};
        ImVec2 layer_min{}, layer_max{};
        std::string tag_label;   // what the tag chip reads: the first tag, or "Tags" when there is none
        std::string layer_label; // the layer's name, or "Layer N" for an index the project has no name for
        std::vector<std::string> tag_items; // the open tag popup's tags, top to bottom
        std::vector<ImVec2> tag_item_min, tag_item_max;
        std::vector<std::string> layer_items; // the open layer popup's names
        std::vector<ImVec2> layer_item_min, layer_item_max;
    };
    const InspectorHeader& LastInspectorHeader() const { return inspector_header_; }

    // The Inspector column's "Inspector | World" tabs, where the concept puts them: at the column's top, with ... at the right. World
    // edits the scene itself (the first DirectionalLight, SkyLight and Wind, each with an Add button when the scene has none), one undo
    // step per edit, and says so when the scene has several of one. The rectangles are what a test clicks.
    InspectorTab ActiveInspectorTab() const { return inspector_tab_; }
    void SetInspectorTab(InspectorTab tab) { inspector_tab_ = tab; }
    struct InspectorTabs {
        InspectorTab active = InspectorTab::Inspector;
        ImVec2 inspector_min{}, inspector_max{}, world_min{}, world_max{}, more_min{}, more_max{};
    };
    const InspectorTabs& LastInspectorTabs() const { return inspector_tabs_; }
    // The World page's entity of `part` (the first one that has the component), kNullEntity when the scene has none.
    Entity WorldPartEntity(WorldPart part) const;
    usize WorldPartCount(WorldPart part) const;
    // Creates "Sun", "Sky Light" or "Wind" with that component at the scene root and selects it; the World page's Add buttons, also
    // callable without a mouse. Each is a new entity and its component, as the Actor menu's items are.
    Entity AddWorldPart(WorldPart part);

    // The project's layer names, for the header's layer dropdown (ProjectSettings::layers). Not owned; null or empty
    // leaves the dropdown with "Default" only. The workspace points it at its open project's settings.
    void SetLayerNames(const std::vector<std::string>* names) { layer_names_ = names; }

private:
    enum class Pending : u8 { None, New, Open };

    std::filesystem::path Resolve(const std::string& path) const;
    FileDialogOptions SceneFileDialogOptions(bool save) const;
    std::string DisplayPath(const std::filesystem::path& absolute) const;
    void RecordRecent(const std::string& path);
    void DrawToolbar();
    void DrawHierarchy();
    void DrawHierarchyHeader();
    void DrawHierarchyFooter();
    void DrawPlacePanel();
    void DrawNode(Entity entity, const HierarchyIndex& index, const std::unordered_set<Entity>* shown = nullptr);
    void DrawEntityMenu(Entity entity);
    void DrawCreateMenu(Entity parent);
    void DrawCreateItems(Entity parent);
    void DrawInspector();
    void DrawInspectorTabs();
    void DrawWorldPage();
    void DrawInspectorHeader(std::span<const Entity> shown);
    void DrawInspectorTags(Entity entity);
    void DrawInspectorLayer(Entity entity);
    void SetEntityTags(Entity entity, std::vector<std::string> names);
    void SetEntityLayer(Entity entity, u8 index);
    void DrawDiscardPrompt();
    void Select(Entity entity, bool additive);

    SceneDocument document_;
    const CommandRegistry* commands_ = nullptr;
    FileDialog* dialog_ = nullptr;
    SceneViewPanel* view_ = nullptr;
    Layout layout_;
    InspectorHeader inspector_header_;
    const std::vector<std::string>* layer_names_ = nullptr;
    std::filesystem::path content_root_;
    ModelBoundsCache bounds_;
    RecentScenes recent_scenes_;
    std::filesystem::path recent_scenes_file_;
    std::filesystem::path pending_overwrite_; // the file a save was just refused for, so that asking again by name can confirm it
    std::string path_ = "Scenes/Main.ascene";
    std::string status_;
    std::string filter_; // the hierarchy filter box
    HierarchyTab hierarchy_tab_ = HierarchyTab::Hierarchy;
    HierarchyHeader hierarchy_header_;
    InspectorTab inspector_tab_ = InspectorTab::Inspector;
    InspectorTabs inspector_tabs_;
    std::vector<Placeable> placeables_; // the Place tab's rows, as last drawn
    std::string place_filter_;          // the Place tab's search box
    Entity rename_target_ = kNullEntity;
    std::array<char, 128> rename_buffer_{};
    bool rename_focus_ = false;
    bool multi_renaming_ = false; // a multi-selection name edit has an open undo transaction
    Entity press_target_ = kNullEntity; // a selected row pressed without Ctrl: it becomes the whole selection on release unless it was dragged
    std::string property_search_; // the Inspector's property search box
    Pending pending_ = Pending::None;
};

} // namespace aether::editor
