#include "ui/error_overlay.h"

#include <imgui.h>

#include <algorithm>
#include <string>

namespace aether::editor {

int DrawErrorOverlay(const std::vector<ErrorOverlayItem>& items, int max_shown) {
    if (items.empty()) {
        return -1;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 corner(viewport->WorkPos.x + viewport->WorkSize.x - 12.0f,
                        viewport->WorkPos.y + viewport->WorkSize.y - 12.0f);
    ImGui::SetNextWindowPos(corner, ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.92f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
    int clicked = -1;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.28f, 0.06f, 0.06f, 1.0f));
    if (ImGui::Begin("##error_overlay", nullptr, flags)) {
        const int count = static_cast<int>(items.size());
        const int shown = std::min(count, std::max(max_shown, 1));
        if (count > shown) {
            ImGui::TextDisabled("+%d more", count - shown);
        }
        for (int i = count - shown; i < count; ++i) {
            const ErrorOverlayItem& item = items[static_cast<usize>(i)];
            ImGui::PushID(i);
            std::string location = item.source;
            if (item.line > 0) {
                location += ":" + std::to_string(item.line);
            }
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.55f, 1.0f));
            if (ImGui::Selectable(location.c_str(), false, ImGuiSelectableFlags_None)) {
                clicked = i;
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Click to open %s", item.source.c_str());
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(item.message.c_str());
            ImGui::PopID();
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    return clicked;
}

} // namespace aether::editor
