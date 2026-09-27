#pragma once

#include "aether/assets/asset_ref.h"
#include "aether/reflection/reflection.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace aether {

// One exposed variable's value on an entity, overriding the script's default.
struct ScriptProperty {
    std::string name;
    std::string value; // JSON: a number, boolean, string, or [x, y, z]
};

// Attaches a Luau script (a .luau asset) to an entity (Phase 11 step 3,
// docs/design/PHASE_SPECS.md §11.2). The script module returns a table (the
// "class"); each entity gets an instance of it, with the exposed variables
// below applied. Defined in the engine so scenes save and load it without
// the scripting module.
struct ScriptComponent {
    assets::AssetRef<assets::ScriptAsset> script;
    std::vector<ScriptProperty> properties; // only the overridden variables

    const ScriptProperty* FindProperty(const std::string& name) const;
    // Sets (or replaces) an override.
    void SetProperty(const std::string& name, const nlohmann::json& value);
    // Back to the script's default. True if there was an override.
    bool ResetProperty(const std::string& name);
};

// What a script class exposes to the Inspector: its top-level fields with a
// number, boolean, string or vector default (§11.2). Comments on the line(s)
// above add metadata:
//   --@range 0 100
//   --@tooltip How fast it spins
//   Spin.speed = 180
struct ExposedVariable {
    enum class Kind { Number, Bool, String, Vector };
    std::string name;
    Kind kind = Kind::Number;
    nlohmann::json default_value; // same JSON form as ScriptProperty::value
    bool has_range = false;
    f64 range_min = 0.0;
    f64 range_max = 0.0;
    std::string tooltip;
};

struct ScriptClassInfo {
    bool ok = false;
    std::string error;
    std::vector<ExposedVariable> variables; // in name order
    std::vector<std::string> callbacks;     // lifecycle functions it defines ("OnStart", ...)

    const ExposedVariable* Find(const std::string& name) const;
};

// Whether `value` (JSON) is a valid value for a variable of `kind`.
bool MatchesKind(ExposedVariable::Kind kind, const nlohmann::json& value);

} // namespace aether

AETHER_REFLECT(aether::ScriptProperty, 1, AETHER_FIELD(name), AETHER_FIELD(value))
AETHER_REFLECT(aether::ScriptComponent, 1,
    AETHER_FIELD(script, Field_EditAnywhere, {.tooltip = "The .luau script to run on this entity"}),
    AETHER_FIELD(properties, Field_ReadOnly)
)
