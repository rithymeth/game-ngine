#pragma once

#include "aether/ui/draw.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace aether::ui {

class UIInputRouter;

// Input as widgets see it (Phase 18 step 2). Positions are in layout units.
struct PointerEvent {
    Vec2 position;
    int button = 0;   // 0 left (or a touch), 1 right, 2 middle
    u32 pointer = 0;  // which finger, for touch
};
enum class NavDirection : u8 { Up, Down, Left, Right, Next, Previous };
enum class EditKey : u8 { Backspace, Delete, Left, Right, Home, End, SelectAll };

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
    std::string tooltip; // shown after hovering a moment
    std::string style_class; // the theme's "Type.class" style to use ("" = the type's own)
    bool enabled = true; // disabled widgets (and everything in them) ignore input and draw dimmed
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

    // --- Input (step 2) ---------------------------------------------------------------
    // Explicit focus moves by widget name ("" = pick spatially).
    struct Navigation {
        std::string up, down, left, right;
    } navigation;
    bool IsHovered() const { return hovered_; }
    bool IsFocused() const { return focused_; }
    bool IsEnabled() const; // itself and every parent
    // A control: the router sends it pointer events (hits inside go to the nearest one).
    virtual bool Interactive() const { return false; }
    virtual bool Focusable() const { return false; }
    virtual bool WantsText() const { return false; } // a text box: Space and Left/Right edit it

    // Handlers return true when they used the input. Pointer down returning
    // true captures the pointer until it's released.
    virtual bool OnPointerDown(const PointerEvent&, UIInputRouter&) { return false; }
    virtual void OnPointerMove(const PointerEvent&, UIInputRouter&) {}
    virtual void OnPointerUp(const PointerEvent&, bool /*inside*/, UIInputRouter&) {}
    virtual bool OnWheel(f32, UIInputRouter&) { return false; } // bubbles to parents
    virtual bool OnNavigate(NavDirection, UIInputRouter&) { return false; } // false: the router moves focus
    virtual bool OnAccept(UIInputRouter&) { return false; }
    virtual bool OnCancel(UIInputRouter&) { return false; } // bubbles to parents
    virtual bool OnText(std::string_view, UIInputRouter&) { return false; }
    virtual bool OnKey(EditKey, bool, UIInputRouter&) { return false; } // (key, shift)
    virtual void OnHover(bool) {}
    virtual void OnFocus(bool) {}

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
    friend class UIInputRouter;
    Widget* parent_ = nullptr;
    bool hovered_ = false, focused_ = false;
    Vec2 desired_;
    Rect geometry_;
};

} // namespace aether::ui
