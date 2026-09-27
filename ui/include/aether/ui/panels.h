#pragma once

#include "aether/ui/widget.h"

namespace aether::ui {

// Children placed by anchors: each edge on an axis attaches to a fraction of
// the canvas. Point anchors place the child at the anchor plus `position`,
// sized `size` (or its desired size with auto_size), about its `alignment`
// pivot. Stretched anchors inset its edges by `margins`. Drawn in `z` order.
class Canvas : public Widget {
public:
    const char* TypeName() const override { return "Canvas"; }
    i32 MaxChildren() const override { return -1; }

protected:
    Vec2 ComputeDesired(const LayoutContext& ctx) override;
    void ArrangeChildren(const LayoutContext& ctx) override;
    std::vector<Widget*> PaintOrder() const override;
};

// Children in a row (HorizontalBox) or column (VerticalBox). Along the box,
// a child with `fill` 0 takes its desired size and the rest share what's
// left by their fill; across it, they align.
class BoxPanel : public Widget {
public:
    f32 spacing = 0.0f;
    i32 MaxChildren() const override { return -1; }

protected:
    explicit BoxPanel(bool horizontal) : horizontal_(horizontal) {}
    Vec2 ComputeDesired(const LayoutContext& ctx) override;
    void ArrangeChildren(const LayoutContext& ctx) override;

private:
    bool horizontal_;
};
class HorizontalBox final : public BoxPanel {
public:
    HorizontalBox() : BoxPanel(true) {}
    const char* TypeName() const override { return "HorizontalBox"; }
};
class VerticalBox final : public BoxPanel {
public:
    VerticalBox() : BoxPanel(false) {}
    const char* TypeName() const override { return "VerticalBox"; }
};

// Children in cells (row, column, spans). A column's width is the widest
// single-column child in it, or, when `column_fill` gives it a weight, a
// share of what's left; rows likewise. Spanning children don't size cells.
class Grid final : public Widget {
public:
    const char* TypeName() const override { return "Grid"; }
    i32 MaxChildren() const override { return -1; }
    std::vector<f32> column_fill, row_fill;
    f32 column_spacing = 0.0f, row_spacing = 0.0f;
    // Cell rectangles from the last Arrange (for tests and the designer).
    const std::vector<f32>& ColumnWidths() const { return widths_; }
    const std::vector<f32>& RowHeights() const { return heights_; }

protected:
    Vec2 ComputeDesired(const LayoutContext& ctx) override;
    void ArrangeChildren(const LayoutContext& ctx) override;

private:
    void Sizes(std::vector<f32>& widths, std::vector<f32>& heights) const;
    std::vector<f32> widths_, heights_;
};

// Children on top of each other, each aligned in the whole area.
class Overlay final : public Widget {
public:
    const char* TypeName() const override { return "Overlay"; }
    i32 MaxChildren() const override { return -1; }
};

// One child with its size overridden or limited (0 = not set).
class SizeBox final : public Widget {
public:
    const char* TypeName() const override { return "SizeBox"; }
    i32 MaxChildren() const override { return 1; }
    f32 width = 0.0f, height = 0.0f;
    f32 min_width = 0.0f, min_height = 0.0f, max_width = 0.0f, max_height = 0.0f;

protected:
    Vec2 ComputeDesired(const LayoutContext& ctx) override;
};

// Empty space of a set size.
class Spacer final : public Widget {
public:
    explicit Spacer(Vec2 size = {}) : size(size) {}
    const char* TypeName() const override { return "Spacer"; }
    Vec2 size;

protected:
    Vec2 ComputeDesired(const LayoutContext&) override { return size; }
};

// One child with a background and padding around it.
class Border final : public Widget {
public:
    const char* TypeName() const override { return "Border"; }
    i32 MaxChildren() const override { return 1; }
    Brush background;
    Margin padding;
    bool hit_test = true; // a background catches clicks

protected:
    Vec2 ComputeDesired(const LayoutContext& ctx) override;
    void ArrangeChildren(const LayoutContext& ctx) override;
    void PaintSelf(DrawList& list, const PaintContext& ctx) const override;
    bool HitsSelf() const override { return hit_test && background.kind != Brush::Kind::None; }
};

// One child that can be taller (or wider) than the box, scrolled and
// clipped, with a scroll bar.
class ScrollBox final : public Widget {
public:
    ScrollBox() { clip_children = true; }
    const char* TypeName() const override { return "ScrollBox"; }
    i32 MaxChildren() const override { return 1; }
    bool horizontal = false;
    f32 scrollbar_thickness = 6.0f;
    Brush scrollbar = Brush::Solid({1, 1, 1, 0.4f});

    f32 Offset() const { return offset_; }
    f32 MaxOffset() const; // after a layout
    void ScrollTo(f32 offset);
    void ScrollBy(f32 delta) { ScrollTo(offset_ + delta); }
    // Scrolls just enough to show `r` (in layout units), e.g. a focused child.
    void ScrollIntoView(const Rect& r);
    f32 wheel_step = 60.0f; // layout units per wheel notch
    bool OnWheel(f32 delta, UIInputRouter&) override;

protected:
    Vec2 ComputeDesired(const LayoutContext& ctx) override;
    void ArrangeChildren(const LayoutContext& ctx) override;
    void PaintOver(DrawList& list, const PaintContext& ctx) const override;
    bool HitsSelf() const override { return true; } // the wheel scrolls it

private:
    f32 offset_ = 0.0f, content_ = 0.0f;
};

} // namespace aether::ui
