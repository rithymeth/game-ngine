#include "core/json_edit.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>

namespace aether::editor {

using nlohmann::json;

namespace {
bool IsColorKey(const std::string& key) { return key.find("color") != std::string::npos || key == "tint"; }
} // namespace

bool EditJsonField(const std::string& key, json& v, bool& dragging, const JsonEnums* enums) {
    const char* label = key.c_str();
    if (v.is_boolean()) {
        bool b = v.get<bool>();
        if (ImGui::Checkbox(label, &b)) return v = b, true;
        return false;
    }
    if (v.is_number_integer()) {
        int i = v.get<int>();
        if (ImGui::DragInt(label, &i)) return dragging = ImGui::IsMouseDragging(0), v = i, true;
        return false;
    }
    if (v.is_number()) {
        f32 f = v.get<f32>();
        if (ImGui::DragFloat(label, &f, 0.5f)) return dragging = ImGui::IsMouseDragging(0), v = f, true;
        return false;
    }
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        const std::vector<const char*>* choices = nullptr;
        if (enums != nullptr) {
            if (const auto found = enums->find(key); found != enums->end()) choices = &found->second;
        }
        if (choices != nullptr) {
            const auto it = enums->find(key);
            int current = 0;
            for (usize i = 0; i < it->second.size(); ++i) {
                if (s == it->second[i]) current = static_cast<int>(i);
            }
            if (ImGui::Combo(label, &current, it->second.data(), static_cast<int>(it->second.size()))) return v = it->second[static_cast<usize>(current)], true;
            return false;
        }
        if (ImGui::InputText(label, &s, ImGuiInputTextFlags_EnterReturnsTrue)) return v = s, true;
        return false;
    }
    if (v.is_array()) {
        const bool numbers = std::all_of(v.begin(), v.end(), [](const json& x) { return x.is_number(); });
        if (numbers && (v.size() == 2 || v.size() == 4)) {
            f32 f[4] = {0, 0, 0, 0};
            for (usize i = 0; i < v.size(); ++i) f[i] = v[i].get<f32>();
            bool changed = false;
            if (v.size() == 4 && IsColorKey(key)) changed = ImGui::ColorEdit4(label, f);
            else if (v.size() == 2) changed = ImGui::DragFloat2(label, f, 0.5f);
            else changed = ImGui::DragFloat4(label, f, 0.5f);
            if (!changed) return false;
            dragging = ImGui::IsMouseDragging(0);
            json out = json::array();
            for (usize i = 0; i < v.size(); ++i) out.push_back(f[i]);
            v = out;
            return true;
        }
        if (std::all_of(v.begin(), v.end(), [](const json& x) { return x.is_string(); })) {
            // A list of strings (a dropdown's options), one per line.
            std::string text;
            for (usize i = 0; i < v.size(); ++i) text += (i ? "\n" : "") + v[i].get<std::string>();
            if (ImGui::InputTextMultiline(label, &text, ImVec2(0, 70), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine)) {
                json out = json::array();
                usize start = 0;
                while (start <= text.size()) {
                    const usize nl = text.find('\n', start);
                    const std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
                    if (!line.empty()) out.push_back(line);
                    if (nl == std::string::npos) break;
                    start = nl + 1;
                }
                return v = out, true;
            }
            return false;
        }
        ImGui::TextDisabled("%s: %s", label, v.dump().c_str());
        return false;
    }
    if (v.is_object()) {
        bool changed = false;
        if (ImGui::TreeNode(label)) {
            for (auto& [k, item] : v.items()) {
                ImGui::PushID(k.c_str());
                changed |= EditJsonField(k, item, dragging, enums);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        return changed;
    }
    ImGui::TextDisabled("%s", label);
    return false;
}

} // namespace aether::editor
