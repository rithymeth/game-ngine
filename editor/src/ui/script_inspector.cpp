#include "ui/script_inspector.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <cfloat>
#include <string>

namespace aether::editor {

namespace {

constexpr ImVec4 kOverrideColor{0.35f, 0.65f, 1.0f, 1.0f};

nlohmann::json CurrentValue(const ExposedVariable& variable, const ScriptComponent& component, bool& overridden) {
    overridden = false;
    if (const ScriptProperty* property = component.FindProperty(variable.name)) {
        nlohmann::json value = nlohmann::json::parse(property->value, nullptr, /*allow_exceptions=*/false);
        if (!value.is_discarded() && MatchesKind(variable.kind, value)) {
            overridden = true;
            return value;
        }
    }
    return variable.default_value;
}

// Draws the value widget; true (with `value` updated) when edited.
bool DrawValue(const ExposedVariable& variable, nlohmann::json& value) {
    ImGui::SetNextItemWidth(-FLT_MIN);
    switch (variable.kind) {
    case ExposedVariable::Kind::Number: {
        f64 number = value.get<f64>();
        const f64 lo = variable.range_min;
        const f64 hi = variable.range_max;
        const bool changed = variable.has_range
                                 ? ImGui::SliderScalar("##v", ImGuiDataType_Double, &number, &lo, &hi, "%.3g")
                                 : ImGui::DragScalar("##v", ImGuiDataType_Double, &number, 0.1f, nullptr, nullptr, "%.3g");
        if (changed) {
            value = number;
        }
        return changed;
    }
    case ExposedVariable::Kind::Bool: {
        bool flag = value.get<bool>();
        if (ImGui::Checkbox("##v", &flag)) {
            value = flag;
            return true;
        }
        return false;
    }
    case ExposedVariable::Kind::String: {
        std::string text = value.get<std::string>();
        if (ImGui::InputText("##v", &text, ImGuiInputTextFlags_AutoSelectAll)) {
            value = text;
            return true;
        }
        return false;
    }
    case ExposedVariable::Kind::Vector: {
        float v[3] = {value[0].get<float>(), value[1].get<float>(), value[2].get<float>()};
        if (ImGui::DragFloat3("##v", v, 0.05f)) {
            value = nlohmann::json::array({v[0], v[1], v[2]});
            return true;
        }
        return false;
    }
    }
    return false;
}

} // namespace

bool InspectScriptVariables(const ScriptClassInfo& info, ScriptComponent& component, const char* id) {
    ImGui::PushID(id);
    if (!info.ok) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Script error: %s", info.error.c_str());
        ImGui::PopID();
        return false;
    }
    if (info.variables.empty()) {
        ImGui::TextDisabled("This script exposes no variables");
        ImGui::PopID();
        return false;
    }
    bool changed = false;
    if (ImGui::BeginTable("##script_vars", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.38f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.62f);
        for (const ExposedVariable& variable : info.variables) {
            ImGui::PushID(variable.name.c_str());
            bool overridden = false;
            nlohmann::json value = CurrentValue(variable, component, overridden);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            if (overridden) {
                const ImVec2 start = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(start.x - 4.0f, start.y),
                                                          ImVec2(start.x - 1.0f, start.y + ImGui::GetFrameHeight()),
                                                          ImGui::GetColorU32(kOverrideColor));
                ImGui::TextColored(kOverrideColor, "%s", variable.name.c_str());
            } else {
                ImGui::TextUnformatted(variable.name.c_str());
            }
            if (!variable.tooltip.empty() && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", variable.tooltip.c_str());
            }
            if (overridden && ImGui::BeginPopupContextItem("reset_menu", ImGuiPopupFlags_MouseButtonRight)) {
                if (ImGui::MenuItem("Reset to Default")) {
                    component.ResetProperty(variable.name);
                    changed = true;
                }
                ImGui::EndPopup();
            }

            ImGui::TableSetColumnIndex(1);
            if (DrawValue(variable, value)) {
                if (value == variable.default_value) {
                    component.ResetProperty(variable.name); // back to the default: no override
                } else {
                    component.SetProperty(variable.name, value);
                }
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
    return changed;
}

} // namespace aether::editor
