#pragma once

#include "aether/input/actions.h"
#include "aether/ui/viewport.h"

#include <map>
#include <memory>
#include <string>

namespace aether::ui {

// Input for a viewport's widgets (Phase 18 step 2, §18.2): pointers (mouse
// and touch) with hover and capture, the wheel, focus with keyboard and
// gamepad navigation, accept and cancel, text editing, popups and tooltips.
// Every call says whether the UI used the input, so gameplay can skip what
// the UI took. Pointer positions are in pixels.
class UIInputRouter {
public:
    UIInputRouter(Viewport& viewport, const Font& font) : viewport_(viewport), font_(font) {}

    bool PointerMove(Vec2 pixel, u32 pointer = 0);
    bool PointerDown(Vec2 pixel, int button = 0, u32 pointer = 0);
    bool PointerUp(Vec2 pixel, int button = 0, u32 pointer = 0);
    bool Wheel(Vec2 pixel, f32 delta); // + is away from the user (scroll up)
    bool Navigate(NavDirection direction);
    bool Accept();
    bool Cancel(); // closes a popup, else asks the focused widget and its parents
    bool Text(std::string_view utf8);
    bool Key(EditKey key, bool shift = false);
    // Tooltips (time spent hovering).
    void Tick(f32 dt);

    Widget* Hovered() const { return hovered_; }
    Widget* Focused() const { return focused_; }
    Widget* Captured(u32 pointer = 0) const;
    void SetFocus(Widget* widget); // scrolls it into view in its scroll boxes
    void ClearFocus() { SetFocus(nullptr); }
    // Where focus would go from `from` (tests and the designer's preview).
    Widget* NavigationTarget(Widget* from, NavDirection direction) const;

    // A popup (a dropdown's list): `content` sits just below `below` (above it
    // if there's no room), over everything, until closed, a click lands
    // outside it, or Cancel. Focus stays inside it while it's open.
    Widget* OpenPopup(std::unique_ptr<Widget> content, const Rect& below, Widget* owner);
    void ClosePopup(); // deferred to the end of the event when called from a handler
    bool PopupOpen() const { return popup_ != nullptr; }
    Widget* Popup() const { return popup_; }

    f32 tooltip_delay = 0.5f;
    bool TooltipShown() const { return tooltip_ != nullptr; }
    const std::string& TooltipText() const { return tooltip_text_; }

    Viewport& GetViewport() { return viewport_; }
    const Font& GetFont() const { return font_; }
    static constexpr i32 kPopupZ = 1 << 29, kTooltipZ = 1 << 30;

private:
    Vec2 ToLayout(Vec2 pixel) const;
    bool Alive(const Widget* w) const;
    void Sanitize(); // forget widgets that are gone
    static Widget* InteractiveOf(Widget* w);
    static bool Within(const Widget* w, const Widget* root);
    std::vector<Widget*> Candidates() const; // focusable widgets in scope, in tree order
    void SetHovered(Widget* w);
    void HideTooltip();
    void Finish(); // deferred work after a handler
    void Relayout() { viewport_.Layout(font_); }

    Viewport& viewport_;
    const Font& font_;
    Widget* hovered_ = nullptr;
    Widget* focused_ = nullptr;
    std::map<u32, Widget*> captured_;
    Widget* popup_ = nullptr;
    Widget* popup_owner_ = nullptr;
    bool close_popup_ = false;
    int dispatching_ = 0;
    // Tooltips.
    Widget* tooltip_source_ = nullptr;
    Vec2 pointer_;
    f32 hover_time_ = 0.0f;
    Widget* tooltip_ = nullptr;
    std::string tooltip_text_;
};

// Turns the device state (Phase 10) into UI input each frame: arrows, the
// d-pad and the left stick navigate (repeating while held); Tab and
// Shift+Tab step through; Enter, Space and A accept; Escape and B cancel.
// While a text box has focus, Space types, Left/Right and the edit keys edit.
class UIInputBridge {
public:
    explicit UIInputBridge(UIInputRouter& router) : router_(router) {}
    void Update(const input::InputState& state, f32 dt);
    f32 repeat_delay = 0.4f, repeat_interval = 0.08f;
    f32 stick_threshold = 0.5f;

    // A context that blocks gameplay's input (Phase 10) while a menu is up.
    static constexpr const char* kModalContext = "UI.Modal";
    static void SetModal(input::InputSystem& system, bool modal, i32 priority = 1000);

private:
    enum Pad : u8 { Up, Down, Left, Right, Tab, Accept, Cancel, Backspace, Delete, EditLeft, EditRight, Home, End, SelectAll, kCount };
    bool Fire(Pad pad, bool down, f32 dt, bool repeats);
    bool held_[kCount] = {};
    f32 timer_[kCount] = {};
    UIInputRouter& router_;
};

} // namespace aether::ui
