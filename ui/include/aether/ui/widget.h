#pragma once

#include "aether/ui/draw.h"

#include <memory>
#include <string>
#include <vector>

namespace aether::ui {

enum class Visibility : u8 {
    Visible,              // drawn and hit
    Hidden,               // takes its space, not drawn or hit
    Collapsed,            // takes no space
    HitTestInvisible,     // drawn; neither it nor its children are hit
    SelfHitTestInvisible, // drawn; its children can be hit, it can't
};

// How a widget's parent places it. Each panel reads the fields it uses:
// Canvas the anchors, position, size, alignment, margins, auto_size and z;
// boxes the padding, fill and alignment; Grid the cell and padding;
// Overlay and single-child panels the padding and alignment.
struct Slot {
    // Canvas.
    Anchors anchors;
    Vec2 position;           // non-stretched axes: from the anchor point
    Vec2 size{100.0f, 30.0f}; // non-stretched axes
    Vec2 alignment;          // the child's pivot, 0..1 (0.5 centres it on the anchor)
    Margin margins;          // stretched axes: insets from the anchored edges
    bool auto_size = false;  // non-stretched axes take the child's desired size
    i32 z = 0;               // drawing (and hit) order among siblings
    // Boxes, Grid, Overlay, single-child panels.
    Margin padding;
    f32 fill = 0.0f; // along a box: 0 = its desired size; > 0 = a share of what's left
    HAlign h_align = HAlign::Fill;
    VAlign v_align = VAlign::Fill;
    // Grid.
    u32 row = 0, column = 0, row_span = 1, column_span = 1;
};

struct LayoutContext {
    const Font* font = nullptr;
};

struct PaintContext {
    const Font* font = nullptr;
    f32 opacity = 1.0f;
};

// A node in the UI tree. Layout is two passes: Measure (children first) for
// desired sizes, then Arrange (parents first) for rectangles. Geometry is in
// layout units.
class Widget {
public:
    virtual ~Widget() = default;
    virtual const char* TypeName() const = 0;

    std::string name;
    Visibility visibility = Visibility::Visible;
    f32 opacity = 1.0f;
    bool clip_children = false;
    Slot slot;

    // --- The tree -----------------------------------------------------------------
    Widget* Parent() const { return parent_; }
    usize ChildCount() const { return children_.size(); }
    Widget* Child(usize i) const { return i < children_.size() ? children_[i].get() : nullptr; }
    // How many children it takes (-1: any).
    virtual i32 MaxChildren() const { return 0; }
    // Adds a child (nullptr back when it's full).
    Widget* AddChild(std::unique_ptr<Widget> child);
    template <typename T, typename... Args>
    T* Add(Args&&... args) {
        return static_cast<T*>(AddChild(std::make_unique<T>(std::forward<Args>(args)...)));
    }
    std::unique_ptr<Widget> RemoveChild(Widget* child);
    // This widget or a descendant, by name.
    Widget* Find(const std::string& name);
    template <typename T>
    T* FindAs(const std::string& n) {
        return dynamic_cast<T*>(Find(n));
    }

    // --- Layout -------------------------------------------------------------------
    Vec2 Measure(const LayoutContext& ctx);
    void Arrange(const Rect& rect, const LayoutContext& ctx);
    Vec2 DesiredSize() const { return desired_; }
    const Rect& Geometry() const { return geometry_; }
    bool TakesSpace() const { return visibility != Visibility::Collapsed; }

    // --- Painting and hit testing -----------------------------------------------------
    void Paint(DrawList& list, const PaintContext& ctx) const;
    // The deepest widget under `p` that can be hit (children in front first).
    Widget* HitTest(Vec2 p);

protected:
    virtual Vec2 ComputeDesired(const LayoutContext& ctx);             // default: the largest child plus its padding
    virtual void ArrangeChildren(const LayoutContext& ctx);            // default: each child fills, less its padding and by its alignment
    virtual void PaintSelf(DrawList&, const PaintContext&) const {}    // before the children
    virtual void PaintOver(DrawList&, const PaintContext&) const {}    // after them (scroll bars)
    virtual bool HitsSelf() const { return false; }                    // panels let clicks through; controls don't
    virtual std::vector<Widget*> PaintOrder() const;                    // front last
    // Places `child` in `cell` by its padding and alignment.
    static Rect Align(const Widget& child, const Rect& cell);

    std::vector<std::unique_ptr<Widget>> children_;

private:
    Widget* parent_ = nullptr;
    Vec2 desired_;
    Rect geometry_;
};

} // namespace aether::ui
