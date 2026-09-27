#pragma once

#include "aether/ui/widget.h"

#include <functional>
#include <string>
#include <vector>

namespace aether::ui {

// Interactive widgets (Phase 18 step 2, docs/design/PHASE_SPECS.md §18.2).
// They paint with a ControlStyle until themes arrive (step 3).

struct ControlStyle {
    Brush normal = Brush::Solid({0.22f, 0.24f, 0.28f, 1});
    Brush hovered = Brush::Solid({0.30f, 0.33f, 0.38f, 1});
    Brush pressed = Brush::Solid({0.16f, 0.18f, 0.21f, 1});
    Brush disabled = Brush::Solid({0.18f, 0.18f, 0.18f, 0.6f});
    Brush accent = Brush::Solid({0.25f, 0.55f, 0.95f, 1}); // fills, checks, thumbs, selections
    Brush focus = Brush::Solid({1.0f, 0.8f, 0.2f, 1});     // the focus outline
    f32 focus_width = 2.0f;
    Color text{0.95f, 0.95f, 0.95f, 1};
    Color hint{0.6f, 0.6f, 0.6f, 1};
    f32 text_size = 20.0f;
};
const ControlStyle& DefaultStyle();

// Common to controls: the style, and the state brush.
class Control : public Widget {
public:
    ControlStyle style = DefaultStyle();
    bool Interactive() const override { return true; }
    bool Focusable() const override { return focusable; }
    bool focusable = true;

protected:
    bool HitsSelf() const override { return true; }
    const Brush& StateBrush(bool pressed) const;
    void PaintFocus(DrawList& list, const PaintContext& ctx) const; // an outline while focused
};

// Clicked when pressed and released inside, or accepted while focused. It
// can hold a child (a Text, an Image, a box of both) inside `padding`.
class Button : public Control {
public:
    const char* TypeName() const override { return "Button"; }
    i32 MaxChildren() const override { return 1; }
    Margin padding{16, 8, 16, 8};
    std::function<void()> on_clicked, on_pressed, on_released;
    std::function<void(bool)> on_hovered;
    bool IsPressed() const { return pressed_; }

    bool OnPointerDown(const PointerEvent&, UIInputRouter&) override;
    void OnPointerUp(const PointerEvent&, bool inside, UIInputRouter&) override;
    bool OnAccept(UIInputRouter&) override;
    void OnHover(bool hovered) override;
    void Click(); // what a click does (tests and scripts)

protected:
    Vec2 ComputeDesired(const LayoutContext& ctx) override;
    void ArrangeChildren(const LayoutContext& ctx) override;
    void PaintSelf(DrawList& list, const PaintContext& ctx) const override;
    void PaintOver(DrawList& list, const PaintContext& ctx) const override { PaintFocus(list, ctx); }

private:
    bool pressed_ = false;
};

// A check box.
class Toggle : public Control {
public:
    const char* TypeName() const override { return "Toggle"; }
    bool checked = false;
    f32 box_size = 24.0f;
    std::function<void(bool)> on_changed;
    void SetChecked(bool value); // calls on_changed when it changes

    bool OnPointerDown(const PointerEvent&, UIInputRouter&) override { return true; }
    void OnPointerUp(const PointerEvent&, bool inside, UIInputRouter&) override;
    bool OnAccept(UIInputRouter&) override;

protected:
    Vec2 ComputeDesired(const LayoutContext&) override { return {box_size, box_size}; }
    void PaintSelf(DrawList& list, const PaintContext& ctx) const override;
    void PaintOver(DrawList& list, const PaintContext& ctx) const override { PaintFocus(list, ctx); }
};

// A value between min and max, dragged or stepped with left/right.
class Slider : public Control {
public:
    const char* TypeName() const override { return "Slider"; }
    f32 value = 0.0f, min = 0.0f, max = 1.0f;
    f32 step = 0.0f;      // snap (0: continuous)
    f32 nav_step = 0.0f;  // left/right (0: a twentieth of the range, or `step`)
    std::function<void(f32)> on_changed;
    void SetValue(f32 v); // clamped and snapped; on_changed when it changes

    bool OnPointerDown(const PointerEvent& e, UIInputRouter&) override;
    void OnPointerMove(const PointerEvent& e, UIInputRouter&) override;
    bool OnNavigate(NavDirection d, UIInputRouter&) override;

protected:
    Vec2 ComputeDesired(const LayoutContext&) override { return {200.0f, 24.0f}; }
    void PaintSelf(DrawList& list, const PaintContext& ctx) const override;
    void PaintOver(DrawList& list, const PaintContext& ctx) const override { PaintFocus(list, ctx); }

private:
    void FromPointer(f32 x);
};

// A bar filled to `percent` (0..1).
class ProgressBar : public Widget {
public:
    const char* TypeName() const override { return "ProgressBar"; }
    enum class Fill : u8 { LeftToRight, RightToLeft, BottomToTop, TopToBottom };
    f32 percent = 0.0f;
    Fill fill = Fill::LeftToRight;
    Brush background = Brush::Solid({0.15f, 0.15f, 0.17f, 1});
    Brush bar = DefaultStyle().accent;

protected:
    Vec2 ComputeDesired(const LayoutContext&) override { return {200.0f, 16.0f}; }
    void PaintSelf(DrawList& list, const PaintContext& ctx) const override;
};

// One line of editable text: typing inserts at the cursor (replacing a
// selection); the edit keys move, select (with shift) and delete; Enter
// commits. Longer text scrolls to keep the cursor in view.
class TextInput : public Control {
public:
    const char* TypeName() const override { return "TextInput"; }
    std::string text, hint;
    usize max_length = 0; // in code points; 0 = no limit
    bool password = false;
    Margin padding{8, 6, 8, 6};
    std::function<void(const std::string&)> on_changed, on_committed;
    bool WantsText() const override { return true; }

    usize Cursor() const { return cursor_; }         // a byte offset, on a code point boundary
    usize SelectionStart() const { return std::min(anchor_, cursor_); }
    usize SelectionEnd() const { return std::max(anchor_, cursor_); }
    bool HasSelection() const { return anchor_ != cursor_; }
    void SetText(const std::string& t); // cursor to the end, no selection
    void SetCursor(usize byte, bool extend_selection = false);
    std::string Shown() const; // what's drawn (dots for passwords)

    bool OnPointerDown(const PointerEvent& e, UIInputRouter&) override;
    void OnPointerMove(const PointerEvent& e, UIInputRouter&) override;
    bool OnText(std::string_view typed, UIInputRouter&) override;
    bool OnKey(EditKey key, bool shift, UIInputRouter&) override;
    bool OnAccept(UIInputRouter&) override;
    bool OnNavigate(NavDirection d, UIInputRouter&) override;

protected:
    Vec2 ComputeDesired(const LayoutContext& ctx) override;
    void ArrangeChildren(const LayoutContext& ctx) override;
    void PaintSelf(DrawList& list, const PaintContext& ctx) const override;
    void PaintOver(DrawList& list, const PaintContext& ctx) const override { PaintFocus(list, ctx); }

private:
    f32 XOf(usize byte) const;  // in the shown text, from its start
    usize ByteAt(f32 x) const;  // the nearest boundary to a point
    void DeleteSelection();
    void Changed();
    usize cursor_ = 0, anchor_ = 0;
    f32 scroll_ = 0.0f;
    const Font* font_ = nullptr;
};

// A choice from a list, opened as a popup (click or accept); in it, arrows
// move, accept picks, cancel closes.
class Dropdown : public Control {
public:
    const char* TypeName() const override { return "Dropdown"; }
    std::vector<std::string> options;
    i32 selected = -1;
    std::function<void(i32)> on_changed;
    void Select(i32 index); // on_changed when it changes
    bool IsOpen() const { return open_; }

    bool OnPointerDown(const PointerEvent&, UIInputRouter&) override { return true; }
    void OnPointerUp(const PointerEvent&, bool inside, UIInputRouter& router) override;
    bool OnAccept(UIInputRouter& router) override;
    void Open(UIInputRouter& router);

protected:
    Vec2 ComputeDesired(const LayoutContext& ctx) override;
    void PaintSelf(DrawList& list, const PaintContext& ctx) const override;
    void PaintOver(DrawList& list, const PaintContext& ctx) const override { PaintFocus(list, ctx); }

private:
    friend class UIInputRouter;
    bool open_ = false;
};

// A long list that only makes widgets for the rows in view: `make_row`
// builds a row, `bind_row` fills it for an item (rows are reused as it
// scrolls). Arrows move the selection, which scrolls into view.
class ListView : public Control {
public:
    ListView() { clip_children = true; }
    const char* TypeName() const override { return "ListView"; }
    i32 MaxChildren() const override { return -1; } // its rows (it manages them)
    usize item_count = 0;
    f32 item_height = 40.0f;
    std::function<std::unique_ptr<Widget>()> make_row;
    std::function<void(Widget& row, usize item, bool selected)> bind_row;
    std::function<void(i64)> on_selected;
    std::function<void(i64)> on_activated; // accepted (Enter, A) or double... (accept)
    i64 selected = -1;
    f32 wheel_step = 60.0f;

    f32 Offset() const { return offset_; }
    f32 MaxOffset() const;
    void ScrollTo(f32 offset);
    void ScrollIntoView(usize item);
    void Select(i64 item); // on_selected when it changes
    usize RowWidgets() const { return children_.size(); }
    i64 ItemOfRow(const Widget* row) const;

    bool OnPointerDown(const PointerEvent& e, UIInputRouter&) override;
    bool OnWheel(f32 delta, UIInputRouter&) override;
    bool OnNavigate(NavDirection d, UIInputRouter&) override;
    bool OnAccept(UIInputRouter&) override;

protected:
    Vec2 ComputeDesired(const LayoutContext&) override;
    void ArrangeChildren(const LayoutContext& ctx) override;
    void PaintSelf(DrawList& list, const PaintContext& ctx) const override;
    void PaintOver(DrawList& list, const PaintContext& ctx) const override { PaintFocus(list, ctx); }

private:
    f32 offset_ = 0.0f;
    std::vector<i64> row_items_; // the item each row widget shows
};

} // namespace aether::ui
