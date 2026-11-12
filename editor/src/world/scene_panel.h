#pragma once

#include "core/scene_document.h"

#include <array>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace aether::editor {

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
std::vector<Entity> MatchingEntities(const SceneDocument& document, const std::string& filter);

// The scene editor of the portable (ImGui) editor: a toolbar (new, open, save, undo and redo, play), the hierarchy (filter, select, rename, create, duplicate, delete, copy and paste, and
// drag one entity onto another to make it a child) and the inspector for the selected entity (every reflected component, add and remove, all undoable). It edits a SceneDocument, the same
// document the Qt editor uses, so what it saves is what either editor opens. Fills the current ImGui window and runs headless in tests.
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
    const std::string& Path() const { return path_; }
    void SetPath(const std::string& path) { path_ = path; }

    // The actions the toolbar and the hierarchy run, public so that a test (and a command palette) can run them without clicking. Each one that would discard unsaved changes is
    // refused with a status line unless `discard` is true (the toolbar asks first and then passes true).
    bool NewScene(bool discard = false);
    bool OpenScene(const std::string& path, bool discard = false);
    bool SaveScene(const std::string& path = {});
    // Drops `entity` under `parent` (kNullEntity: the scene root). False, and a status line, when that would put an entity under its own descendant, or it is already there.
    bool Reparent(Entity entity, Entity parent);
    // Deletes everything selected, one undo step each.
    usize DeleteSelection();
    usize DuplicateSelection();
    // Renames while typing is merged into one undo step, and committed when it ends.
    void BeginRename(Entity entity);
    bool Renaming() const { return !rename_target_.IsNull(); }

private:
    enum class Pending : u8 { None, New, Open };

    std::filesystem::path Resolve(const std::string& path) const;
    void DrawToolbar();
    void DrawHierarchy();
    void DrawNode(Entity entity, const HierarchyIndex& index);
    void DrawEntityMenu(Entity entity);
    void DrawCreateMenu(Entity parent);
    void DrawInspector();
    void DrawDiscardPrompt();
    void Select(Entity entity, bool additive);

    SceneDocument document_;
    std::filesystem::path content_root_;
    std::string path_ = "Scenes/Main.ascene";
    std::string status_;
    std::array<char, 128> filter_{};
    Entity rename_target_ = kNullEntity;
    std::array<char, 128> rename_buffer_{};
    bool rename_focus_ = false;
    Pending pending_ = Pending::None;
};

} // namespace aether::editor
