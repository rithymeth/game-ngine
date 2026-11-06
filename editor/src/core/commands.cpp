#include "core/commands.h"

#include "aether/scene/data_layers.h"
#include "aether/scene/level_instance.h"
#include "aether/scene/prefab.h"
#include "aether/scene/gameplay.h"

#include "aether/core/log.h"
#include "aether/reflection/serialize.h"

namespace aether::editor {

namespace {

// Resolves a GUID; commands only run against states where the entity exists,
// so a miss is a bug in the command sequence.
Entity Resolve(CommandContext& ctx, const EntityGuid& guid) {
    Entity entity = ctx.guids.Find(ctx.world, guid);
    AETHER_ASSERT(!entity.IsNull());
    return entity;
}

// On a prefab instance, keeps its overrides in step with an edit. Runs on
// Do, Undo and Redo alike, so undoing an edit also undoes its override.
void RecordPrefabEdit(CommandContext& ctx, Entity entity, ComponentId component) {
    if (!ctx.find_prefab) {
        return;
    }
    const Entity root = FindInstanceRoot(ctx.world, ctx.guids, entity);
    if (root.IsNull()) {
        return;
    }
    if (const PrefabData* prefab = ctx.find_prefab(ctx.world.GetComponent<PrefabInstance>(root)->source.guid)) {
        RecordPrefabOverrides(ctx.world, ctx.guids, entity, component, *prefab);
    }
}

const char* ComponentName(ComponentId id) {
    const ComponentInfo& info = GetComponentInfo(id);
    return info.reflected != nullptr ? info.reflected->name : info.name;
}

} // namespace

// ---------------------------------------------------------------------------
// EntitySnapshot
// ---------------------------------------------------------------------------

EntitySnapshot EntitySnapshot::Capture(const World& world, Entity entity) {
    EntitySnapshot snapshot;
    for (ComponentId id = 0; id < RegisteredComponentCount(); ++id) {
        if (!world.HasComponentRaw(entity, id)) {
            continue;
        }
        const ComponentInfo& info = GetComponentInfo(id);
        Component component{id, {}};
        if (info.serialize) {
            info.serialize(world.GetComponentRaw(entity, id), component.bytes);
        }
        snapshot.components.push_back(std::move(component));
    }
    return snapshot;
}

Entity EntitySnapshot::Restore(World& world) const {
    ComponentMask mask;
    for (const Component& component : components) {
        mask.set(component.id);
    }
    Entity entity = world.CreateEntityRaw(mask);
    for (const Component& component : components) {
        const ComponentInfo& info = GetComponentInfo(component.id);
        if (info.deserialize) {
            info.deserialize(world.GetComponentRaw(entity, component.id), component.bytes.data(), component.bytes.size());
        }
    }
    return entity;
}

usize EntitySnapshot::MemoryBytes() const {
    usize total = sizeof(*this);
    for (const Component& component : components) {
        total += sizeof(component) + component.bytes.size();
    }
    return total;
}

// ---------------------------------------------------------------------------
// SetFieldCommand
// ---------------------------------------------------------------------------

SetFieldCommand::SetFieldCommand(EntityGuid entity, ComponentId component, const reflect::FieldInfo& field,
                                 reflect::Any old_value, reflect::Any new_value)
    : entity_(entity), component_(component), field_(&field), old_value_(std::move(old_value)),
      new_value_(std::move(new_value)) {}

void SetFieldCommand::Apply(CommandContext& ctx, const reflect::Any& value) {
    Entity entity = Resolve(ctx, entity_);
    void* component = ctx.world.GetComponentRaw(entity, component_);
    AETHER_ASSERT(component != nullptr);
    field_->Set(component, value);
    RecordPrefabEdit(ctx, entity, component_);
    if (ctx.hooks != nullptr && ctx.hooks->on_field_changed) {
        ctx.hooks->on_field_changed(entity, component_, *field_);
    }
}

void SetFieldCommand::Do(CommandContext& ctx) { Apply(ctx, new_value_); }
void SetFieldCommand::Undo(CommandContext& ctx) { Apply(ctx, old_value_); }

std::string SetFieldCommand::Label() const {
    return std::string("Edit ") + ComponentName(component_) + "." + field_->name;
}

bool SetFieldCommand::TryMerge(const ICommand& next) {
    const auto* other = dynamic_cast<const SetFieldCommand*>(&next);
    if (other == nullptr || other->entity_ != entity_ || other->component_ != component_ || other->field_ != field_) {
        return false;
    }
    new_value_ = other->new_value_;
    return true;
}

usize SetFieldCommand::MemoryBytes() const {
    return sizeof(*this) + 2 * static_cast<usize>(field_->type->size);
}

// ---------------------------------------------------------------------------
// CreateEntityCommand / DestroyEntityCommand
// ---------------------------------------------------------------------------

CreateEntityCommand::CreateEntityCommand(EntitySnapshot snapshot, std::string label)
    : snapshot_(std::move(snapshot)), label_(std::move(label)) {
    // Decode the GUID from the snapshot's IdComponent record.
    const ComponentId id_component = GetComponentId<IdComponent>();
    for (const EntitySnapshot::Component& component : snapshot_.components) {
        if (component.id == id_component) {
            IdComponent id;
            GetComponentInfo(id_component).deserialize(&id, component.bytes.data(), component.bytes.size());
            guid_ = id.guid;
        }
    }
    AETHER_ASSERT(!guid_.IsNull());
}

std::unique_ptr<CreateEntityCommand> CreateEntityCommand::FromExisting(CommandContext& ctx, Entity entity,
                                                                       std::string label) {
    EnsureGuid(ctx.world, entity, &ctx.guids);
    return std::make_unique<CreateEntityCommand>(EntitySnapshot::Capture(ctx.world, entity), std::move(label));
}

void CreateEntityCommand::Do(CommandContext& ctx) {
    Entity entity = snapshot_.Restore(ctx.world);
    ctx.guids.Add(guid_, entity);
    if (ctx.hooks != nullptr && ctx.hooks->on_entity_created) {
        ctx.hooks->on_entity_created(entity);
    }
}

void CreateEntityCommand::Undo(CommandContext& ctx) {
    Entity entity = Resolve(ctx, guid_);
    // Re-capture: edits made after creation are undone before this runs, but
    // capturing keeps redo exact even for state no command tracked.
    snapshot_ = EntitySnapshot::Capture(ctx.world, entity);
    if (ctx.hooks != nullptr && ctx.hooks->on_entity_destroying) {
        ctx.hooks->on_entity_destroying(entity);
    }
    ctx.world.DestroyEntity(entity);
    ctx.guids.Remove(guid_);
}

DestroyEntityCommand::DestroyEntityCommand(EntityGuid entity, std::string label)
    : guid_(entity), label_(std::move(label)) {}

void DestroyEntityCommand::Do(CommandContext& ctx) {
    Entity entity = Resolve(ctx, guid_);
    snapshot_ = EntitySnapshot::Capture(ctx.world, entity);
    if (ctx.hooks != nullptr && ctx.hooks->on_entity_destroying) {
        ctx.hooks->on_entity_destroying(entity);
    }
    ctx.world.DestroyEntity(entity);
    ctx.guids.Remove(guid_);
}

void DestroyEntityCommand::Undo(CommandContext& ctx) {
    Entity entity = snapshot_.Restore(ctx.world);
    ctx.guids.Add(guid_, entity);
    if (ctx.hooks != nullptr && ctx.hooks->on_entity_created) {
        ctx.hooks->on_entity_created(entity);
    }
}

// ---------------------------------------------------------------------------
// AddComponentCommand / RemoveComponentCommand
// ---------------------------------------------------------------------------

AddComponentCommand::AddComponentCommand(EntityGuid entity, ComponentId component)
    : entity_(entity), component_(component) {}

void AddComponentCommand::Do(CommandContext& ctx) {
    Entity entity = Resolve(ctx, entity_);
    ctx.world.AddComponentRaw(entity, component_);
    if (ctx.hooks != nullptr && ctx.hooks->on_component_added) {
        ctx.hooks->on_component_added(entity, component_);
    }
}

void AddComponentCommand::Undo(CommandContext& ctx) {
    Entity entity = Resolve(ctx, entity_);
    if (ctx.hooks != nullptr && ctx.hooks->on_component_removing) {
        ctx.hooks->on_component_removing(entity, component_);
    }
    ctx.world.RemoveComponentRaw(entity, component_);
}

std::string AddComponentCommand::Label() const {
    return std::string("Add ") + ComponentName(component_);
}

RemoveComponentCommand::RemoveComponentCommand(EntityGuid entity, ComponentId component)
    : entity_(entity), component_(component) {}

void RemoveComponentCommand::Do(CommandContext& ctx) {
    Entity entity = Resolve(ctx, entity_);
    const ComponentInfo& info = GetComponentInfo(component_);
    bytes_.clear();
    if (info.serialize) {
        info.serialize(ctx.world.GetComponentRaw(entity, component_), bytes_);
    }
    if (ctx.hooks != nullptr && ctx.hooks->on_component_removing) {
        ctx.hooks->on_component_removing(entity, component_);
    }
    ctx.world.RemoveComponentRaw(entity, component_);
}

void RemoveComponentCommand::Undo(CommandContext& ctx) {
    Entity entity = Resolve(ctx, entity_);
    ctx.world.AddComponentRaw(entity, component_);
    const ComponentInfo& info = GetComponentInfo(component_);
    if (info.deserialize) {
        info.deserialize(ctx.world.GetComponentRaw(entity, component_), bytes_.data(), bytes_.size());
    }
    if (ctx.hooks != nullptr && ctx.hooks->on_component_added) {
        ctx.hooks->on_component_added(entity, component_);
    }
}

std::string RemoveComponentCommand::Label() const {
    return std::string("Remove ") + ComponentName(component_);
}

// ---------------------------------------------------------------------------
// ReparentCommand
// ---------------------------------------------------------------------------

ReparentCommand::ReparentCommand(EntityGuid entity, EntityGuid new_parent)
    : entity_(entity), new_parent_(new_parent) {}

void ReparentCommand::Attach(CommandContext& ctx, bool has_parent, const EntityGuid& parent) {
    Entity entity = Resolve(ctx, entity_);
    if (has_parent) {
        ctx.world.AddComponent(entity, Parent{parent}); // adds, or overwrites an existing one
    } else {
        ctx.world.RemoveComponent<Parent>(entity);
    }
}

void ReparentCommand::Do(CommandContext& ctx) {
    const Parent* current = ctx.world.GetComponent<Parent>(Resolve(ctx, entity_));
    had_parent_ = current != nullptr;
    old_parent_ = current != nullptr ? current->parent : EntityGuid{};
    Attach(ctx, !new_parent_.IsNull(), new_parent_);
}

void ReparentCommand::Undo(CommandContext& ctx) { Attach(ctx, had_parent_, old_parent_); }

std::string ReparentCommand::Label() const { return new_parent_.IsNull() ? "Unparent" : "Parent"; }

bool CommitFieldEdit(CommandContext& ctx, CommandStack& stack, const EntityGuid& entity, ComponentId component,
                     const reflect::FieldInfo& field, void* component_data, const reflect::Any& old_value,
                     bool committed) {
    bool executed = false;
    const void* current = field.Ptr(component_data);
    if (reflect::ToJson(*field.type, old_value.Data()) != reflect::ToJson(*field.type, current)) {
        reflect::Any new_value = field.Get(component_data);
        field.Set(component_data, old_value); // the command re-applies it (and fires hooks)
        stack.Execute(ctx, std::make_unique<SetFieldCommand>(entity, component, field, old_value, std::move(new_value)),
                      MergePolicy::Allow);
        executed = true;
    }
    if (committed) {
        stack.BreakMergeChain();
    }
    return executed;
}

usize EnsureAllGuids(World& world, GuidIndex& guids) {
    usize changed = 0;
    for (Entity duplicate : guids.Rebuild(world)) {
        RegenerateGuid(world, duplicate, &guids);
        ++changed;
    }
    std::vector<Entity> entities;
    world.ForEachArchetype([&](const Archetype& archetype) {
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            const Entity* chunk = archetype.EntityArray(c);
            entities.insert(entities.end(), chunk, chunk + archetype.ChunkEntityCount(c));
        }
    });
    // Fixed up after iterating: adding a component moves the entity to
    // another archetype, which would invalidate the iteration above.
    for (Entity entity : entities) {
        const IdComponent* id = world.GetComponent<IdComponent>(entity);
        if (id == nullptr || id->guid.IsNull()) {
            EnsureGuid(world, entity, &guids);
            ++changed;
        }
    }
    return changed;
}

// ---------------------------------------------------------------------------
// RevertPrefabOverrideCommand
// ---------------------------------------------------------------------------

RevertPrefabOverrideCommand::RevertPrefabOverrideCommand(EntityGuid entity, ComponentId component,
                                                         std::string field_path)
    : entity_(entity), component_(component), field_path_(std::move(field_path)) {}

RevertPrefabOverrideCommand::RevertPrefabOverrideCommand(EntityGuid entity, PrefabLocalId local_id,
                                                         ComponentId component, std::string field_path)
    : entity_(entity), component_(component), field_path_(std::move(field_path)), local_id_(local_id), has_local_id_(true) {}

void RevertPrefabOverrideCommand::Resolve(CommandContext& ctx, Entity root) {
    if (!ctx.find_prefab) {
        last_report_ = {false, "Prefab lookup is unavailable"};
        return;
    }
    // A plain entity has no PrefabInstance, whatever it does have: dereferencing it here crashed the
    // process when the command was built for an entity that is not part of an instance.
    const PrefabInstance* instance = ctx.world.GetComponent<PrefabInstance>(root);
    if (instance == nullptr) {
        last_report_ = {false, "That entity does not belong to a prefab instance"};
        return;
    }
    if (const PrefabData* prefab = ctx.find_prefab(instance->source.guid)) {
        last_report_ = ResolvePrefabInstance(ctx.world, ctx.guids, root, *prefab);
    } else {
        last_report_ = {false, "Prefab could not be loaded"};
    }
}

void RevertPrefabOverrideCommand::Do(CommandContext& ctx) {
    // Not editor::Resolve: that asserts the GUID names a live entity, and this
    // command's contract is the opposite -- a GUID whose entity is gone is a case
    // it reports on. Under a build with assertions on, going through Resolve
    // trapped here before the check below could run.
    const Entity entity = ctx.guids.Find(ctx.world, entity_);
    // A GUID that names nothing resolves to no entity, and FindInstanceRoot does not survive being handed
    // one -- so the command says so rather than walking with it. Every caller benefits, not just the one
    // that happened to check first.
    if (entity.IsNull()) {
        last_report_ = {false, "No entity with that GUID"};
        return;
    }
    const Entity root = FindInstanceRoot(ctx.world, ctx.guids, entity);
    const PrefabLink* link = ctx.world.GetComponent<PrefabLink>(entity);
    PrefabInstance* instance = root.IsNull() ? nullptr : ctx.world.GetComponent<PrefabInstance>(root);
    if (instance == nullptr || link == nullptr) {
        // Say why rather than returning silently: the caller shows LastReport to the user, and a command
        // that did nothing without a reason is worse than one that explains itself.
        last_report_ = {false, "That entity does not belong to a prefab instance"};
        return;
    }
    const PrefabLocalId local_id = has_local_id_ ? local_id_ : link->local_id;
    before_ = instance->overrides;
    const std::string component = component_ == kInvalidComponentId ? std::string() : ComponentName(component_);
    RevertOverrides(*instance, local_id, component, component_ == kInvalidComponentId ? std::string() : field_path_);
    Resolve(ctx, root);
}

void RevertPrefabOverrideCommand::Undo(CommandContext& ctx) {
    // As in Do: a missing entity is a normal outcome here, not a broken invariant.
    const Entity entity = ctx.guids.Find(ctx.world, entity_);
    if (entity.IsNull()) {
        return;
    }
    const Entity root = FindInstanceRoot(ctx.world, ctx.guids, entity);
    if (root.IsNull()) {
        return;
    }
    PrefabInstance* instance = ctx.world.GetComponent<PrefabInstance>(root);
    if (instance == nullptr) {
        return;
    }
    instance->overrides = before_;
    Resolve(ctx, root);
}

std::string RevertPrefabOverrideCommand::Label() const {
    if (component_ == kInvalidComponentId) {
        return "Revert to Prefab";
    }
    return std::string("Revert ") + ComponentName(component_) + (field_path_.empty() ? "" : "." + field_path_);
}

usize RevertPrefabOverrideCommand::MemoryBytes() const {
    usize bytes = sizeof(*this) + field_path_.size();
    for (const PropertyOverride& o : before_) {
        bytes += sizeof(o) + o.component.size() + o.field_path.size() + o.value.size();
    }
    return bytes;
}

// ---------------------------------------------------------------- data layers (Phase 42.6)

std::unique_ptr<ICommand> SetDataLayerActiveCommand::Make(const CommandContext& ctx, std::string layer, bool active) {
    auto command = std::make_unique<SetDataLayerActiveCommand>();
    command->layer_ = std::move(layer);
    command->active_ = active;
    // Snapshot every member before anything changes, so Undo can put each one back exactly.
    bool changes = false;
    ctx.world.ForEachArchetype([&command, &ctx, &changes, active](Archetype& archetype) {
        if ((archetype.Mask() & ComponentMaskOf<DataLayer>()) != ComponentMaskOf<DataLayer>()) return;
        for (usize chunk = 0; chunk < archetype.ChunkCount(); ++chunk) {
            const u32 count = archetype.ChunkEntityCount(chunk);
            if (count == 0) continue;
            Entity* entities = archetype.EntityArray(chunk);
            auto* layers = static_cast<DataLayer*>(archetype.ComponentArray(chunk, GetComponentId<DataLayer>()));
            for (u32 i = 0; i < count; ++i) {
                if (layers[i].layer != command->layer_) continue;
                SetDataLayerActiveCommand::Member member;
                member.entity = entities[i];
                const Active* current = ctx.world.GetComponent<Active>(entities[i]);
                member.has_active = current != nullptr;
                member.active = current == nullptr || current->active;
                member.captured = layers[i].captured;
                member.active_when_layer_on = layers[i].active_when_layer_on;
                command->members_.push_back(member);
                if (member.active != active) changes = true;
            }
        }
    });
    // Nothing to do: no layer by that name, or every member is already in the wanted state. Return null
    // so the caller does not record a history entry that changes nothing.
    if (!changes) return nullptr;
    return command;
}

void SetDataLayerActiveCommand::Do(CommandContext& ctx) { SetDataLayerActive(ctx.world, layer_, active_); }

void SetDataLayerActiveCommand::Undo(CommandContext& ctx) {
    for (const Member& member : members_) {
        if (!ctx.world.IsAlive(member.entity)) continue;
        DataLayer* layer = ctx.world.GetComponent<DataLayer>(member.entity);
        if (layer != nullptr) {
            layer->captured = member.captured;
            layer->active_when_layer_on = member.active_when_layer_on;
        }
        Active* current = ctx.world.GetComponent<Active>(member.entity);
        if (!member.has_active) {
            // It had no Active component before, so it should have none after.
            if (current != nullptr) ctx.world.RemoveComponent<Active>(member.entity);
            continue;
        }
        if (current == nullptr) {
            ctx.world.AddComponent<Active>(member.entity, Active{member.active});
        } else {
            current->active = member.active;
        }
    }
}

std::string SetDataLayerActiveCommand::Label() const {
    return std::string(active_ ? "Show " : "Hide ") + layer_ + " layer";
}

// ---------------------------------------------------------------- level instances (Phase 42.6)

std::unique_ptr<ICommand> ExpandLevelInstanceCommand::Make(const CommandContext& ctx, const EntityGuid& instance,
                                                           PrefabData prefab) {
    auto command = std::make_unique<ExpandLevelInstanceCommand>();
    command->instance_ = instance;
    command->prefab_ = std::move(prefab);
    command->instance_entity_ = ctx.guids.Find(ctx.world, instance);
    if (command->instance_entity_ != kNullEntity) {
        const LevelInstance* level = ctx.world.GetComponent<LevelInstance>(command->instance_entity_);
        command->expanded_before_ = level != nullptr && level->expanded;
    }
    return command;
}

void ExpandLevelInstanceCommand::Do(CommandContext& ctx) {
    created_.clear();
    if (instance_entity_ == kNullEntity || !ctx.world.IsAlive(instance_entity_)) return;
    LevelInstance* level = ctx.world.GetComponent<LevelInstance>(instance_entity_);
    if (level == nullptr || level->expanded) return; // already placed: nothing to do
    const Transform* placement = ctx.world.GetComponent<Transform>(instance_entity_);
    ResolveReport report;
    assets::AssetGuid source{}; // a scene is not a prefab asset; the instance is identified by its root
    const Entity root = InstantiatePrefab(ctx.world, ctx.guids, source, prefab_, placement, &report);    if (root.IsNull()) return;
    // Parent the placed scene under the instance so moving the instance moves the scene, and only then
    // mark it expanded -- the same order ExpandLevelInstances uses.
    const IdComponent* id = ctx.world.GetComponent<IdComponent>(instance_entity_);
    if (id != nullptr) ctx.world.AddComponent(root, Parent{id->guid});
    created_ = report.created_entities;
    level = ctx.world.GetComponent<LevelInstance>(instance_entity_);
    if (level != nullptr) level->expanded = true;
}

void ExpandLevelInstanceCommand::Undo(CommandContext& ctx) {
    if (instance_entity_ == kNullEntity || !ctx.world.IsAlive(instance_entity_)) return;
    // Reverse order, because the list is parents-first and destroying a parent leaves its children
    // alive with a Parent whose GUID no longer resolves.
    for (auto it = created_.rbegin(); it != created_.rend(); ++it) {
        if (ctx.world.IsAlive(*it)) ctx.world.DestroyEntity(*it);
    }
    created_.clear();
    LevelInstance* level = ctx.world.GetComponent<LevelInstance>(instance_entity_);
    if (level != nullptr) level->expanded = expanded_before_;
}

std::string ExpandLevelInstanceCommand::Label() const {
    return "Expand level instance";
}

} // namespace aether::editor
