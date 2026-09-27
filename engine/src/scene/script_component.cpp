#include "aether/scene/script_component.h"

#include <algorithm>

namespace aether {

const ScriptProperty* ScriptComponent::FindProperty(const std::string& name) const {
    for (const ScriptProperty& property : properties) {
        if (property.name == name) {
            return &property;
        }
    }
    return nullptr;
}

void ScriptComponent::SetProperty(const std::string& name, const nlohmann::json& value) {
    for (ScriptProperty& property : properties) {
        if (property.name == name) {
            property.value = value.dump();
            return;
        }
    }
    properties.push_back({name, value.dump()});
}

bool ScriptComponent::ResetProperty(const std::string& name) {
    auto it = std::find_if(properties.begin(), properties.end(), [&](const ScriptProperty& p) { return p.name == name; });
    if (it == properties.end()) {
        return false;
    }
    properties.erase(it);
    return true;
}

const ExposedVariable* ScriptClassInfo::Find(const std::string& name) const {
    for (const ExposedVariable& variable : variables) {
        if (variable.name == name) {
            return &variable;
        }
    }
    return nullptr;
}

bool MatchesKind(ExposedVariable::Kind kind, const nlohmann::json& value) {
    switch (kind) {
    case ExposedVariable::Kind::Number: return value.is_number();
    case ExposedVariable::Kind::Bool: return value.is_boolean();
    case ExposedVariable::Kind::String: return value.is_string();
    case ExposedVariable::Kind::Vector:
        return value.is_array() && value.size() == 3 &&
               std::all_of(value.begin(), value.end(), [](const nlohmann::json& c) { return c.is_number(); });
    }
    return false;
}

} // namespace aether
