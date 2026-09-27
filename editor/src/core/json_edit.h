#pragma once

#include "aether/core/base.h"

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <vector>

namespace aether::editor {

// Enum-valued keys and their choices, shown as combos.
using JsonEnums = std::map<std::string, std::vector<const char*>>;

// One JSON value as an ImGui field, for editors that edit assets through
// their saved form (the UI Designer, the particle editor): booleans,
// numbers (dragged: `dragging` says a change is part of a drag, for undo
// merging), strings (Enter commits; `enums` turns known keys into combos),
// float2/float4 arrays (colours for keys naming one), string lists, and
// objects as trees. True when the value changed.
bool EditJsonField(const std::string& key, nlohmann::json& v, bool& dragging, const JsonEnums* enums = nullptr);

} // namespace aether::editor
