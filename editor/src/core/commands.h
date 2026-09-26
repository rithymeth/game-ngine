#pragma once

#include "aether/reflection/reflection.h"
#include "aether/scene/entity_guid.h"
#include "aether/scene/hierarchy.h"
#include "aether/scene/prefab.h"
#include "core/command_stack.h"

#include <memory>
#include <string>
#include <vector>

// The editor's built-in undoable commands (Phase 7 step 3, see
// docs/design/PHASE_SPECS.md §7.2). They work for any component, through the
// ECS's type-erased component registry and reflection, and refer to entities
// only by EntityGuid.

namespace aether::editor {

// Every component of one entity, serialized with each component's own
// ComponentInfo::serialize (reflected, raw or custom alike), so an entity can
// be destroyed and later recreated exactly — with the same GUID, since its
// IdComponent is one of the components. Live handles that serializers
// deliberately skip (a RigidBody's Jolt body) are rebuilt by
// EditorHooks::on_entity_created.
struct EntitySnapshot {
    struct Component {
        ComponentId id;
        std::vector<u8> bytes;
    };
    std::vector<Component> components;

    static EntitySnapshot Capture(const World& world, Entity entity);
    Entity Restore(World& world) const;
    usize MemoryBytes() const;
};

// Sets one top-level field of one component. Successive edits of the same
// field merge (MergePolicy::Allow), so a whole slider drag is one undo step.
class SetFieldCommand final : public ICommand {
public:
    SetFieldCommand(EntityGuid entity, ComponentId component, const reflect::FieldInfo& field, reflect::Any old_value,
                    reflect::Any new_value);
    void Do(CommandContext& ctx) override;
    void Undo(CommandContext& ctx) override;
    std::string Label() const override;
    bool TryMerge(const ICommand& next) override;
    usize MemoryBytes() const override;

private:
    void Apply(CommandContext& ctx, const reflect::Any& value);

    EntityGuid entity_;
    ComponentId component_;
    const reflect::FieldInfo* field_;
    reflect::Any old_value_;
    reflect::Any new_value_;
};

// Creates an entity from a snapshot (Do) / destroys it (Undo).
class CreateEntityCommand final : public ICommand {
public:
    // The snapshot must include an IdComponent (see EnsureGuid).
    CreateEntityCommand(EntitySnapshot snapshot, std::string label = "Create Entity");
    // For an entity that already exists (spawned by other code): captures it,
    // giving it a GUID first if it has none. Use with CommandStack::Record.
    static std::unique_ptr<CreateEntityCommand> FromExisting(CommandContext& ctx, Entity entity,
                                                             std::string label = "Create Entity");

    void Do(CommandContext& ctx) override;
    void Undo(CommandContext& ctx) override;
    std::string Label() const override { return label_; }
    usize MemoryBytes() const override { return sizeof(*this) + snapshot_.MemoryBytes(); }

private:
    EntitySnapshot snapshot_;
    EntityGuid guid_;
    std::string label_;
};

// Destroys an entity (Do) / recreates it with every component and the same
// GUID (Undo). The snapshot is taken at Do time, so redo after later edits
// destroys what's actually there.
class DestroyEntityCommand final : public ICommand {
public:
    explicit DestroyEntityCommand(EntityGuid entity, std::string label = "Delete Entity");
    void Do(CommandContext& ctx) override;
    void Undo(CommandContext& ctx) override;
    std::string Label() const override { return label_; }
    usize MemoryBytes() const override { return sizeof(*this) + snapshot_.MemoryBytes(); }

private:
    EntityGuid guid_;
    EntitySnapshot snapshot_;
    std::string label_;
};

// Adds a default-constructed component (Do) / removes it (Undo).
class AddComponentCommand final : public ICommand {
public:
    AddComponentCommand(EntityGuid entity, ComponentId component);
    void Do(CommandContext& ctx) override;
    void Undo(CommandContext& ctx) override;
    std::string Label() const override;

private:
    EntityGuid entity_;
    ComponentId component_;
};

// Removes a component (Do) / restores it with its data (Undo).
class RemoveComponentCommand final : public ICommand {
public:
    RemoveComponentCommand(EntityGuid entity, ComponentId component);
    void Do(CommandContext& ctx) override;
    void Undo(CommandContext& ctx) override;
    std::string Label() const override;
    usize MemoryBytes() const override { return sizeof(*this) + bytes_.size(); }

private:
    EntityGuid entity_;
    ComponentId component_;
    std::vector<u8> bytes_;
};

// Makes `new_parent` the parent of an entity (a null GUID detaches it to the
// root), remembering what it was attached to before. The child's Transform
// is kept as-is, i.e. interpreted relative to its new parent.
class ReparentCommand final : public ICommand {
public:
    ReparentCommand(EntityGuid entity, EntityGuid new_parent);
    void Do(CommandContext& ctx) override;
    void Undo(CommandContext& ctx) override;
    std::string Label() const override;

private:
    void Attach(CommandContext& ctx, bool has_parent, const EntityGuid& parent);

    EntityGuid entity_;
    EntityGuid new_parent_;
    bool had_parent_ = false;
    EntityGuid old_parent_;
};

// "Revert to Prefab" for part of a prefab instance (§9.3): removes the
// overrides of `entity`'s `component` at `field_path` (and inside it; "" =
// the whole component, ComponentId kInvalidComponentId = every component of
// the entity) and resolves the instance. Undo puts the overrides back.
// Needs CommandContext::find_prefab.
class RevertPrefabOverrideCommand final : public ICommand {
public:
    RevertPrefabOverrideCommand(EntityGuid entity, ComponentId component, std::string field_path);
    void Do(CommandContext& ctx) override;
    void Undo(CommandContext& ctx) override;
    std::string Label() const override;
    usize MemoryBytes() const override;

private:
    void Resolve(CommandContext& ctx, Entity root);

    EntityGuid entity_;
    ComponentId component_;
    std::string field_path_;
    std::vector<PropertyOverride> before_; // the instance's overrides before Do
};

// Turns an edit a widget already made in place into an undoable command: if
// the field's value differs from `old_value`, puts the old value back and
// executes a SetFieldCommand to the new one (merging with the previous edit
// of the same field, so a drag is one step). If `committed` (the widget's
// edit just finished), also breaks the merge chain. Returns true if a
// command was executed.
bool CommitFieldEdit(CommandContext& ctx, CommandStack& stack, const EntityGuid& entity, ComponentId component,
                     const reflect::FieldInfo& field, void* component_data, const reflect::Any& old_value,
                     bool committed);

// Gives every entity in the world a unique GUID and reindexes: call after
// loading a scene or at startup. Returns how many GUIDs were created or
// replaced (duplicates).
usize EnsureAllGuids(World& world, GuidIndex& guids);

} // namespace aether::editor
