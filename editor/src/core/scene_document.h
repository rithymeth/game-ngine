#pragma once

#include "aether/ecs/world.h"
#include "aether/scene/components.h"
#include "aether/scene/entity_guid.h"
#include "aether/reflection/any.h"
#include "core/command_stack.h"
#include "core/commands.h" // EntitySnapshot, for the copy/paste clipboard
#include "core/editor_document.h"
#include "core/play_session.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace aether::editor {

enum class SceneEntityKind {
    Empty,
    Camera,
    CineCamera,
    Model,
};

// One open scene document: ECS data, stable entity IDs, and undoable editor
// commands. Presentation hosts (ImGui, MCP) can share this state. It is an
// EditorDocument (Stage 0 of the parity audit), so the shell can ask it for its
// name, unsaved state and undo history without knowing it is a scene.
class SceneDocument : public EditorDocument {
public:
    SceneDocument();

    World& GetWorld() { return world_; }
    const World& GetWorld() const { return world_; }
    const std::filesystem::path& FilePath() const override { return file_; }
    const char* Kind() const override { return "scene"; }
    // Phase 42.1: the scene's absolute origin. A JSON scene records it and its
    // Transforms are local to it, so a large world round-trips to the right place.
    void SetOrigin(const Vec3d& origin) { origin_ = origin; }
    const Vec3d& Origin() const { return origin_; }
    GuidIndex& Guids() { return guids_; }
    const GuidIndex& Guids() const { return guids_; }
    bool IsDirty() const override { return recovered_unsaved_ || commands_.IsDirty(); }
    // Goes up whenever the scene may have changed: a command, an undo or redo, a new or loaded scene, or Touch(). A viewer compares it with the
    // value it last drew to know whether to draw again. Not saved, and not undone by an undo (an undo is a change too).
    u64 Revision() const { return commands_.Revision() + local_revision_; }
    // For an edit that writes into the scene directly while it is in progress (a gizmo drag), so a viewer sees it before it is recorded.
    void Touch() { ++local_revision_; }
    // Like Touch(), for a gizmo drag that writes only Transforms. It is a change (Revision goes up) but not a structural one.
    void TouchTransforms() { ++local_revision_; ++touched_revision_; }
    // Revision() less the changes a viewer that already runs the scene can take without loading it again (LiveViewport::PatchScene): a field set through
    // the inspector, SetPosition, SetRotation, SetScale or any other SetFieldCommand, a component added or removed on an entity that stays, an entity
    // created or destroyed, and TouchTransforms. TakeComponentEdits says which components; the Transforms the viewer reads whole, and the entities that came
    // or went it finds by comparing with what it last saw. An undo or redo counts the same way when everything it undoes does.
    u64 StructureRevision() const { return Revision() - commands_.PatchableRevision() - touched_revision_; }
    // The (entity, component) pairs set since the last call (see CommandStack::TakeComponentEdits); `overflowed` says the list is incomplete.
    std::vector<CommandStack::ComponentEdit> TakeComponentEdits(bool* overflowed = nullptr) { return commands_.TakeComponentEdits(overflowed); }
    bool CanUndo() const override { return commands_.CanUndo(); }
    bool CanRedo() const override { return commands_.CanRedo(); }
    std::string UndoLabel() const override { return commands_.UndoLabel(); }
    std::string RedoLabel() const override { return commands_.RedoLabel(); }
    std::string UndoAttribution() const { return commands_.UndoAttribution(); }
    std::string RedoAttribution() const { return commands_.RedoAttribution(); }

    void NewScene();
    bool Load(const std::filesystem::path& file, std::string* error = nullptr);
    bool Save(const std::filesystem::path& file = {}, std::string* error = nullptr) override;
    bool SaveAutosave(const std::filesystem::path& file, std::string* error = nullptr) const;
    bool LoadAutosave(const std::filesystem::path& autosave, const std::filesystem::path& source,
                      std::string* error = nullptr);
    std::vector<Entity> Entities() const;
    // Phase 43.3: multi-selection, so a gizmo can act on several entities at once. Plain data,
    // no undo entry of its own -- selecting is not an edit. An edit that removes an entity
    // leaves a stale handle in the list; a consumer checks before acting on one.
    const std::vector<Entity>& Selection() const { return selection_; }
    void SetSelection(std::vector<Entity> entities);
    void ToggleSelection(Entity entity); // adds it, or removes it if already selected
    bool IsSelected(Entity entity) const;
    void ClearSelection();
    // Phase 43.3: copy/paste of the selection. The clipboard holds entity *snapshots*, not handles,
    // so a paste still works after the originals are deleted or the scene is replaced. Paste gives
    // each new entity a fresh GUID and records an undoable command per entity, like DuplicateEntity.
    bool CopySelection();
    usize PasteClipboard(const Vec3& offset = Vec3{0.0f, 0.0f, 0.0f});
    bool HasClipboard() const { return !clipboard_.empty(); }
    std::string Name(Entity entity) const;
    bool Rename(Entity entity, const std::string& name, bool committed = true);
    Entity CreateEntity(const std::string& name, SceneEntityKind kind = SceneEntityKind::Empty,
                        const std::string& model_asset_path = {}, Entity parent = kNullEntity);
    Entity DuplicateEntity(Entity entity, const Vec3& offset = Vec3{1.0f, 0.0f, 1.0f});
    Entity ParentOf(Entity entity) const;
    bool ReparentEntity(Entity entity, Entity new_parent = kNullEntity);
    bool DestroyEntity(Entity entity);
    bool AddComponent(Entity entity, ComponentId component);
    bool RemoveComponent(Entity entity, ComponentId component);
    bool SetComponentField(Entity entity, ComponentId component, const std::string& field,
                           const reflect::Any& value, bool committed = true);
    bool SetPosition(Entity entity, const Vec3& position, bool committed);
    bool SetRotation(Entity entity, const Quaternion& rotation, bool committed);
    bool SetScale(Entity entity, const Vec3& scale, bool committed);
    bool SetCameraField(Entity entity, const std::string& field_name, const reflect::Any& value);
    bool SetCineCameraField(Entity entity, const std::string& field_name, const reflect::Any& value);
    bool Undo() override;
    bool Redo() override;
    // For a tool panel that issues commands of its own (the entity inspector's field edits and component adds): they go through the document's own stack and
    // context, so they are undoable with everything else and mark the scene as changed.
    CommandContext& Context() { return context_; }
    CommandStack& Commands() { return commands_; }
    PlaySession::State PlayState() const { return play_session_.GetState(); }
    void Play();
    void Pause();
    // While paused, lets exactly one simulation frame run (ShouldSimulate() is true for the next call only).
    void Step();
    void Stop();
    // Whether the host should advance the game this frame: every frame while playing, once after Step() while paused.
    bool ShouldSimulate() { return play_session_.ShouldSimulate(); }

private:
    void Reset();

    World world_;
    GuidIndex guids_;
    Vec3d origin_;
    std::vector<Entity> selection_; // Phase 43.3: multi-selection, not an undoable edit
    std::vector<EntityGuid> play_selection_; // authored selection, captured once on entry to Play; handles do not survive Stop
    std::vector<EntitySnapshot> clipboard_; // Phase 43.3: snapshots, so paste outlives the originals
    CommandStack commands_;
    CommandContext context_;
    PlaySession play_session_;
    std::filesystem::path file_;
    bool recovered_unsaved_ = false;
    u64 local_revision_ = 0;
    u64 touched_revision_ = 0; // how much of Revision() was TouchTransforms (values written without a command)
    EntityGuid pending_position_entity_{};
    std::optional<reflect::Any> pending_position_before_;
    EntityGuid pending_rotation_entity_{};
    std::optional<reflect::Any> pending_rotation_before_;
    EntityGuid pending_scale_entity_{};
    std::optional<reflect::Any> pending_scale_before_;
};

} // namespace aether::editor
