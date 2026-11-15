#pragma once

#include "core/command_stack.h"
#include "aether/scene/prefab.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aether::editor {

// The Inspector's body for one entity: every reflected component with
// inspectable fields, drawn with InspectObject, plus "+ Add Component" and a
// per-component card menu (Copy / Paste Values / Reset / Revert to Prefab /
// Remove). Every change goes through
// `stack` as an undoable command (SetField / AddComponent / RemoveComponent):
// a slider drag is merged into one undo step and the merge chain is broken
// when the edit is committed. Gives the entity a GUID if it has none, and
// shows it as a line of text rather than as an editable component.
//
// A non-empty `search` shows only the components with a matching property
// and, inside them, only the matching rows: the property search. Nothing is
// added or removed while searching; "No properties match" says so.
void InspectEntity(CommandContext& ctx, CommandStack& stack, Entity entity, std::string_view search = {});

// The Inspector's body for a selection: the components every entity has, edited together with InspectObjects (a
// shared value stays one value, a mixed one is marked, and an edit is one undo step across the selection), plus
// Adding a component to the entities that lack it and removing one from all. One entity draws exactly as
// InspectEntity. `search` filters the shared components the same way.
void InspectEntities(CommandContext& ctx, CommandStack& stack, std::span<const Entity> entities,
                     std::string_view search = {});

// The components `entities` all have, in registration order: what the multi-selection Inspector shows.
std::vector<ComponentId> SharedComponents(const World& world, std::span<const Entity> entities);

// The component card menu («...» on the header, or right-click it), also callable without a mouse. Copy takes the
// first entity's component. Paste and Reset write every field the inspector shows (IsCopiedField) that differs on any
// of `entities` — the clipboard's values or a default-constructed component's — as one undo step that records prefab
// overrides like any edit. They return false and record nothing when the clipboard holds another type, an entity
// lacks the component, or nothing would change.
void CopyComponent(const World& world, Entity entity, ComponentId component);
bool PasteComponentValues(CommandContext& ctx, CommandStack& stack, std::span<const Entity> entities,
                          ComponentId component);
bool ResetComponent(CommandContext& ctx, CommandStack& stack, std::span<const Entity> entities, ComponentId component);

// One override of a prefab instance, as the Inspector's override list shows it.
struct InstanceOverride {
    PrefabLocalId local_id = 0;
    Entity entity = kNullEntity; // the instance's entity it is on; null when the prefab no longer has that entity
    std::string entity_name;
    std::string component;
    std::string field_path;      // "" = the whole component
    std::string value;           // the instance's value, as JSON text
    std::string prefab_value;    // the prefab's value at the same path, as JSON text; empty when orphaned
    bool orphaned = false;       // the prefab has no such entity, component or field: kept, but it applies to nothing
};

// Every override of the instance `entity` belongs to, in the instance's order. Empty when `entity` is not part of an
// instance or its prefab cannot be found (CommandContext::find_prefab).
std::vector<InstanceOverride> InstanceOverrides(const CommandContext& ctx, Entity entity);

// Removes one listed override of the instance `entity` belongs to, so the value goes back to the prefab's, as one undo
// step. False when there is no such override.
bool RevertInstanceOverride(CommandContext& ctx, CommandStack& stack, Entity entity, const InstanceOverride& row);

// Removes every override of the instance `entity` belongs to, on all its entities, as one undo step.
bool RevertWholeInstance(CommandContext& ctx, CommandStack& stack, Entity entity);

} // namespace aether::editor
