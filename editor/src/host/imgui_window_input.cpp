#include "host/imgui_window_input.h"

namespace aether::editor {

using input::Key;

ImGuiKey ToImGuiKey(Key key) {
    const auto k = static_cast<u16>(key);
    if (k >= static_cast<u16>(Key::A) && k <= static_cast<u16>(Key::Z)) {
        return static_cast<ImGuiKey>(ImGuiKey_A + (k - static_cast<u16>(Key::A)));
    }
    if (k >= static_cast<u16>(Key::Num0) && k <= static_cast<u16>(Key::Num9)) {
        return static_cast<ImGuiKey>(ImGuiKey_0 + (k - static_cast<u16>(Key::Num0)));
    }
    if (k >= static_cast<u16>(Key::F1) && k <= static_cast<u16>(Key::F12)) {
        return static_cast<ImGuiKey>(ImGuiKey_F1 + (k - static_cast<u16>(Key::F1)));
    }
    switch (key) {
        case Key::Space: return ImGuiKey_Space;
        case Key::Enter: return ImGuiKey_Enter;
        case Key::Escape: return ImGuiKey_Escape;
        case Key::Tab: return ImGuiKey_Tab;
        case Key::Backspace: return ImGuiKey_Backspace;
        case Key::Delete: return ImGuiKey_Delete;
        case Key::Insert: return ImGuiKey_Insert;
        case Key::Home: return ImGuiKey_Home;
        case Key::End: return ImGuiKey_End;
        case Key::PageUp: return ImGuiKey_PageUp;
        case Key::PageDown: return ImGuiKey_PageDown;
        case Key::LeftShift: return ImGuiKey_LeftShift;
        case Key::RightShift: return ImGuiKey_RightShift;
        case Key::LeftCtrl: return ImGuiKey_LeftCtrl;
        case Key::RightCtrl: return ImGuiKey_RightCtrl;
        case Key::LeftAlt: return ImGuiKey_LeftAlt;
        case Key::RightAlt: return ImGuiKey_RightAlt;
        case Key::Up: return ImGuiKey_UpArrow;
        case Key::Down: return ImGuiKey_DownArrow;
        case Key::Left: return ImGuiKey_LeftArrow;
        case Key::Right: return ImGuiKey_RightArrow;
        case Key::Minus: return ImGuiKey_Minus;
        case Key::Equals: return ImGuiKey_Equal;
        case Key::LeftBracket: return ImGuiKey_LeftBracket;
        case Key::RightBracket: return ImGuiKey_RightBracket;
        case Key::Semicolon: return ImGuiKey_Semicolon;
        case Key::Apostrophe: return ImGuiKey_Apostrophe;
        case Key::Comma: return ImGuiKey_Comma;
        case Key::Period: return ImGuiKey_Period;
        case Key::Slash: return ImGuiKey_Slash;
        case Key::Backslash: return ImGuiKey_Backslash;
        case Key::Grave: return ImGuiKey_GraveAccent;
        case Key::GamepadA: return ImGuiKey_GamepadFaceDown;
        case Key::GamepadB: return ImGuiKey_GamepadFaceRight;
        case Key::GamepadX: return ImGuiKey_GamepadFaceLeft;
        case Key::GamepadY: return ImGuiKey_GamepadFaceUp;
        case Key::GamepadDPadUp: return ImGuiKey_GamepadDpadUp;
        case Key::GamepadDPadDown: return ImGuiKey_GamepadDpadDown;
        case Key::GamepadDPadLeft: return ImGuiKey_GamepadDpadLeft;
        case Key::GamepadDPadRight: return ImGuiKey_GamepadDpadRight;
        case Key::GamepadStart: return ImGuiKey_GamepadStart;
        case Key::GamepadBack: return ImGuiKey_GamepadBack;
        case Key::GamepadLeftShoulder: return ImGuiKey_GamepadL1;
        case Key::GamepadRightShoulder: return ImGuiKey_GamepadR1;
        default: return ImGuiKey_None;
    }
}

namespace {

int MouseButton(Key key) {
    switch (key) {
        case Key::MouseLeft: return 0;
        case Key::MouseRight: return 1;
        case Key::MouseMiddle: return 2;
        case Key::MouseButton4: return 3;
        case Key::MouseButton5: return 4;
        default: return -1;
    }
}

// The modifier keys ImGui wants as ImGuiMod_* alongside the keys themselves.
void UpdateModifier(ImGuiIO& io, Key key, bool down, bool (&held)[6]) {
    static constexpr Key kKeys[6] = {Key::LeftShift, Key::RightShift, Key::LeftCtrl,
                                     Key::RightCtrl, Key::LeftAlt,    Key::RightAlt};
    for (int i = 0; i < 6; ++i) {
        if (kKeys[i] == key) held[i] = down;
    }
    io.AddKeyEvent(ImGuiMod_Shift, held[0] || held[1]);
    io.AddKeyEvent(ImGuiMod_Ctrl, held[2] || held[3]);
    io.AddKeyEvent(ImGuiMod_Alt, held[4] || held[5]);
}

} // namespace

void FeedImGuiEvents(std::span<const WindowEvent> events, ImGuiIO& io) {
    // ImGui keeps modifier state itself; reconstruct both sides from it.
    bool held[6] = {ImGui::IsKeyDown(ImGuiKey_LeftShift), ImGui::IsKeyDown(ImGuiKey_RightShift),
                    ImGui::IsKeyDown(ImGuiKey_LeftCtrl),  ImGui::IsKeyDown(ImGuiKey_RightCtrl),
                    ImGui::IsKeyDown(ImGuiKey_LeftAlt),   ImGui::IsKeyDown(ImGuiKey_RightAlt)};
    for (const WindowEvent& e : events) {
        switch (e.type) {
            case WindowEventType::Key: {
                if (const int button = MouseButton(e.key); button >= 0) {
                    io.AddMouseButtonEvent(button, e.down);
                    break;
                }
                const ImGuiKey key = ToImGuiKey(e.key);
                if (key == ImGuiKey_None) break;
                io.AddKeyEvent(key, e.down);
                UpdateModifier(io, e.key, e.down, held);
                break;
            }
            case WindowEventType::Char:
                io.AddInputCharacter(e.codepoint);
                break;
            case WindowEventType::MouseMove:
                io.AddMousePosEvent(e.x, e.y);
                break;
            case WindowEventType::Scroll:
                io.AddMouseWheelEvent(e.dx, e.dy);
                break;
            case WindowEventType::Focus:
                io.AddFocusEvent(e.down);
                break;
            case WindowEventType::Resize:
                io.DisplaySize = ImVec2(e.x, e.y);
                break;
            case WindowEventType::Axis:
            case WindowEventType::Close:
                break;
        }
    }
}

} // namespace aether::editor
