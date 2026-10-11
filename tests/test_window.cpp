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

AETHER_TEST(Window_TouchEventsFillThePointerSlots) {
    input::InputState state;
    const auto touch = [](u32 id, bool down, f32 x, f32 y) {
        WindowEvent e;
        e.type = WindowEventType::Touch, e.pointer = id, e.down = down, e.x = x, e.y = y;
        return e;
    };
    const auto moved = [](u32 id, f32 x, f32 y) {
        WindowEvent e;
        e.type = WindowEventType::TouchMove, e.pointer = id, e.x = x, e.y = y;
        return e;
    };
    // The mouse is pointer 0: it follows the cursor and the left button.
    WindowEvent cursor;
    cursor.type = WindowEventType::MouseMove, cursor.x = 40, cursor.y = 50, cursor.dx = 1, cursor.dy = 1;
    WindowEvent click;
    click.type = WindowEventType::Key, click.key = Key::MouseLeft, click.down = true;
    std::vector<WindowEvent> events = {cursor, click};
    ApplyWindowEvents(events, state);
    CHECK(state.GetPointer(0).active && state.GetPointer(0).down && state.GetPointer(0).x == 40 && state.GetPointer(0).y == 50);

    // Two fingers take slots 1 and 2; the first also holds the left mouse button.
    events = {touch(7, true, 10, 20), touch(9, true, 30, 40)};
    ApplyWindowEvents(events, state);
    CHECK(state.GetPointer(1).down && state.GetPointer(1).x == 10 && state.GetPointer(2).down && state.GetPointer(2).y == 40);
    CHECK(state.IsDown(Key::MouseLeft));
    events = {moved(7, 12, 22), moved(99, 1, 1)}; // a finger that never landed is ignored
    ApplyWindowEvents(events, state);
    CHECK(state.GetPointer(1).x == 12 && state.GetPointer(1).y == 22 && state.GetPointer(1).down);
    // Lifting the first frees its slot and lets go of the button; the next finger takes the lowest free slot.
    events = {touch(7, false, 12, 22)};
    ApplyWindowEvents(events, state);
    CHECK(!state.GetPointer(1).down && !state.GetPointer(1).active && !state.IsDown(Key::MouseLeft) && state.GetPointer(2).down);
    events = {touch(11, true, 70, 80)};
    ApplyWindowEvents(events, state);
    CHECK(state.GetPointer(1).down && state.GetPointer(1).x == 70);
    events = {touch(123, false, 0, 0)}; // a lift for a finger that is not down
    ApplyWindowEvents(events, state);
    CHECK(state.GetPointer(1).down && state.GetPointer(2).down);
    // Past the slots there is nowhere to put a finger.
    events.clear();
    for (u32 id = 200; id < 215; ++id) events.push_back(touch(id, true, 1, 1));
    ApplyWindowEvents(events, state);
    CHECK(state.TouchSlot(214, false) == -1);
    // Losing focus lets go of everything.
    WindowEvent lost;
    lost.type = WindowEventType::Focus, lost.down = false;
    events = {lost};
    ApplyWindowEvents(events, state);
    CHECK(!state.GetPointer(1).down && !state.GetPointer(0).down && state.TouchSlot(11, false) == -1);
}
