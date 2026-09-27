#pragma once

#include "aether/scene/script_component.h"

namespace aether::editor {

// Draws a script's exposed variables for one entity (Phase 11 step 3,
// docs/design/PHASE_SPECS.md §11.2): a number field (clamped to --@range),
// a checkbox, a text field or an X/Y/Z row per variable, showing the
// script's default until the entity overrides it. Overridden rows are marked
// like prefab overrides and have a right-click "Reset to Default". Editing a
// value back to the default drops the override. --@tooltip text shows on
// hover.
//
// Returns true if `component.properties` changed this frame (the caller
// records the edit, e.g. as a command on the component's `properties`).
bool InspectScriptVariables(const ScriptClassInfo& info, ScriptComponent& component, const char* id = "script_vars");

} // namespace aether::editor
