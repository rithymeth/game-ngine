#include "aether/ui/input.h"

#include "aether/ui/basic.h"
#include "aether/ui/controls.h"
#include "aether/ui/panels.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace aether::ui {

namespace {

bool Shown(const Widget* w) {
    for (; w != nullptr; w = w->Parent()) {
        if (w->visibility == Visibility::Hidden || w->visibility == Visibility::Collapsed) return false;
    }
    return true;
}

// Not entirely clipped away by a clipping parent (a scroll box scrolled past it).
bool InView(const Widget* w) {
    for (const Widget* p = w->Parent(); p != nullptr; p = p->Parent()) {
        if (p->clip_children && p->Geometry().Intersect(w->Geometry()).Empty()) return false;
    }
    return true;
}

void Collect(Widget* w, std::vector<Widget*>& out) {
    if (w->Focusable() && w->IsEnabled() && Shown(w) && !w->Geometry().Empty() && InView(w)) out.push_back(w);
    for (usize i = 0; i < w->ChildCount(); ++i) Collect(w->Child(i), out);
}

bool Contains(const Widget* root, const Widget* w) {
    if (root == w) return true;
    for (usize i = 0; i < root->ChildCount(); ++i) {
        if (Contains(root->Child(i), w)) return true;
    }
    return false;
}

Vec2 Center(const Rect& r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }

// Scoped "handler is running" marker: popups closed inside handlers wait until it ends.
struct Dispatch {
    int& depth;
    explicit Dispatch(int& d) : depth(d) { ++depth; }
    ~Dispatch() { --depth; }
};

} // namespace

Vec2 UIInputRouter::ToLayout(Vec2 pixel) const {
    const f32 k = viewport_.Scale();
    return {pixel.x / k, pixel.y / k};
}

bool UIInputRouter::Within(const Widget* w, const Widget* root) {
    for (; w != nullptr; w = w->Parent()) {
        if (w == root) return true;
    }
    return false;
}

bool UIInputRouter::Alive(const Widget* w) const {
    if (w == nullptr) return false;
    for (usize i = 0; i < viewport_.LayerCount(); ++i) {
        if (Contains(viewport_.Layer(i), w)) return true;
    }
    return false;
}

void UIInputRouter::Sanitize() {
    if (!Alive(hovered_)) hovered_ = nullptr;
    if (!Alive(focused_)) focused_ = nullptr;
    for (auto it = captured_.begin(); it != captured_.end();) it = Alive(it->second) ? std::next(it) : captured_.erase(it);
    if (!Alive(popup_)) popup_ = nullptr, popup_owner_ = nullptr;
    if (!Alive(popup_owner_)) popup_owner_ = nullptr;
    if (!Alive(tooltip_source_)) tooltip_source_ = nullptr;
    if (!Alive(tooltip_)) tooltip_ = nullptr, tooltip_text_.clear();
}

Widget* UIInputRouter::InteractiveOf(Widget* w) {
    for (; w != nullptr; w = w->Parent()) {
        if (w->Interactive()) return w;
    }
    return nullptr;
}

Widget* UIInputRouter::Captured(u32 pointer) const {
    const auto it = captured_.find(pointer);
    return it == captured_.end() ? nullptr : it->second;
}

void UIInputRouter::SetHovered(Widget* w) {
    if (w == hovered_) return;
    if (hovered_ != nullptr) {
        hovered_->hovered_ = false;
        hovered_->OnHover(false);
    }
    hovered_ = w;
    if (hovered_ != nullptr) {
        hovered_->hovered_ = true;
        hovered_->OnHover(true);
    }
}

void UIInputRouter::SetFocus(Widget* w) {
    Sanitize();
    if (w == focused_) return;
    if (focused_ != nullptr) {
        focused_->focused_ = false;
        focused_->OnFocus(false);
    }
    focused_ = w;
    if (focused_ == nullptr) return;
    focused_->focused_ = true;
    focused_->OnFocus(true);
    // Bring it into view in every scroll box it's inside.
    for (Widget* p = focused_->Parent(); p != nullptr; p = p->Parent()) {
        if (auto* scroll = dynamic_cast<ScrollBox*>(p)) scroll->ScrollIntoView(focused_->Geometry());
    }
    Relayout();
}

void UIInputRouter::Finish() {
    if (dispatching_ > 0) return;
    if (close_popup_) {
        close_popup_ = false;
        ClosePopup();
    }
}

bool UIInputRouter::PointerMove(Vec2 pixel, u32 pointer) {
    Sanitize();
    const Vec2 p = ToLayout(pixel);
    Widget* hit = viewport_.HitTest(pixel);
    if (pointer == 0) {
        pointer_ = p;
        if (Widget* c = Captured(0)) SetHovered(Within(hit, c) ? c : nullptr); // a held control stays "pressed" only while over it
        else SetHovered(InteractiveOf(hit));
        // A tooltip's source: the nearest widget with one.
        Widget* source = hit;
        while (source != nullptr && source->tooltip.empty()) source = source->Parent();
        if (source != tooltip_source_) {
            HideTooltip();
            tooltip_source_ = source;
            hover_time_ = 0.0f;
        }
    }
    bool used = hit != nullptr;
    if (Widget* c = Captured(pointer)) {
        Dispatch d(dispatching_);
        c->OnPointerMove({p, 0, pointer}, *this);
        used = true;
    }
    Finish();
    return used;
}

bool UIInputRouter::PointerDown(Vec2 pixel, int button, u32 pointer) {
    Sanitize();
    HideTooltip();
    tooltip_source_ = nullptr;
    const Vec2 p = ToLayout(pixel);
    Widget* hit = viewport_.HitTest(pixel);
    if (popup_ != nullptr && !Within(hit, popup_)) {
        // A click outside a popup only closes it.
        Widget* owner = popup_owner_;
        ClosePopup();
        if (owner != nullptr) SetFocus(owner);
        return true;
    }
    Widget* target = InteractiveOf(hit);
    if (target != nullptr && target->IsEnabled()) {
        if (target->Focusable()) SetFocus(target);
        Dispatch d(dispatching_);
        if (target->OnPointerDown({p, button, pointer}, *this)) captured_[pointer] = target;
    }
    Finish();
    return hit != nullptr;
}

bool UIInputRouter::PointerUp(Vec2 pixel, int button, u32 pointer) {
    Sanitize();
    const Vec2 p = ToLayout(pixel);
    Widget* hit = viewport_.HitTest(pixel);
    Widget* c = Captured(pointer);
    if (c == nullptr) return hit != nullptr;
    captured_.erase(pointer);
    {
        Dispatch d(dispatching_);
        c->OnPointerUp({p, button, pointer}, Within(hit, c), *this);
    }
    Finish();
    Sanitize();
    if (pointer == 0) SetHovered(InteractiveOf(viewport_.HitTest(pixel)));
    return true;
}

bool UIInputRouter::Wheel(Vec2 pixel, f32 delta) {
    Sanitize();
    Widget* hit = viewport_.HitTest(pixel);
    {
        Dispatch d(dispatching_);
        for (Widget* w = hit; w != nullptr; w = w->Parent()) {
            if (w->IsEnabled() && w->OnWheel(delta, *this)) break;
        }
    }
    Relayout(); // scroll offsets apply on layout
    Finish();
    return hit != nullptr;
}

std::vector<Widget*> UIInputRouter::Candidates() const {
    std::vector<Widget*> out;
    if (popup_ != nullptr) {
        Collect(popup_, out);
        return out;
    }
    for (usize i = 0; i < viewport_.LayerCount(); ++i) {
        Widget* layer = viewport_.Layer(i);
        if (layer != tooltip_) Collect(layer, out);
    }
    return out;
}

Widget* UIInputRouter::NavigationTarget(Widget* from, NavDirection dir) const {
    const std::vector<Widget*> all = Candidates();
    if (all.empty()) return nullptr;
    if (from == nullptr) return dir == NavDirection::Previous ? all.back() : all.front();
    // An explicit target by name wins.
    const std::string* named = nullptr;
    switch (dir) {
    case NavDirection::Up: named = &from->navigation.up; break;
    case NavDirection::Down: named = &from->navigation.down; break;
    case NavDirection::Left: named = &from->navigation.left; break;
    case NavDirection::Right: named = &from->navigation.right; break;
    default: break;
    }
    if (named != nullptr && !named->empty()) {
        for (Widget* w : all) {
            if (w->name == *named) return w;
        }
    }
    if (dir == NavDirection::Next || dir == NavDirection::Previous) {
        // Tree order, wrapping round.
        const auto it = std::find(all.begin(), all.end(), from);
        if (it == all.end()) return all.front();
        const usize i = static_cast<usize>(it - all.begin());
        return dir == NavDirection::Next ? all[(i + 1) % all.size()] : all[(i + all.size() - 1) % all.size()];
    }
    // Spatially: the nearest whose centre lies that way, favouring ones in line.
    const Vec2 c0 = Center(from->Geometry());
    Widget* best = nullptr;
    f32 best_score = 1e30f;
    for (Widget* w : all) {
        if (w == from) continue;
        const Vec2 d = Center(w->Geometry()) - c0;
        f32 along = 0.0f, across = 0.0f;
        switch (dir) {
        case NavDirection::Up: along = -d.y, across = d.x; break;
        case NavDirection::Down: along = d.y, across = d.x; break;
        case NavDirection::Left: along = -d.x, across = d.y; break;
        case NavDirection::Right: along = d.x, across = d.y; break;
        default: break;
        }
        if (along <= 0.5f) continue;
        const f32 score = along + 2.0f * std::fabs(across);
        if (score < best_score) best_score = score, best = w;
    }
    return best;
}

bool UIInputRouter::Navigate(NavDirection dir) {
    Sanitize();
    HideTooltip();
    if (focused_ != nullptr) {
        Dispatch d(dispatching_);
        if (focused_->IsEnabled() && focused_->OnNavigate(dir, *this)) {
            Finish();
            return true;
        }
    }
    Widget* next = NavigationTarget(focused_, dir);
    if (next == nullptr) return focused_ != nullptr;
    SetFocus(next);
    return true;
}

bool UIInputRouter::Accept() {
    Sanitize();
    if (focused_ == nullptr || !focused_->IsEnabled()) return false;
    bool used;
    {
        Dispatch d(dispatching_);
        used = focused_->OnAccept(*this);
    }
    Finish();
    return used;
}

bool UIInputRouter::Cancel() {
    Sanitize();
    if (popup_ != nullptr) {
        Widget* owner = popup_owner_;
        ClosePopup();
        if (owner != nullptr) SetFocus(owner);
        return true;
    }
    bool used = false;
    {
        Dispatch d(dispatching_);
        for (Widget* w = focused_; w != nullptr && !used; w = w->Parent()) used = w->OnCancel(*this);
    }
    Finish();
    return used;
}

bool UIInputRouter::Text(std::string_view utf8) {
    Sanitize();
    if (focused_ == nullptr || !focused_->WantsText() || !focused_->IsEnabled()) return false;
    Dispatch d(dispatching_);
    const bool used = focused_->OnText(utf8, *this);
    Relayout();
    return used;
}

bool UIInputRouter::Key(EditKey key, bool shift) {
    Sanitize();
    if (focused_ == nullptr || !focused_->WantsText() || !focused_->IsEnabled()) return false;
    Dispatch d(dispatching_);
    const bool used = focused_->OnKey(key, shift, *this);
    Relayout();
    return used;
}

Widget* UIInputRouter::OpenPopup(std::unique_ptr<Widget> content, const Rect& below, Widget* owner) {
    if (popup_ != nullptr) {
        viewport_.Remove(popup_);
        popup_ = nullptr;
    }
    auto root = std::make_unique<Canvas>();
    root->name = "__popup";
    const Rect safe = viewport_.SafeRect();
    content->slot.auto_size = true;
    content->slot.position = {below.x - safe.x, below.Bottom() - safe.y};
    Widget* inner = root->AddChild(std::move(content));
    popup_ = viewport_.Add(std::move(root), kPopupZ);
    popup_owner_ = owner;
    Relayout();
    // No room below: open above instead.
    if (inner->Geometry().Bottom() > safe.Bottom() && below.y - inner->Geometry().h >= safe.y) {
        inner->slot.position.y = below.y - inner->Geometry().h - safe.y;
        Relayout();
    }
    return inner;
}

void UIInputRouter::ClosePopup() {
    if (dispatching_ > 0) {
        close_popup_ = true;
        return;
    }
    if (popup_ == nullptr) return;
    if (auto* dd = dynamic_cast<Dropdown*>(popup_owner_)) dd->open_ = false;
    Widget* doomed = popup_;
    popup_ = nullptr;
    popup_owner_ = nullptr;
    viewport_.Remove(doomed); // destroyed here
    Sanitize();
    Relayout();
}

void UIInputRouter::HideTooltip() {
    if (tooltip_ != nullptr) {
        viewport_.Remove(tooltip_);
        tooltip_ = nullptr;
        tooltip_text_.clear();
    }
    hover_time_ = 0.0f;
}

void UIInputRouter::Tick(f32 dt) {
    Sanitize();
    if (tooltip_source_ == nullptr || tooltip_ != nullptr || Captured(0) != nullptr) return; // not while dragging
    hover_time_ += dt;
    if (hover_time_ < tooltip_delay) return;
    auto root = std::make_unique<Canvas>();
    root->name = "__tooltip";
    root->visibility = Visibility::HitTestInvisible;
    auto* box = root->Add<Border>();
    box->background = Brush::Solid({0.08f, 0.08f, 0.1f, 0.95f});
    box->padding = {8, 4, 8, 4};
    box->slot.auto_size = true;
    const Rect safe = viewport_.SafeRect();
    box->slot.position = {pointer_.x + 16.0f - safe.x, pointer_.y + 20.0f - safe.y};
    auto* label = box->Add<ui::Text>(tooltip_source_->tooltip);
    label->size = 16.0f;
    tooltip_text_ = tooltip_source_->tooltip;
    tooltip_ = viewport_.Add(std::move(root), kTooltipZ);
    Relayout();
}

// --- The bridge from device state ---------------------------------------------------------------------

bool UIInputBridge::Fire(Pad pad, bool down, f32 dt, bool repeats) {
    if (!down) {
        held_[pad] = false;
        return false;
    }
    if (!held_[pad]) {
        held_[pad] = true;
        timer_[pad] = repeat_delay;
        return true;
    }
    if (!repeats) return false;
    timer_[pad] -= dt;
    if (timer_[pad] > 0.0f) return false;
    timer_[pad] += repeat_interval;
    return true;
}

void UIInputBridge::Update(const input::InputState& s, f32 dt) {
    using input::Key;
    const bool shift = s.IsDown(Key::LeftShift) || s.IsDown(Key::RightShift);
    const bool ctrl = s.IsDown(Key::LeftCtrl) || s.IsDown(Key::RightCtrl);
    const Widget* focused = router_.Focused();
    const bool typing = focused != nullptr && focused->WantsText();
    const f32 sx = s.Value(Key::GamepadLeftStickX), sy = s.Value(Key::GamepadLeftStickY);
    // In a text box the arrow keys move the cursor; the d-pad and stick still navigate.
    const bool up = s.IsDown(Key::Up) || s.IsDown(Key::GamepadDPadUp) || sy > stick_threshold;
    const bool down = s.IsDown(Key::Down) || s.IsDown(Key::GamepadDPadDown) || sy < -stick_threshold;
    const bool left = (!typing && s.IsDown(Key::Left)) || s.IsDown(Key::GamepadDPadLeft) || sx < -stick_threshold;
    const bool right = (!typing && s.IsDown(Key::Right)) || s.IsDown(Key::GamepadDPadRight) || sx > stick_threshold;
    if (Fire(Up, up, dt, true)) router_.Navigate(NavDirection::Up);
    if (Fire(Down, down, dt, true)) router_.Navigate(NavDirection::Down);
    if (Fire(Left, left, dt, true)) router_.Navigate(NavDirection::Left);
    if (Fire(Right, right, dt, true)) router_.Navigate(NavDirection::Right);
    if (Fire(Tab, s.IsDown(Key::Tab), dt, true)) router_.Navigate(shift ? NavDirection::Previous : NavDirection::Next);
    const bool accept = s.IsDown(Key::Enter) || s.IsDown(Key::GamepadA) || (!typing && s.IsDown(Key::Space));
    if (Fire(Accept, accept, dt, false)) router_.Accept();
    if (Fire(Cancel, s.IsDown(Key::Escape) || s.IsDown(Key::GamepadB), dt, false)) router_.Cancel();
    if (Fire(Backspace, typing && s.IsDown(Key::Backspace), dt, true)) router_.Key(EditKey::Backspace, shift);
    if (Fire(Delete, typing && s.IsDown(Key::Delete), dt, true)) router_.Key(EditKey::Delete, shift);
    if (Fire(EditLeft, typing && s.IsDown(Key::Left), dt, true)) router_.Key(EditKey::Left, shift);
    if (Fire(EditRight, typing && s.IsDown(Key::Right), dt, true)) router_.Key(EditKey::Right, shift);
    if (Fire(Home, typing && s.IsDown(Key::Home), dt, false)) router_.Key(EditKey::Home, shift);
    if (Fire(End, typing && s.IsDown(Key::End), dt, false)) router_.Key(EditKey::End, shift);
    if (Fire(SelectAll, typing && ctrl && s.IsDown(Key::A), dt, false)) router_.Key(EditKey::SelectAll, false);
}

void UIInputBridge::SetModal(input::InputSystem& system, bool modal, i32 priority) {
    if (!modal) {
        system.RemoveContext(kModalContext);
        return;
    }
    input::InputMappingContext context;
    context.name = kModalContext;
    context.block_lower_contexts = true;
    system.AddContext(std::move(context), priority);
}

} // namespace aether::ui
