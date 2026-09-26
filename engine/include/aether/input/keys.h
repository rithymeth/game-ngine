#pragma once

#include "aether/core/base.h"
#include "aether/reflection/reflection.h"

#include <array>

namespace aether::input {

// Every input the engine knows, across devices (Phase 10). Buttons have a
// value of 0 or 1; axes (mouse movement and wheel, sticks, triggers) are
// analog. Saved by name ("W", "GamepadA", "MouseX"), so the numbering may
// change.
enum class Key : u16 {
    None,
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,
    Num0,
    Num1,
    Num2,
    Num3,
    Num4,
    Num5,
    Num6,
    Num7,
    Num8,
    Num9,
    Space,
    Enter,
    Escape,
    Tab,
    Backspace,
    Delete,
    Insert,
    Home,
    End,
    PageUp,
    PageDown,
    LeftShift,
    RightShift,
    LeftCtrl,
    RightCtrl,
    LeftAlt,
    RightAlt,
    Up,
    Down,
    Left,
    Right,
    Minus,
    Equals,
    LeftBracket,
    RightBracket,
    Semicolon,
    Apostrophe,
    Comma,
    Period,
    Slash,
    Backslash,
    Grave,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    MouseLeft,
    MouseRight,
    MouseMiddle,
    MouseButton4,
    MouseButton5,
    MouseX,
    MouseY,
    MouseWheel,
    GamepadA,
    GamepadB,
    GamepadX,
    GamepadY,
    GamepadLeftShoulder,
    GamepadRightShoulder,
    GamepadBack,
    GamepadStart,
    GamepadLeftThumb,
    GamepadRightThumb,
    GamepadDPadUp,
    GamepadDPadDown,
    GamepadDPadLeft,
    GamepadDPadRight,
    GamepadLeftStickX,
    GamepadLeftStickY,
    GamepadRightStickX,
    GamepadRightStickY,
    GamepadLeftTrigger,
    GamepadRightTrigger,
    Count
};

inline constexpr usize kKeyCount = static_cast<usize>(Key::Count);

// Mouse movement/wheel (per-frame deltas), sticks (-1..1) and triggers (0..1).
constexpr bool IsAxis(Key key) {
    return key == Key::MouseX || key == Key::MouseY || key == Key::MouseWheel ||
           (key >= Key::GamepadLeftStickX && key <= Key::GamepadRightTrigger);
}
// Values that are per-frame deltas, reset by InputState::EndFrame.
constexpr bool IsDelta(Key key) { return key == Key::MouseX || key == Key::MouseY || key == Key::MouseWheel; }

const char* KeyName(Key key);
Key KeyFromName(std::string_view name); // Key::None if unknown

// The raw device layer: what each key is doing right now, fed by the
// platform (window messages, XInput) or by tests with synthetic events.
class InputState {
public:
    void SetButton(Key key, bool down) { values_[Index(key)] = down ? 1.0f : 0.0f; }
    void SetAxis(Key key, f32 value) { values_[Index(key)] = value; }
    // Mouse movement and wheel accumulate over the frame.
    void AddMouseDelta(f32 dx, f32 dy) {
        values_[Index(Key::MouseX)] += dx;
        values_[Index(Key::MouseY)] += dy;
    }
    void AddWheel(f32 delta) { values_[Index(Key::MouseWheel)] += delta; }

    f32 Value(Key key) const { return values_[Index(key)]; }
    bool IsDown(Key key) const { return values_[Index(key)] >= 0.5f; }

    // After the frame's input has been processed: resets per-frame deltas.
    void EndFrame() {
        for (Key key : {Key::MouseX, Key::MouseY, Key::MouseWheel}) {
            values_[Index(key)] = 0.0f;
        }
    }
    // Everything released and centred (e.g. the window lost focus).
    void Clear() { values_.fill(0.0f); }

private:
    static usize Index(Key key) {
        const usize i = static_cast<usize>(key);
        return i < kKeyCount ? i : 0;
    }
    std::array<f32, kKeyCount> values_{};
};

} // namespace aether::input

AETHER_ENUM(aether::input::Key, 1,
    AETHER_ENUM_VALUE(None),
    AETHER_ENUM_VALUE(A),
    AETHER_ENUM_VALUE(B),
    AETHER_ENUM_VALUE(C),
    AETHER_ENUM_VALUE(D),
    AETHER_ENUM_VALUE(E),
    AETHER_ENUM_VALUE(F),
    AETHER_ENUM_VALUE(G),
    AETHER_ENUM_VALUE(H),
    AETHER_ENUM_VALUE(I),
    AETHER_ENUM_VALUE(J),
    AETHER_ENUM_VALUE(K),
    AETHER_ENUM_VALUE(L),
    AETHER_ENUM_VALUE(M),
    AETHER_ENUM_VALUE(N),
    AETHER_ENUM_VALUE(O),
    AETHER_ENUM_VALUE(P),
    AETHER_ENUM_VALUE(Q),
    AETHER_ENUM_VALUE(R),
    AETHER_ENUM_VALUE(S),
    AETHER_ENUM_VALUE(T),
    AETHER_ENUM_VALUE(U),
    AETHER_ENUM_VALUE(V),
    AETHER_ENUM_VALUE(W),
    AETHER_ENUM_VALUE(X),
    AETHER_ENUM_VALUE(Y),
    AETHER_ENUM_VALUE(Z),
    AETHER_ENUM_VALUE(Num0),
    AETHER_ENUM_VALUE(Num1),
    AETHER_ENUM_VALUE(Num2),
    AETHER_ENUM_VALUE(Num3),
    AETHER_ENUM_VALUE(Num4),
    AETHER_ENUM_VALUE(Num5),
    AETHER_ENUM_VALUE(Num6),
    AETHER_ENUM_VALUE(Num7),
    AETHER_ENUM_VALUE(Num8),
    AETHER_ENUM_VALUE(Num9),
    AETHER_ENUM_VALUE(Space),
    AETHER_ENUM_VALUE(Enter),
    AETHER_ENUM_VALUE(Escape),
    AETHER_ENUM_VALUE(Tab),
    AETHER_ENUM_VALUE(Backspace),
    AETHER_ENUM_VALUE(Delete),
    AETHER_ENUM_VALUE(Insert),
    AETHER_ENUM_VALUE(Home),
    AETHER_ENUM_VALUE(End),
    AETHER_ENUM_VALUE(PageUp),
    AETHER_ENUM_VALUE(PageDown),
    AETHER_ENUM_VALUE(LeftShift),
    AETHER_ENUM_VALUE(RightShift),
    AETHER_ENUM_VALUE(LeftCtrl),
    AETHER_ENUM_VALUE(RightCtrl),
    AETHER_ENUM_VALUE(LeftAlt),
    AETHER_ENUM_VALUE(RightAlt),
    AETHER_ENUM_VALUE(Up),
    AETHER_ENUM_VALUE(Down),
    AETHER_ENUM_VALUE(Left),
    AETHER_ENUM_VALUE(Right),
    AETHER_ENUM_VALUE(Minus),
    AETHER_ENUM_VALUE(Equals),
    AETHER_ENUM_VALUE(LeftBracket),
    AETHER_ENUM_VALUE(RightBracket),
    AETHER_ENUM_VALUE(Semicolon),
    AETHER_ENUM_VALUE(Apostrophe),
    AETHER_ENUM_VALUE(Comma),
    AETHER_ENUM_VALUE(Period),
    AETHER_ENUM_VALUE(Slash),
    AETHER_ENUM_VALUE(Backslash),
    AETHER_ENUM_VALUE(Grave),
    AETHER_ENUM_VALUE(F1),
    AETHER_ENUM_VALUE(F2),
    AETHER_ENUM_VALUE(F3),
    AETHER_ENUM_VALUE(F4),
    AETHER_ENUM_VALUE(F5),
    AETHER_ENUM_VALUE(F6),
    AETHER_ENUM_VALUE(F7),
    AETHER_ENUM_VALUE(F8),
    AETHER_ENUM_VALUE(F9),
    AETHER_ENUM_VALUE(F10),
    AETHER_ENUM_VALUE(F11),
    AETHER_ENUM_VALUE(F12),
    AETHER_ENUM_VALUE(MouseLeft),
    AETHER_ENUM_VALUE(MouseRight),
    AETHER_ENUM_VALUE(MouseMiddle),
    AETHER_ENUM_VALUE(MouseButton4),
    AETHER_ENUM_VALUE(MouseButton5),
    AETHER_ENUM_VALUE(MouseX),
    AETHER_ENUM_VALUE(MouseY),
    AETHER_ENUM_VALUE(MouseWheel),
    AETHER_ENUM_VALUE(GamepadA),
    AETHER_ENUM_VALUE(GamepadB),
    AETHER_ENUM_VALUE(GamepadX),
    AETHER_ENUM_VALUE(GamepadY),
    AETHER_ENUM_VALUE(GamepadLeftShoulder),
    AETHER_ENUM_VALUE(GamepadRightShoulder),
    AETHER_ENUM_VALUE(GamepadBack),
    AETHER_ENUM_VALUE(GamepadStart),
    AETHER_ENUM_VALUE(GamepadLeftThumb),
    AETHER_ENUM_VALUE(GamepadRightThumb),
    AETHER_ENUM_VALUE(GamepadDPadUp),
    AETHER_ENUM_VALUE(GamepadDPadDown),
    AETHER_ENUM_VALUE(GamepadDPadLeft),
    AETHER_ENUM_VALUE(GamepadDPadRight),
    AETHER_ENUM_VALUE(GamepadLeftStickX),
    AETHER_ENUM_VALUE(GamepadLeftStickY),
    AETHER_ENUM_VALUE(GamepadRightStickX),
    AETHER_ENUM_VALUE(GamepadRightStickY),
    AETHER_ENUM_VALUE(GamepadLeftTrigger),
    AETHER_ENUM_VALUE(GamepadRightTrigger)
)
