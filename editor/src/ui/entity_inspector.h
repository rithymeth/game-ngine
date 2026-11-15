#pragma once

#include "core/command_stack.h"

#include <span>
#include <string_view>
#include <vector>

namespace aether::editor {

// The Inspector's body for one entity: every reflected component with
// inspectable fields, drawn with InspectObject, plus "+ Add Component" and a
// per-component "Remove Component" context menu. Every change goes through
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

} // namespace aether::editor
