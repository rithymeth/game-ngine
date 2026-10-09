#pragma once

#include "aether/ecs/world.h"
#include "aether/scene/components.h"
#include "aether/scene/entity_guid.h"
#include "aether/reflection/any.h"
#include "core/command_stack.h"
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
// commands. Presentation hosts (Qt, ImGui, MCP) can share this state.
class SceneDocument {
public:
    SceneDocument();

    World& GetWorld() { return world_; }
    const World& GetWorld() const { return world_; }
    const std::filesystem::path& FilePath() const { return file_; }
    const GuidIndex& Guids() const { return guids_; }
    bool IsDirty() const { return commands_.IsDirty(); }
    bool CanUndo() const { return commands_.CanUndo(); }
    bool CanRedo() const { return commands_.CanRedo(); }
    std::string UndoLabel() const { return commands_.UndoLabel(); }
    std::string RedoLabel() const { return commands_.RedoLabel(); }

    void NewScene();
    bool Load(const std::filesystem::path& file, std::string* error = nullptr);
    bool Save(const std::filesystem::path& file = {}, std::string* error = nullptr);
    std::vector<Entity> Entities() const;
    std::string Name(Entity entity) const;
    bool Rename(Entity entity, const std::string& name, bool committed = true);
    Entity CreateEntity(const std::string& name, SceneEntityKind kind = SceneEntityKind::Empty,
                        const std::string& model_asset_path = {}, Entity parent = kNullEntity);
    Entity DuplicateEntity(Entity entity, const Vec3& offset = Vec3{1.0f, 0.0f, 1.0f});
    Entity ParentOf(Entity entity) const;
    bool ReparentEntity(Entity entity, Entity new_parent = kNullEntity);
    bool DestroyEntity(Entity entity);
    bool SetPosition(Entity entity, const Vec3& position, bool committed);
    bool SetRotation(Entity entity, const Quaternion& rotation, bool committed);
    bool SetCameraField(Entity entity, const std::string& field_name, const reflect::Any& value);
    bool SetCineCameraField(Entity entity, const std::string& field_name, const reflect::Any& value);
    bool Undo();
    bool Redo();
    PlaySession::State PlayState() const { return play_session_.GetState(); }
    void Play();
    void Pause();
    void Stop();

private:
    void Reset();

    World world_;
    GuidIndex guids_;
    CommandStack commands_;
    CommandContext context_;
    PlaySession play_session_;
    std::filesystem::path file_;
    EntityGuid pending_position_entity_{};
    std::optional<reflect::Any> pending_position_before_;
    EntityGuid pending_rotation_entity_{};
    std::optional<reflect::Any> pending_rotation_before_;
};

} // namespace aether::editor
