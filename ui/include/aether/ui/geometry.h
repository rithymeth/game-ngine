#pragma once

#include "aether/core/base.h"

#include <algorithm>

namespace aether::ui {

// UI geometry (Phase 18 step 1, docs/design/PHASE_SPECS.md §18.1). Layout
// works in layout units (the reference resolution's pixels); the viewport
// scales them to screen pixels. +x is right, +y is down.

struct Vec2 {
    f32 x = 0.0f, y = 0.0f;
    Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(f32 s) const { return {x * s, y * s}; }
    bool operator==(const Vec2& o) const { return x == o.x && y == o.y; }
};

struct Rect {
    f32 x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    f32 Right() const { return x + w; }
    f32 Bottom() const { return y + h; }
    bool Contains(Vec2 p) const { return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h; }
    bool Empty() const { return w <= 0.0f || h <= 0.0f; }
    Rect Intersect(const Rect& o) const {
        const f32 x0 = std::max(x, o.x), y0 = std::max(y, o.y);
        const f32 x1 = std::min(Right(), o.Right()), y1 = std::min(Bottom(), o.Bottom());
        return {x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
    }
    bool operator==(const Rect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};

struct Margin {
    f32 left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
    static Margin All(f32 v) { return {v, v, v, v}; }
    f32 Horizontal() const { return left + right; }
    f32 Vertical() const { return top + bottom; }
    Rect Shrink(const Rect& r) const { return {r.x + left, r.y + top, std::max(0.0f, r.w - left - right), std::max(0.0f, r.h - top - bottom)}; }
};

struct Color {
    f32 r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
    Color WithAlpha(f32 alpha) const { return {r, g, b, a * alpha}; }
    bool operator==(const Color& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
};

// Where a Canvas child's edges attach, as fractions of the canvas (0..1). A
// min equal to the max on an axis is a point; different, the child stretches.
struct Anchors {
    f32 min_x = 0.0f, min_y = 0.0f, max_x = 0.0f, max_y = 0.0f;
    static Anchors Point(f32 x, f32 y) { return {x, y, x, y}; }
    static Anchors Stretch() { return {0.0f, 0.0f, 1.0f, 1.0f}; }
    bool StretchX() const { return max_x > min_x; }
    bool StretchY() const { return max_y > min_y; }
};

enum class HAlign : u8 { Fill, Left, Center, Right };
enum class VAlign : u8 { Fill, Top, Center, Bottom };
// The direction text and horizontal layout run in (Phase 29 step 6, §29.6).
enum class FlowDirection : u8 { LeftToRight, RightToLeft };

} // namespace aether::ui
