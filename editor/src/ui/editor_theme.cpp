#include "ui/editor_theme.h"

#include <imgui.h>

#include <system_error>
#include <vector>

namespace aether::editor {

namespace {

ImVec4 Rgb(int r, int g, int b, float a = 1.0f) {
    return ImVec4(static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f, a);
}

std::filesystem::path FindFont() {
    std::vector<std::filesystem::path> candidates;
    std::filesystem::path dir = std::filesystem::current_path();
    for (int up = 0; up < 5; ++up) {
        candidates.push_back(dir / "assets" / "fonts" / "Roboto-Medium.ttf");
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    for (const auto& c : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(c, ec)) return c;
    }
    return {};
}

} // namespace

bool ApplyEditorTheme(float font_size, const std::filesystem::path& font_file) {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding = ImVec2(10, 10);
    s.FramePadding = ImVec2(8, 5);
    s.ItemSpacing = ImVec2(8, 6);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.IndentSpacing = 18.0f;
    s.ScrollbarSize = 13.0f;
    s.GrabMinSize = 10.0f;
    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.PopupBorderSize = 1.0f;
    s.WindowRounding = 6.0f;
    s.ChildRounding = 5.0f;
    s.FrameRounding = 4.0f;
    s.PopupRounding = 6.0f;
    s.ScrollbarRounding = 8.0f;
    s.GrabRounding = 4.0f;
    s.TabRounding = 4.0f;
    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);

    const ImVec4 accent = Rgb(66, 150, 250);
    const ImVec4 accent_hover = Rgb(96, 170, 255);
    const ImVec4 accent_dim = Rgb(40, 92, 160);
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = Rgb(226, 230, 236);
    c[ImGuiCol_TextDisabled] = Rgb(128, 135, 148);
    c[ImGuiCol_WindowBg] = Rgb(27, 29, 34);
    c[ImGuiCol_ChildBg] = Rgb(31, 33, 39);
    c[ImGuiCol_PopupBg] = Rgb(34, 37, 44, 0.98f);
    c[ImGuiCol_Border] = Rgb(52, 56, 66);
    c[ImGuiCol_BorderShadow] = Rgb(0, 0, 0, 0.0f);
    c[ImGuiCol_FrameBg] = Rgb(40, 43, 51);
    c[ImGuiCol_FrameBgHovered] = Rgb(50, 55, 66);
    c[ImGuiCol_FrameBgActive] = Rgb(58, 66, 80);
    c[ImGuiCol_TitleBg] = Rgb(22, 24, 28);
    c[ImGuiCol_TitleBgActive] = Rgb(30, 34, 42);
    c[ImGuiCol_TitleBgCollapsed] = Rgb(22, 24, 28);
    c[ImGuiCol_MenuBarBg] = Rgb(22, 24, 28);
    c[ImGuiCol_ScrollbarBg] = Rgb(27, 29, 34, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = Rgb(70, 76, 90);
    c[ImGuiCol_ScrollbarGrabHovered] = Rgb(90, 98, 114);
    c[ImGuiCol_ScrollbarGrabActive] = Rgb(110, 120, 140);
    c[ImGuiCol_CheckMark] = accent_hover;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accent_hover;
    c[ImGuiCol_Button] = Rgb(48, 53, 64);
    c[ImGuiCol_ButtonHovered] = accent_dim;
    c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_Header] = Rgb(44, 70, 110);
    c[ImGuiCol_HeaderHovered] = Rgb(52, 84, 132);
    c[ImGuiCol_HeaderActive] = accent_dim;
    c[ImGuiCol_Separator] = Rgb(52, 56, 66);
    c[ImGuiCol_SeparatorHovered] = accent;
    c[ImGuiCol_SeparatorActive] = accent_hover;
    c[ImGuiCol_ResizeGrip] = Rgb(66, 150, 250, 0.20f);
    c[ImGuiCol_ResizeGripHovered] = Rgb(66, 150, 250, 0.60f);
    c[ImGuiCol_ResizeGripActive] = Rgb(66, 150, 250, 0.90f);
    c[ImGuiCol_Tab] = Rgb(34, 37, 44);
    c[ImGuiCol_TabHovered] = accent_dim;
    c[ImGuiCol_TabSelected] = Rgb(48, 88, 150);
    c[ImGuiCol_TabDimmed] = Rgb(30, 32, 38);
    c[ImGuiCol_TabDimmedSelected] = Rgb(40, 64, 104);
    c[ImGuiCol_TextSelectedBg] = Rgb(66, 150, 250, 0.35f);
    c[ImGuiCol_DragDropTarget] = Rgb(255, 200, 60);
    c[ImGuiCol_NavCursor] = accent_hover;
    c[ImGuiCol_TableHeaderBg] = Rgb(36, 39, 46);
    c[ImGuiCol_TableBorderStrong] = Rgb(52, 56, 66);
    c[ImGuiCol_TableBorderLight] = Rgb(42, 45, 53);
    c[ImGuiCol_TableRowBgAlt] = Rgb(255, 255, 255, 0.03f);

    const std::filesystem::path font = font_file.empty() ? FindFont() : font_file;
    if (font.empty()) return false;
    ImGuiIO& io = ImGui::GetIO();
    ImFont* loaded = io.Fonts->AddFontFromFileTTF(font.string().c_str(), font_size);
    if (!loaded) return false;
    io.FontDefault = loaded;
    return true;
}

} // namespace aether::editor
