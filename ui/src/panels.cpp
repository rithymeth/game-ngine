#include "aether/ui/panels.h"

#include <algorithm>
#include <numeric>

namespace aether::ui {

// --- Canvas ----------------------------------------------------------------------------------------

Vec2 Canvas::ComputeDesired(const LayoutContext&) {
    // What point-anchored children need from the top-left (stretched ones need nothing).
    Vec2 d;
    for (auto& c : children_) {
        if (!c->TakesSpace()) continue;
        const Slot& s = c->slot;
        const Vec2 size = s.auto_size ? c->DesiredSize() : s.size;
        if (!s.anchors.StretchX()) d.x = std::max(d.x, s.position.x + size.x * (1.0f - s.alignment.x));
        if (!s.anchors.StretchY()) d.y = std::max(d.y, s.position.y + size.y * (1.0f - s.alignment.y));
    }
    return d;
}

void Canvas::ArrangeChildren(const LayoutContext& ctx) {
    const Rect& g = Geometry();
    for (auto& c : children_) {
        if (!c->TakesSpace()) continue;
        const Slot& s = c->slot;
        const Vec2 size = s.auto_size ? c->DesiredSize() : s.size;
        Rect r;
        auto axis = [&](bool stretch, f32 amin, f32 amax, f32 origin, f32 extent, f32 pos, f32 len, f32 pivot, f32 inset_min, f32 inset_max, f32& out_pos,
                        f32& out_len) {
            if (stretch) {
                out_pos = origin + amin * extent + inset_min;
                out_len = std::max(0.0f, origin + amax * extent - inset_max - out_pos);
            } else {
                out_len = len;
                out_pos = origin + amin * extent + pos - pivot * len;
            }
        };
        axis(s.anchors.StretchX(), s.anchors.min_x, s.anchors.max_x, g.x, g.w, s.position.x, size.x, s.alignment.x, s.margins.left, s.margins.right, r.x, r.w);
        axis(s.anchors.StretchY(), s.anchors.min_y, s.anchors.max_y, g.y, g.h, s.position.y, size.y, s.alignment.y, s.margins.top, s.margins.bottom, r.y, r.h);
        c->Arrange(r, ctx);
    }
}

std::vector<Widget*> Canvas::PaintOrder() const {
    std::vector<Widget*> order = Widget::PaintOrder();
    std::stable_sort(order.begin(), order.end(), [](const Widget* a, const Widget* b) { return a->slot.z < b->slot.z; });
    return order;
}

// --- Boxes -------------------------------------------------------------------------------------------

Vec2 BoxPanel::ComputeDesired(const LayoutContext&) {
    Vec2 d;
    usize n = 0;
    for (auto& c : children_) {
        if (!c->TakesSpace()) continue;
        const Vec2 cd = c->DesiredSize();
        const f32 along = (horizontal_ ? cd.x : cd.y) + (horizontal_ ? c->slot.padding.Horizontal() : c->slot.padding.Vertical());
        const f32 across = (horizontal_ ? cd.y : cd.x) + (horizontal_ ? c->slot.padding.Vertical() : c->slot.padding.Horizontal());
        (horizontal_ ? d.x : d.y) += along;
        (horizontal_ ? d.y : d.x) = std::max(horizontal_ ? d.y : d.x, across);
        ++n;
    }
    if (n > 1) (horizontal_ ? d.x : d.y) += spacing * static_cast<f32>(n - 1);
    return d;
}

void BoxPanel::ArrangeChildren(const LayoutContext& ctx) {
    const Rect& g = Geometry();
    std::vector<Widget*> kids;
    f32 fixed = 0.0f, weights = 0.0f;
    for (auto& c : children_) {
        if (!c->TakesSpace()) continue;
        kids.push_back(c.get());
        const f32 pad = horizontal_ ? c->slot.padding.Horizontal() : c->slot.padding.Vertical();
        if (c->slot.fill > 0.0f) {
            weights += c->slot.fill;
            fixed += pad;
        } else {
            fixed += (horizontal_ ? c->DesiredSize().x : c->DesiredSize().y) + pad;
        }
    }
    if (kids.empty()) return;
    const f32 extent = horizontal_ ? g.w : g.h;
    const f32 left = std::max(0.0f, extent - fixed - spacing * static_cast<f32>(kids.size() - 1));
    f32 at = horizontal_ ? g.x : g.y;
    for (Widget* c : kids) {
        const f32 pad = horizontal_ ? c->slot.padding.Horizontal() : c->slot.padding.Vertical();
        const f32 content = c->slot.fill > 0.0f ? left * c->slot.fill / weights : (horizontal_ ? c->DesiredSize().x : c->DesiredSize().y);
        const f32 cell = content + pad;
        const Rect r = horizontal_ ? Rect{at, g.y, cell, g.h} : Rect{g.x, at, g.w, cell};
        // Along the box the child gets its whole cell; alignment applies across it.
        Rect placed = Align(*c, r);
        if (horizontal_) placed.x = at + c->slot.padding.left, placed.w = content;
        else placed.y = at + c->slot.padding.top, placed.h = content;
        c->Arrange(placed, ctx);
        at += cell + spacing;
    }
}

// --- Grid --------------------------------------------------------------------------------------------

void Grid::Sizes(std::vector<f32>& widths, std::vector<f32>& heights) const {
    u32 columns = static_cast<u32>(column_fill.size()), rows = static_cast<u32>(row_fill.size());
    for (const auto& c : children_) {
        if (!c->TakesSpace()) continue;
        columns = std::max(columns, c->slot.column + std::max(c->slot.column_span, 1u));
        rows = std::max(rows, c->slot.row + std::max(c->slot.row_span, 1u));
    }
    widths.assign(columns, 0.0f);
    heights.assign(rows, 0.0f);
    for (const auto& c : children_) {
        if (!c->TakesSpace()) continue;
        if (c->slot.column_span <= 1) widths[c->slot.column] = std::max(widths[c->slot.column], c->DesiredSize().x + c->slot.padding.Horizontal());
        if (c->slot.row_span <= 1) heights[c->slot.row] = std::max(heights[c->slot.row], c->DesiredSize().y + c->slot.padding.Vertical());
    }
}

Vec2 Grid::ComputeDesired(const LayoutContext&) {
    std::vector<f32> w, h;
    Sizes(w, h);
    Vec2 d{std::accumulate(w.begin(), w.end(), 0.0f), std::accumulate(h.begin(), h.end(), 0.0f)};
    if (w.size() > 1) d.x += column_spacing * static_cast<f32>(w.size() - 1);
    if (h.size() > 1) d.y += row_spacing * static_cast<f32>(h.size() - 1);
    return d;
}

void Grid::ArrangeChildren(const LayoutContext& ctx) {
    Sizes(widths_, heights_);
    // Weighted tracks share what the others leave.
    auto share = [](std::vector<f32>& tracks, const std::vector<f32>& fill, f32 extent, f32 gap) {
        f32 fixed = gap * static_cast<f32>(tracks.empty() ? 0 : tracks.size() - 1), weights = 0.0f;
        for (usize i = 0; i < tracks.size(); ++i) {
            const f32 w = i < fill.size() ? fill[i] : 0.0f;
            if (w > 0.0f) weights += w;
            else fixed += tracks[i];
        }
        const f32 left = std::max(0.0f, extent - fixed);
        for (usize i = 0; i < tracks.size(); ++i) {
            const f32 w = i < fill.size() ? fill[i] : 0.0f;
            if (w > 0.0f) tracks[i] = left * w / weights;
        }
    };
    const Rect& g = Geometry();
    share(widths_, column_fill, g.w, column_spacing);
    share(heights_, row_fill, g.h, row_spacing);
    auto start = [](const std::vector<f32>& tracks, u32 index, f32 gap) {
        f32 s = 0.0f;
        for (u32 i = 0; i < index && i < tracks.size(); ++i) s += tracks[i] + gap;
        return s;
    };
    for (auto& c : children_) {
        if (!c->TakesSpace()) continue;
        const Slot& s = c->slot;
        const f32 x = g.x + start(widths_, s.column, column_spacing), y = g.y + start(heights_, s.row, row_spacing);
        const f32 w = start(widths_, s.column + std::max(s.column_span, 1u), column_spacing) - start(widths_, s.column, column_spacing) - column_spacing;
        const f32 h = start(heights_, s.row + std::max(s.row_span, 1u), row_spacing) - start(heights_, s.row, row_spacing) - row_spacing;
        c->Arrange(Align(*c, {x, y, std::max(w, 0.0f), std::max(h, 0.0f)}), ctx);
    }
}

// --- SizeBox, Border ----------------------------------------------------------------------------------

Vec2 SizeBox::ComputeDesired(const LayoutContext& ctx) {
    Vec2 d = Widget::ComputeDesired(ctx);
    if (width > 0.0f) d.x = width;
    if (height > 0.0f) d.y = height;
    if (min_width > 0.0f) d.x = std::max(d.x, min_width);
    if (min_height > 0.0f) d.y = std::max(d.y, min_height);
    if (max_width > 0.0f) d.x = std::min(d.x, max_width);
    if (max_height > 0.0f) d.y = std::min(d.y, max_height);
    return d;
}

Vec2 Border::ComputeDesired(const LayoutContext& ctx) {
    const Vec2 d = Widget::ComputeDesired(ctx);
    return {d.x + padding.Horizontal(), d.y + padding.Vertical()};
}

void Border::ArrangeChildren(const LayoutContext& ctx) {
    const Rect inner = padding.Shrink(Geometry());
    for (auto& c : children_) {
        if (c->TakesSpace()) c->Arrange(Align(*c, inner), ctx);
    }
}

void Border::PaintSelf(DrawList& list, const PaintContext& ctx) const { list.AddBrush(Geometry(), background, ctx.opacity); }

// --- ScrollBox -------------------------------------------------------------------------------------------

Vec2 ScrollBox::ComputeDesired(const LayoutContext& ctx) { return Widget::ComputeDesired(ctx); }

f32 ScrollBox::MaxOffset() const { return std::max(0.0f, content_ - (horizontal ? Geometry().w : Geometry().h)); }

void ScrollBox::ScrollTo(f32 offset) { offset_ = std::clamp(offset, 0.0f, MaxOffset()); }

void ScrollBox::ScrollIntoView(const Rect& r) {
    const Rect& g = Geometry();
    const f32 lo = horizontal ? r.x - g.x : r.y - g.y, len = horizontal ? r.w : r.h, view = horizontal ? g.w : g.h;
    // `lo` is where it is now in the view; move the view the least that shows it.
    if (lo < 0.0f) ScrollBy(lo);
    else if (lo + len > view) ScrollBy(lo + len - view);
}

void ScrollBox::ArrangeChildren(const LayoutContext& ctx) {
    const Rect& g = Geometry();
    content_ = 0.0f;
    for (auto& c : children_) {
        if (!c->TakesSpace()) continue;
        // The content keeps its desired length along the scroll (at least the view's) and fills across it.
        const Vec2 d = c->DesiredSize();
        content_ = std::max(horizontal ? d.x + c->slot.padding.Horizontal() : d.y + c->slot.padding.Vertical(), horizontal ? g.w : g.h);
        offset_ = std::clamp(offset_, 0.0f, MaxOffset());
        const Rect area = horizontal ? Rect{g.x - offset_, g.y, content_, g.h} : Rect{g.x, g.y - offset_, g.w, content_};
        c->Arrange(c->slot.padding.Shrink(area), ctx);
    }
}

void ScrollBox::PaintOver(DrawList& list, const PaintContext& ctx) const {
    if (MaxOffset() <= 0.0f) return;
    const Rect& g = Geometry();
    const f32 view = horizontal ? g.w : g.h;
    const f32 thumb = std::max(view * view / content_, scrollbar_thickness * 2.0f);
    const f32 at = (view - thumb) * offset_ / MaxOffset();
    const Rect r = horizontal ? Rect{g.x + at, g.Bottom() - scrollbar_thickness, thumb, scrollbar_thickness}
                              : Rect{g.Right() - scrollbar_thickness, g.y + at, scrollbar_thickness, thumb};
    list.AddBrush(r, scrollbar, ctx.opacity);
}

} // namespace aether::ui
