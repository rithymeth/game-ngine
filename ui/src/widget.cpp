#include "aether/ui/widget.h"

#include <algorithm>

namespace aether::ui {

Widget* Widget::AddChild(std::unique_ptr<Widget> child) {
    if (!child) return nullptr;
    const i32 max = MaxChildren();
    if (max >= 0 && children_.size() >= static_cast<usize>(max)) return nullptr;
    if (child->parent_ != nullptr) return nullptr;
    child->parent_ = this;
    children_.push_back(std::move(child));
    return children_.back().get();
}

std::unique_ptr<Widget> Widget::RemoveChild(Widget* child) {
    for (auto it = children_.begin(); it != children_.end(); ++it) {
        if (it->get() == child) {
            std::unique_ptr<Widget> out = std::move(*it);
            children_.erase(it);
            out->parent_ = nullptr;
            return out;
        }
    }
    return nullptr;
}

Widget* Widget::Find(const std::string& n) {
    if (name == n) return this;
    for (auto& c : children_) {
        if (Widget* w = c->Find(n)) return w;
    }
    return nullptr;
}

bool Widget::IsEnabled() const {
    for (const Widget* w = this; w != nullptr; w = w->parent_) {
        if (!w->enabled) return false;
    }
    return true;
}

Vec2 Widget::Measure(const LayoutContext& ctx) {
    for (auto& c : children_) c->Measure(ctx);
    desired_ = TakesSpace() ? ComputeDesired(ctx) : Vec2{};
    return desired_;
}

void Widget::Arrange(const Rect& rect, const LayoutContext& ctx) {
    geometry_ = rect;
    ArrangeChildren(ctx);
}

Vec2 Widget::ComputeDesired(const LayoutContext&) {
    Vec2 d;
    for (auto& c : children_) {
        if (!c->TakesSpace()) continue;
        d.x = std::max(d.x, c->DesiredSize().x + c->slot.padding.Horizontal());
        d.y = std::max(d.y, c->DesiredSize().y + c->slot.padding.Vertical());
    }
    return d;
}

Rect Widget::Align(const Widget& child, const Rect& cell) {
    const Rect inner = child.slot.padding.Shrink(cell);
    const Vec2 d = child.DesiredSize();
    Rect r = inner;
    switch (child.slot.h_align) {
    case HAlign::Fill: break;
    case HAlign::Left: r.w = std::min(d.x, inner.w); break;
    case HAlign::Center: r.w = std::min(d.x, inner.w), r.x = inner.x + (inner.w - r.w) * 0.5f; break;
    case HAlign::Right: r.w = std::min(d.x, inner.w), r.x = inner.Right() - r.w; break;
    }
    switch (child.slot.v_align) {
    case VAlign::Fill: break;
    case VAlign::Top: r.h = std::min(d.y, inner.h); break;
    case VAlign::Center: r.h = std::min(d.y, inner.h), r.y = inner.y + (inner.h - r.h) * 0.5f; break;
    case VAlign::Bottom: r.h = std::min(d.y, inner.h), r.y = inner.Bottom() - r.h; break;
    }
    return r;
}

void Widget::ArrangeChildren(const LayoutContext& ctx) {
    for (auto& c : children_) {
        if (c->TakesSpace()) c->Arrange(Align(*c, geometry_), ctx);
    }
}

std::vector<Widget*> Widget::PaintOrder() const {
    std::vector<Widget*> order;
    for (const auto& c : children_) order.push_back(c.get());
    return order;
}

void Widget::Paint(DrawList& list, const PaintContext& ctx) const {
    if (visibility == Visibility::Hidden || visibility == Visibility::Collapsed || opacity <= 0.0f) return;
    PaintContext mine = ctx;
    mine.opacity *= opacity;
    const bool transformed = HasRenderTransform();
    if (transformed) {
        list.PushTransform(render_offset, render_scale, {geometry_.x + render_pivot.x * geometry_.w, geometry_.y + render_pivot.y * geometry_.h});
    }
    PaintSelf(list, mine);
    if (clip_children) list.PushClip(geometry_);
    for (const Widget* c : PaintOrder()) c->Paint(list, mine);
    if (clip_children) list.PopClip();
    PaintOver(list, mine);
    if (transformed) list.PopTransform();
}

Widget* Widget::HitTest(Vec2 p) {
    if (visibility == Visibility::Hidden || visibility == Visibility::Collapsed || visibility == Visibility::HitTestInvisible) return nullptr;
    if (HasRenderTransform()) {
        // Into the widget's own (untransformed) space.
        if (render_scale.x == 0.0f || render_scale.y == 0.0f) return nullptr;
        const Vec2 pivot{geometry_.x + render_pivot.x * geometry_.w, geometry_.y + render_pivot.y * geometry_.h};
        p = {pivot.x + (p.x - render_offset.x - pivot.x) / render_scale.x, pivot.y + (p.y - render_offset.y - pivot.y) / render_scale.y};
    }
    if (clip_children && !geometry_.Contains(p)) return nullptr;
    const std::vector<Widget*> order = PaintOrder();
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        if (Widget* hit = (*it)->HitTest(p)) return hit;
    }
    if (visibility == Visibility::SelfHitTestInvisible || !HitsSelf()) return nullptr;
    return geometry_.Contains(p) ? this : nullptr;
}

} // namespace aether::ui
