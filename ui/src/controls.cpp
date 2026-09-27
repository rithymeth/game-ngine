#include "aether/ui/controls.h"

#include "aether/ui/basic.h"
#include "aether/ui/input.h"
#include "aether/ui/panels.h"

#include <algorithm>
#include <cmath>

namespace aether::ui {

const ControlStyle& DefaultStyle() {
    static const ControlStyle style;
    return style;
}

const Brush& Control::StateBrush(bool pressed) const {
    if (!IsEnabled()) return style.disabled;
    if (pressed) return style.pressed;
    return IsHovered() ? style.hovered : style.normal;
}

void Control::PaintFocus(DrawList& list, const PaintContext& ctx) const {
    if (!IsFocused()) return;
    const Rect& g = Geometry();
    const f32 w = style.focus_width;
    const Color c = style.focus.tint.WithAlpha(ctx.opacity);
    list.AddQuad({g.x, g.y, g.w, w}, c);
    list.AddQuad({g.x, g.Bottom() - w, g.w, w}, c);
    list.AddQuad({g.x, g.y + w, w, g.h - 2 * w}, c);
    list.AddQuad({g.Right() - w, g.y + w, w, g.h - 2 * w}, c);
}

// --- Button ----------------------------------------------------------------------------------------

Vec2 Button::ComputeDesired(const LayoutContext& ctx) {
    const Vec2 d = Widget::ComputeDesired(ctx);
    return {d.x + padding.Horizontal(), d.y + padding.Vertical()};
}

void Button::ArrangeChildren(const LayoutContext& ctx) {
    const Rect inner = padding.Shrink(Geometry());
    for (auto& c : children_) {
        if (c->TakesSpace()) c->Arrange(Align(*c, inner), ctx);
    }
}

void Button::PaintSelf(DrawList& list, const PaintContext& ctx) const { list.AddBrush(Geometry(), StateBrush(pressed_), ctx.opacity * (IsEnabled() ? 1.0f : 0.6f)); }

bool Button::OnPointerDown(const PointerEvent& e, UIInputRouter&) {
    if (e.button != 0) return false;
    pressed_ = true;
    if (on_pressed) on_pressed();
    return true;
}

void Button::OnPointerUp(const PointerEvent&, bool inside, UIInputRouter&) {
    pressed_ = false;
    if (on_released) on_released();
    if (inside) Click();
}

bool Button::OnAccept(UIInputRouter&) {
    Click();
    return true;
}

void Button::OnHover(bool hovered) {
    if (on_hovered) on_hovered(hovered);
}

void Button::Click() {
    if (IsEnabled() && on_clicked) on_clicked();
}

// --- Toggle ----------------------------------------------------------------------------------------

void Toggle::SetChecked(bool value) {
    if (value == checked) return;
    checked = value;
    if (on_changed) on_changed(checked);
}

void Toggle::OnPointerUp(const PointerEvent&, bool inside, UIInputRouter&) {
    if (inside) SetChecked(!checked);
}

bool Toggle::OnAccept(UIInputRouter&) {
    SetChecked(!checked);
    return true;
}

void Toggle::PaintSelf(DrawList& list, const PaintContext& ctx) const {
    const Rect& g = Geometry();
    list.AddBrush(g, StateBrush(false), ctx.opacity);
    if (checked) {
        const f32 inset = g.w * 0.25f;
        list.AddBrush({g.x + inset, g.y + inset, g.w - 2 * inset, g.h - 2 * inset}, style.accent, ctx.opacity);
    }
}

// --- Slider ----------------------------------------------------------------------------------------

void Slider::SetValue(f32 v) {
    v = std::clamp(v, std::min(min, max), std::max(min, max));
    if (step > 0.0f) v = std::clamp(min + std::round((v - min) / step) * step, std::min(min, max), std::max(min, max));
    if (v == value) return;
    value = v;
    if (on_changed) on_changed(value);
}

void Slider::FromPointer(f32 x) {
    const Rect& g = Geometry();
    const f32 t = g.w > 0.0f ? std::clamp((x - g.x) / g.w, 0.0f, 1.0f) : 0.0f;
    SetValue(min + (max - min) * t);
}

bool Slider::OnPointerDown(const PointerEvent& e, UIInputRouter&) {
    FromPointer(e.position.x);
    return true;
}

void Slider::OnPointerMove(const PointerEvent& e, UIInputRouter&) { FromPointer(e.position.x); }

bool Slider::OnNavigate(NavDirection d, UIInputRouter&) {
    if (d != NavDirection::Left && d != NavDirection::Right) return false; // up/down move focus
    const f32 n = nav_step > 0.0f ? nav_step : step > 0.0f ? step : (max - min) / 20.0f;
    SetValue(value + (d == NavDirection::Right ? n : -n));
    return true;
}

void Slider::PaintSelf(DrawList& list, const PaintContext& ctx) const {
    const Rect& g = Geometry();
    const f32 t = max != min ? std::clamp((value - min) / (max - min), 0.0f, 1.0f) : 0.0f;
    const f32 track_h = std::max(4.0f, g.h * 0.2f);
    const Rect track{g.x, g.y + (g.h - track_h) * 0.5f, g.w, track_h};
    list.AddBrush(track, StateBrush(false), ctx.opacity);
    list.AddBrush({track.x, track.y, track.w * t, track.h}, style.accent, ctx.opacity);
    const f32 thumb = g.h;
    list.AddBrush({g.x + (g.w - thumb) * t, g.y, thumb, thumb}, IsHovered() ? style.hovered : style.accent, ctx.opacity);
}

// --- ProgressBar -----------------------------------------------------------------------------------

void ProgressBar::PaintSelf(DrawList& list, const PaintContext& ctx) const {
    const Rect& g = Geometry();
    list.AddBrush(g, background, ctx.opacity);
    const f32 p = std::clamp(percent, 0.0f, 1.0f);
    Rect r = g;
    switch (fill) {
    case Fill::LeftToRight: r.w = g.w * p; break;
    case Fill::RightToLeft: r.w = g.w * p, r.x = g.Right() - r.w; break;
    case Fill::TopToBottom: r.h = g.h * p; break;
    case Fill::BottomToTop: r.h = g.h * p, r.y = g.Bottom() - r.h; break;
    }
    list.AddBrush(r, bar, ctx.opacity);
}

// --- TextInput -------------------------------------------------------------------------------------

namespace {

constexpr f32 kCaretWidth = 2.0f;

usize PrevBoundary(const std::string& s, usize i) {
    if (i == 0) return 0;
    --i;
    while (i > 0 && (static_cast<u8>(s[i]) & 0xC0) == 0x80) --i;
    return i;
}
usize NextBoundary(const std::string& s, usize i) {
    if (i >= s.size()) return s.size();
    ++i;
    while (i < s.size() && (static_cast<u8>(s[i]) & 0xC0) == 0x80) ++i;
    return i;
}
usize CodePoints(std::string_view s) {
    usize n = 0;
    for (usize i = 0; i < s.size();) {
        DecodeUtf8(s, i);
        ++n;
    }
    return n;
}

} // namespace

void TextInput::SetText(const std::string& t) {
    text = t;
    cursor_ = anchor_ = text.size();
    scroll_ = 0.0f;
}

void TextInput::SetCursor(usize byte, bool extend) {
    byte = std::min(byte, text.size());
    while (byte > 0 && byte < text.size() && (static_cast<u8>(text[byte]) & 0xC0) == 0x80) --byte; // on a boundary
    cursor_ = byte;
    if (!extend) anchor_ = cursor_;
}

std::string TextInput::Shown() const {
    if (!password) return text;
    return std::string(CodePoints(text), '*');
}

// Byte offsets map to the shown text: for passwords, the n-th code point is the n-th '*'.
f32 TextInput::XOf(usize byte) const {
    if (font_ == nullptr) return 0.0f;
    const std::string shown = Shown();
    const usize end = password ? CodePoints(std::string_view(text).substr(0, byte)) : byte;
    return LayoutText(*font_, std::string_view(shown).substr(0, end), style.text_size).size.x;
}

usize TextInput::ByteAt(f32 x) const {
    usize best = 0;
    f32 best_d = 1e30f;
    for (usize i = 0;; i = NextBoundary(text, i)) {
        const f32 d = std::fabs(XOf(i) - x);
        if (d < best_d) best_d = d, best = i;
        if (i >= text.size()) break;
    }
    return best;
}

void TextInput::DeleteSelection() {
    if (!HasSelection()) return;
    const usize a = SelectionStart(), b = SelectionEnd();
    text.erase(a, b - a);
    cursor_ = anchor_ = a;
}

void TextInput::Changed() {
    if (on_changed) on_changed(text);
}

bool TextInput::OnPointerDown(const PointerEvent& e, UIInputRouter&) {
    SetCursor(ByteAt(e.position.x - Geometry().x - padding.left + scroll_));
    return true;
}

void TextInput::OnPointerMove(const PointerEvent& e, UIInputRouter&) {
    SetCursor(ByteAt(e.position.x - Geometry().x - padding.left + scroll_), true); // drag selects
}

bool TextInput::OnText(std::string_view typed, UIInputRouter&) {
    std::string clean;
    for (char c : typed) {
        if (static_cast<u8>(c) >= 0x20 && c != 0x7F) clean += c; // no control characters (a single line)
    }
    if (clean.empty()) return true;
    DeleteSelection();
    if (max_length > 0) {
        const usize room = max_length > CodePoints(text) ? max_length - CodePoints(text) : 0;
        usize cut = 0, n = 0;
        while (cut < clean.size() && n < room) {
            DecodeUtf8(clean, cut);
            ++n;
        }
        clean.resize(cut);
    }
    text.insert(cursor_, clean);
    cursor_ = anchor_ = cursor_ + clean.size();
    Changed();
    return true;
}

bool TextInput::OnKey(EditKey key, bool shift, UIInputRouter&) {
    switch (key) {
    case EditKey::Backspace:
        if (HasSelection()) DeleteSelection();
        else if (cursor_ > 0) {
            const usize p = PrevBoundary(text, cursor_);
            text.erase(p, cursor_ - p);
            cursor_ = anchor_ = p;
        } else return true;
        Changed();
        return true;
    case EditKey::Delete:
        if (HasSelection()) DeleteSelection();
        else if (cursor_ < text.size()) text.erase(cursor_, NextBoundary(text, cursor_) - cursor_);
        else return true;
        Changed();
        return true;
    case EditKey::Left:
        if (!shift && HasSelection()) SetCursor(SelectionStart());
        else SetCursor(PrevBoundary(text, cursor_), shift);
        return true;
    case EditKey::Right:
        if (!shift && HasSelection()) SetCursor(SelectionEnd());
        else SetCursor(NextBoundary(text, cursor_), shift);
        return true;
    case EditKey::Home: SetCursor(0, shift); return true;
    case EditKey::End: SetCursor(text.size(), shift); return true;
    case EditKey::SelectAll:
        anchor_ = 0;
        cursor_ = text.size();
        return true;
    }
    return false;
}

bool TextInput::OnAccept(UIInputRouter&) {
    if (on_committed) on_committed(text);
    return true;
}

bool TextInput::OnNavigate(NavDirection d, UIInputRouter&) {
    // Left/right from the d-pad move the cursor too; the rest leave the box.
    if (d == NavDirection::Left && cursor_ > 0) return SetCursor(PrevBoundary(text, cursor_)), true;
    if (d == NavDirection::Right && cursor_ < text.size()) return SetCursor(NextBoundary(text, cursor_)), true;
    return false;
}

Vec2 TextInput::ComputeDesired(const LayoutContext& ctx) {
    font_ = ctx.font;
    const f32 line = ctx.font != nullptr ? ctx.font->LineHeight(style.text_size) : style.text_size;
    return {200.0f, line + padding.Vertical()};
}

void TextInput::ArrangeChildren(const LayoutContext& ctx) {
    font_ = ctx.font;
    // Keep the cursor (and its width) in view.
    const f32 view = std::max(0.0f, Geometry().w - padding.Horizontal() - kCaretWidth);
    const f32 x = XOf(cursor_);
    if (x - scroll_ > view) scroll_ = x - view;
    if (x < scroll_) scroll_ = x;
    scroll_ = std::max(0.0f, std::min(scroll_, std::max(0.0f, XOf(text.size()) - view)));
}

void TextInput::PaintSelf(DrawList& list, const PaintContext& ctx) const {
    const Rect& g = Geometry();
    list.AddBrush(g, StateBrush(false), ctx.opacity);
    if (ctx.font == nullptr) return;
    const Rect inner = padding.Shrink(g);
    list.PushClip(inner);
    const Rect at{inner.x - scroll_, inner.y, inner.w + scroll_, inner.h};
    if (HasSelection() && IsFocused()) {
        const f32 x0 = XOf(SelectionStart()), x1 = XOf(SelectionEnd());
        list.AddBrush({at.x + x0, inner.y, x1 - x0, inner.h}, style.accent, ctx.opacity * 0.5f);
    }
    if (text.empty() && !IsFocused()) list.AddText(*ctx.font, hint, style.text_size, inner, style.hint.WithAlpha(ctx.opacity));
    else list.AddText(*ctx.font, Shown(), style.text_size, at, style.text.WithAlpha(ctx.opacity));
    if (IsFocused()) list.AddQuad({at.x + XOf(cursor_), inner.y, kCaretWidth, inner.h}, style.text.WithAlpha(ctx.opacity));
    list.PopClip();
}

// --- Dropdown --------------------------------------------------------------------------------------

void Dropdown::Select(i32 index) {
    if (index < -1 || index >= static_cast<i32>(options.size()) || index == selected) return;
    selected = index;
    if (on_changed) on_changed(selected);
}

Vec2 Dropdown::ComputeDesired(const LayoutContext& ctx) {
    f32 widest = 0.0f;
    if (ctx.font != nullptr) {
        for (const std::string& o : options) widest = std::max(widest, LayoutText(*ctx.font, o, style.text_size).size.x);
    }
    const f32 line = ctx.font != nullptr ? ctx.font->LineHeight(style.text_size) : style.text_size;
    return {widest + 48.0f, line + 12.0f}; // room for the arrow
}

void Dropdown::PaintSelf(DrawList& list, const PaintContext& ctx) const {
    const Rect& g = Geometry();
    list.AddBrush(g, StateBrush(open_), ctx.opacity);
    if (ctx.font != nullptr && selected >= 0 && selected < static_cast<i32>(options.size())) {
        list.AddText(*ctx.font, options[static_cast<usize>(selected)], style.text_size, {g.x + 8, g.y + 6, g.w - 40, g.h - 12}, style.text.WithAlpha(ctx.opacity));
    }
    list.AddQuad({g.Right() - 24, g.y + g.h * 0.5f - 3, 12, 6}, style.text.WithAlpha(ctx.opacity)); // the arrow
}

void Dropdown::OnPointerUp(const PointerEvent&, bool inside, UIInputRouter& router) {
    if (inside) Open(router);
}

bool Dropdown::OnAccept(UIInputRouter& router) {
    Open(router);
    return true;
}

void Dropdown::Open(UIInputRouter& router) {
    if (options.empty()) return;
    auto panel = std::make_unique<Border>();
    panel->background = Brush::Solid({0.12f, 0.13f, 0.15f, 0.98f});
    panel->padding = Margin::All(2);
    auto* size = panel->Add<SizeBox>();
    size->min_width = Geometry().w - 4;
    auto* list = size->Add<VerticalBox>();
    Button* focus = nullptr;
    for (usize i = 0; i < options.size(); ++i) {
        auto* b = list->Add<Button>();
        b->name = "option" + std::to_string(i);
        b->style = style;
        b->padding = {8, 4, 8, 4};
        auto* label = b->Add<Text>(options[i]);
        label->size = style.text_size;
        label->color = style.text;
        const i32 index = static_cast<i32>(i);
        b->on_clicked = [this, index, &router] {
            Select(index);
            router.ClosePopup(); // after this click is done
            router.SetFocus(this);
        };
        if (focus == nullptr || index == selected) focus = b; // the selected option, else the first
    }
    router.OpenPopup(std::move(panel), Geometry(), this);
    open_ = true;
    if (focus != nullptr) router.SetFocus(focus);
}

// --- ListView --------------------------------------------------------------------------------------

Vec2 ListView::ComputeDesired(const LayoutContext&) { return {200.0f, item_height * static_cast<f32>(std::min<usize>(item_count, 5))}; }

f32 ListView::MaxOffset() const { return std::max(0.0f, item_height * static_cast<f32>(item_count) - Geometry().h); }

void ListView::ScrollTo(f32 offset) { offset_ = std::clamp(offset, 0.0f, MaxOffset()); }

void ListView::ScrollIntoView(usize item) {
    const f32 top = item_height * static_cast<f32>(item), bottom = top + item_height;
    if (top < offset_) ScrollTo(top);
    else if (bottom > offset_ + Geometry().h) ScrollTo(bottom - Geometry().h);
}

void ListView::Select(i64 item) {
    if (item < -1 || item >= static_cast<i64>(item_count) || item == selected) return;
    selected = item;
    if (item >= 0) ScrollIntoView(static_cast<usize>(item));
    if (on_selected) on_selected(selected);
}

i64 ListView::ItemOfRow(const Widget* row) const {
    for (usize i = 0; i < children_.size(); ++i) {
        if (children_[i].get() == row) return row_items_[i];
    }
    return -1;
}

void ListView::ArrangeChildren(const LayoutContext& ctx) {
    const Rect& g = Geometry();
    offset_ = std::clamp(offset_, 0.0f, MaxOffset());
    if (item_height <= 0.0f || item_count == 0 || !make_row) {
        children_.clear();
        row_items_.clear();
        return;
    }
    // Only the rows in view (plus a partly shown one): reuse the pool, growing or shrinking it.
    const usize first = static_cast<usize>(offset_ / item_height);
    const usize last = std::min(item_count, static_cast<usize>(std::ceil((offset_ + g.h) / item_height)));
    const usize count = last > first ? last - first : 0;
    while (children_.size() < count) {
        if (AddChild(make_row()) == nullptr) return; // make_row gave nothing
    }
    while (children_.size() > count) children_.pop_back();
    row_items_.assign(count, -1);
    for (usize i = 0; i < count; ++i) {
        const usize item = first + i;
        Widget& row = *children_[i];
        row_items_[i] = static_cast<i64>(item);
        if (bind_row) bind_row(row, item, static_cast<i64>(item) == selected);
        row.Measure(ctx);
        row.Arrange({g.x, g.y + item_height * static_cast<f32>(item) - offset_, g.w, item_height}, ctx);
    }
}

void ListView::PaintSelf(DrawList& list, const PaintContext& ctx) const {
    const Rect& g = Geometry();
    list.AddBrush(g, style.normal, ctx.opacity);
    if (selected >= 0) {
        list.PushClip(g);
        list.AddBrush({g.x, g.y + item_height * static_cast<f32>(selected) - offset_, g.w, item_height}, style.accent, ctx.opacity * 0.6f);
        list.PopClip();
    }
}

bool ListView::OnPointerDown(const PointerEvent& e, UIInputRouter&) {
    const f32 y = e.position.y - Geometry().y + offset_;
    if (y >= 0.0f && item_height > 0.0f) {
        const usize item = static_cast<usize>(y / item_height);
        if (item < item_count) Select(static_cast<i64>(item));
    }
    return true;
}

bool ListView::OnWheel(f32 delta, UIInputRouter&) {
    if (MaxOffset() <= 0.0f) return false;
    ScrollTo(offset_ - delta * wheel_step);
    return true;
}

bool ListView::OnNavigate(NavDirection d, UIInputRouter&) {
    if (item_count == 0) return false;
    if (d == NavDirection::Down && selected + 1 < static_cast<i64>(item_count)) return Select(selected + 1), true;
    if (d == NavDirection::Up && selected > 0) return Select(selected - 1), true;
    return false; // past the ends, and sideways: focus moves on
}

bool ListView::OnAccept(UIInputRouter&) {
    if (selected < 0) return false;
    if (on_activated) on_activated(selected);
    return true;
}

} // namespace aether::ui
