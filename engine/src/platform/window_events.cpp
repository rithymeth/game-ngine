// Portable parts of the window layer (Phase 24 step 3): platform key codes
// to engine keys, and window events to input state. No OS headers: the
// Win32 and GLFW codes are their documented numeric values.
#include "aether/platform/window.h"

#include <iterator>

namespace aether {

using input::Key;

namespace {
Key Offset(Key first, u32 by) { return static_cast<Key>(static_cast<u16>(first) + by); }
} // namespace

Key KeyFromVirtualKey(u32 vk) {
    if (vk >= 0x41 && vk <= 0x5A) return Offset(Key::A, vk - 0x41);   // 'A'..'Z'
    if (vk >= 0x30 && vk <= 0x39) return Offset(Key::Num0, vk - 0x30); // '0'..'9'
    if (vk >= 0x70 && vk <= 0x7B) return Offset(Key::F1, vk - 0x70);   // VK_F1..VK_F12
    switch (vk) {
    case 0x01: return Key::MouseLeft;    // VK_LBUTTON
    case 0x02: return Key::MouseRight;   // VK_RBUTTON
    case 0x04: return Key::MouseMiddle;  // VK_MBUTTON
    case 0x05: return Key::MouseButton4; // VK_XBUTTON1
    case 0x06: return Key::MouseButton5; // VK_XBUTTON2
    case 0x08: return Key::Backspace;
    case 0x09: return Key::Tab;
    case 0x0D: return Key::Enter;
    case 0x10: return Key::LeftShift; // VK_SHIFT, when the side is unknown
    case 0x11: return Key::LeftCtrl;  // VK_CONTROL
    case 0x12: return Key::LeftAlt;   // VK_MENU
    case 0x1B: return Key::Escape;
    case 0x20: return Key::Space;
    case 0x21: return Key::PageUp;
    case 0x22: return Key::PageDown;
    case 0x23: return Key::End;
    case 0x24: return Key::Home;
    case 0x25: return Key::Left;
    case 0x26: return Key::Up;
    case 0x27: return Key::Right;
    case 0x28: return Key::Down;
    case 0x2D: return Key::Insert;
    case 0x2E: return Key::Delete;
    case 0xA0: return Key::LeftShift;
    case 0xA1: return Key::RightShift;
    case 0xA2: return Key::LeftCtrl;
    case 0xA3: return Key::RightCtrl;
    case 0xA4: return Key::LeftAlt;
    case 0xA5: return Key::RightAlt;
    case 0xBA: return Key::Semicolon;    // VK_OEM_1
    case 0xBB: return Key::Equals;       // VK_OEM_PLUS
    case 0xBC: return Key::Comma;        // VK_OEM_COMMA
    case 0xBD: return Key::Minus;        // VK_OEM_MINUS
    case 0xBE: return Key::Period;       // VK_OEM_PERIOD
    case 0xBF: return Key::Slash;        // VK_OEM_2
    case 0xC0: return Key::Grave;        // VK_OEM_3
    case 0xDB: return Key::LeftBracket;  // VK_OEM_4
    case 0xDC: return Key::Backslash;    // VK_OEM_5
    case 0xDD: return Key::RightBracket; // VK_OEM_6
    case 0xDE: return Key::Apostrophe;   // VK_OEM_7
    }
    return Key::None;
}

Key KeyFromGlfwKey(int key) {
    if (key >= 65 && key <= 90) return Offset(Key::A, static_cast<u32>(key - 65));     // GLFW_KEY_A..Z
    if (key >= 48 && key <= 57) return Offset(Key::Num0, static_cast<u32>(key - 48));  // GLFW_KEY_0..9
    if (key >= 290 && key <= 301) return Offset(Key::F1, static_cast<u32>(key - 290)); // GLFW_KEY_F1..F12
    switch (key) {
    case 32: return Key::Space;
    case 39: return Key::Apostrophe;
    case 44: return Key::Comma;
    case 45: return Key::Minus;
    case 46: return Key::Period;
    case 47: return Key::Slash;
    case 59: return Key::Semicolon;
    case 61: return Key::Equals;
    case 91: return Key::LeftBracket;
    case 92: return Key::Backslash;
    case 93: return Key::RightBracket;
    case 96: return Key::Grave;
    case 256: return Key::Escape;
    case 257: return Key::Enter;
    case 258: return Key::Tab;
    case 259: return Key::Backspace;
    case 260: return Key::Insert;
    case 261: return Key::Delete;
    case 262: return Key::Right;
    case 263: return Key::Left;
    case 264: return Key::Down;
    case 265: return Key::Up;
    case 266: return Key::PageUp;
    case 267: return Key::PageDown;
    case 268: return Key::Home;
    case 269: return Key::End;
    case 340: return Key::LeftShift;
    case 341: return Key::LeftCtrl;
    case 342: return Key::LeftAlt;
    case 344: return Key::RightShift;
    case 345: return Key::RightCtrl;
    case 346: return Key::RightAlt;
    }
    return Key::None;
}

Key KeyFromGlfwMouseButton(int button) {
    switch (button) {
    case 0: return Key::MouseLeft;
    case 1: return Key::MouseRight;
    case 2: return Key::MouseMiddle;
    case 3: return Key::MouseButton4;
    case 4: return Key::MouseButton5;
    }
    return Key::None;
}

Key KeyFromGlfwGamepadButton(int button) {
    static const Key kButtons[] = {Key::GamepadA,         Key::GamepadB,           Key::GamepadX,          Key::GamepadY,
                                   Key::GamepadLeftShoulder, Key::GamepadRightShoulder, Key::GamepadBack,     Key::GamepadStart,
                                   Key::None /* guide */, Key::GamepadLeftThumb,   Key::GamepadRightThumb, Key::GamepadDPadUp,
                                   Key::GamepadDPadRight, Key::GamepadDPadDown,    Key::GamepadDPadLeft};
    return button >= 0 && button < static_cast<int>(std::size(kButtons)) ? kButtons[button] : Key::None;
}

Key KeyFromGlfwGamepadAxis(int axis) {
    static const Key kAxes[] = {Key::GamepadLeftStickX, Key::GamepadLeftStickY, Key::GamepadRightStickX,
                                Key::GamepadRightStickY, Key::GamepadLeftTrigger, Key::GamepadRightTrigger};
    return axis >= 0 && axis < static_cast<int>(std::size(kAxes)) ? kAxes[axis] : Key::None;
}

void ApplyWindowEvents(std::span<const WindowEvent> events, input::InputState& state) {
    for (const WindowEvent& e : events) {
        switch (e.type) {
        case WindowEventType::Key:
            if (e.key != Key::None) state.SetButton(e.key, e.down);
            break;
        case WindowEventType::MouseMove: state.AddMouseDelta(e.dx, e.dy); break;
        case WindowEventType::Scroll: state.AddWheel(e.dy); break;
        case WindowEventType::Axis:
            if (e.key != Key::None) state.SetAxis(e.key, e.x);
            break;
        case WindowEventType::Focus:
            if (!e.down) state.Clear(); // nothing stays held while another window has the keyboard
            break;
        default: break;
        }
    }
}

std::vector<WindowEvent> Window::TakeEvents() {
    std::vector<WindowEvent> out;
    out.swap(events_);
    return out;
}

} // namespace aether
