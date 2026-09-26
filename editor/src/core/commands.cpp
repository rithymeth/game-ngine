#include "core/commands.h"

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

} // namespace aether::editor
