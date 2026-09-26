#pragma once

#include "core/command_stack.h"

namespace aether::editor {

// The Inspector's body for one entity: every reflected component with
// inspectable fields, drawn with InspectObject, plus "+ Add Component" and a
// per-component "Remove Component" context menu. Every change goes through
// `stack` as an undoable command (SetField / AddComponent / RemoveComponent):
// a slider drag is merged into one undo step and the merge chain is broken
// when the edit is committed. Gives the entity a GUID if it has none, and
// shows it as a line of text rather than as an editable component.
void InspectEntity(CommandContext& ctx, CommandStack& stack, Entity entity);

} // namespace aether::editor
