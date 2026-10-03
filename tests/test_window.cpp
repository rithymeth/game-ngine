#include "aether/platform/window.h"
#include "test_framework.h"

#include <cstdlib>
#include <string>

// Phase 24 step 3: the portable window layer - Win32 and GLFW key codes
// to engine keys, window events to input state, and (where there is a
// display: a desktop, or Xvfb in CI) a real window created, pumped, fed
// events and closed.

using namespace aether;
using input::Key;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

AETHER_TEST(Window_KeyCodesMapToEngineKeys) {
    // Win32 virtual keys.
    CHECK(KeyFromVirtualKey('A') == Key::A && KeyFromVirtualKey('Z') == Key::Z && KeyFromVirtualKey('7') == Key::Num7);
    CHECK(KeyFromVirtualKey(0x70) == Key::F1 && KeyFromVirtualKey(0x7B) == Key::F12);
    CHECK(KeyFromVirtualKey(0x20) == Key::Space && KeyFromVirtualKey(0x1B) == Key::Escape && KeyFromVirtualKey(0x0D) == Key::Enter);
    CHECK(KeyFromVirtualKey(0xA1) == Key::RightShift && KeyFromVirtualKey(0x10) == Key::LeftShift && KeyFromVirtualKey(0xA5) == Key::RightAlt);
    CHECK(KeyFromVirtualKey(0x25) == Key::Left && KeyFromVirtualKey(0x28) == Key::Down && KeyFromVirtualKey(0xC0) == Key::Grave);
    CHECK(KeyFromVirtualKey(0x01) == Key::MouseLeft && KeyFromVirtualKey(0x06) == Key::MouseButton5);
    CHECK(KeyFromVirtualKey(0xFF) == Key::None && KeyFromVirtualKey(0x7C) == Key::None); // F13: no name
    // GLFW keys.
    CHECK(KeyFromGlfwKey(65) == Key::A && KeyFromGlfwKey(90) == Key::Z && KeyFromGlfwKey(48) == Key::Num0);
    CHECK(KeyFromGlfwKey(290) == Key::F1 && KeyFromGlfwKey(301) == Key::F12 && KeyFromGlfwKey(302) == Key::None);
    CHECK(KeyFromGlfwKey(256) == Key::Escape && KeyFromGlfwKey(265) == Key::Up && KeyFromGlfwKey(96) == Key::Grave);
    CHECK(KeyFromGlfwKey(340) == Key::LeftShift && KeyFromGlfwKey(346) == Key::RightAlt && KeyFromGlfwKey(-1) == Key::None);
    // Mouse and gamepad.
    CHECK(KeyFromGlfwMouseButton(0) == Key::MouseLeft && KeyFromGlfwMouseButton(2) == Key::MouseMiddle && KeyFromGlfwMouseButton(7) == Key::None);
    CHECK(KeyFromGlfwGamepadButton(0) == Key::GamepadA && KeyFromGlfwGamepadButton(8) == Key::None && KeyFromGlfwGamepadButton(14) == Key::GamepadDPadLeft);
    CHECK(KeyFromGlfwGamepadAxis(1) == Key::GamepadLeftStickY && KeyFromGlfwGamepadAxis(5) == Key::GamepadRightTrigger && KeyFromGlfwGamepadAxis(6) == Key::None);
    // Every letter and digit, both ways.
    for (u32 i = 0; i < 26; ++i) CHECK(KeyFromVirtualKey('A' + i) == KeyFromGlfwKey(static_cast<int>(65 + i)));
    for (u32 i = 0; i < 10; ++i) CHECK(KeyFromVirtualKey('0' + i) == KeyFromGlfwKey(static_cast<int>(48 + i)));
}

AETHER_TEST(Window_EventsFeedInputState) {
    input::InputState state;
    std::vector<WindowEvent> events;
    const auto key = [](Key k, bool down) {
        WindowEvent e;
        e.type = WindowEventType::Key, e.key = k, e.down = down;
        return e;
    };
    WindowEvent move;
    move.type = WindowEventType::MouseMove, move.dx = 3, move.dy = -2;
    WindowEvent wheel;
    wheel.type = WindowEventType::Scroll, wheel.dy = 1;
    WindowEvent stick;
    stick.type = WindowEventType::Axis, stick.key = Key::GamepadLeftStickX, stick.x = 0.75f;
    WindowEvent typed;
    typed.type = WindowEventType::Char, typed.codepoint = 'w';
    events = {key(Key::W, true), key(Key::MouseLeft, true), move, move, wheel, stick, typed, key(Key::None, true)};
    ApplyWindowEvents(events, state);
    CHECK(state.IsDown(Key::W) && state.IsDown(Key::MouseLeft) && !state.IsDown(Key::None));
    CHECK(state.Value(Key::MouseX) == 6 && state.Value(Key::MouseY) == -4 && state.Value(Key::MouseWheel) == 1);
    CHECK(state.Value(Key::GamepadLeftStickX) == 0.75f);
    events = {key(Key::W, false)};
    ApplyWindowEvents(events, state);
    CHECK(!state.IsDown(Key::W) && state.IsDown(Key::MouseLeft));
    // Losing focus lets go of everything.
    WindowEvent lost;
    lost.type = WindowEventType::Focus, lost.down = false;
    events = {lost};
    ApplyWindowEvents(events, state);
    CHECK(!state.IsDown(Key::MouseLeft) && state.Value(Key::GamepadLeftStickX) == 0);
}

#if defined(AETHER_HAS_WINDOW) && !defined(_WIN32)
AETHER_TEST(Window_GlfwWindowUnderADisplay) {
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY")) {
        std::printf("    (no display: run under xvfb-run to test a real window)\n");
        return;
    }
    WindowDesc desc;
    desc.title = "Aether test window";
    desc.width = 320, desc.height = 200;
    desc.visible = false;
    Window window(desc);
    CHECK(window.IsValid() && std::string(window.Backend()).rfind("glfw-", 0) == 0);
    CHECK(window.PlatformWindow() != nullptr && window.NativeHandle() != nullptr && window.NativeDisplay() != nullptr);
    CHECK(window.Width() == 320 && window.Height() == 200);
    for (int i = 0; i < 3; ++i) CHECK(window.PumpMessages());
    window.SetTitle("Renamed");
    // A second window shares GLFW.
    {
        Window second(desc);
        CHECK(second.IsValid() && second.PumpMessages());
    }
    CHECK(window.PumpMessages());
    window.TakeEvents();
    window.RequestClose();
    CHECK(!window.PumpMessages());
    const std::vector<WindowEvent> events = window.TakeEvents();
    CHECK(!events.empty() && events.back().type == WindowEventType::Close);
    CHECK(window.TakeEvents().empty());
}
#endif
