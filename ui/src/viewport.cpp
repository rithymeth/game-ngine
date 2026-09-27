#include "aether/ui/viewport.h"

#include <algorithm>
#include <cmath>

namespace aether::ui {

f32 ScaleSettings::ScaleFor(Vec2 s) const {
    f32 k = 1.0f;
    const f32 rw = std::max(reference.x, 1.0f), rh = std::max(reference.y, 1.0f);
    switch (rule) {
    case Rule::None: k = 1.0f; break;
    case Rule::ShortestSide: k = std::min(s.x, s.y) / std::min(rw, rh); break;
    case Rule::LongestSide: k = std::max(s.x, s.y) / std::max(rw, rh); break;
    case Rule::Width: k = s.x / rw; break;
    case Rule::Height: k = s.y / rh; break;
    case Rule::Curve: {
        const f32 side = std::min(s.x, s.y);
        if (curve.empty()) break;
        if (side <= curve.front().first) {
            k = curve.front().second;
        } else if (side >= curve.back().first) {
            k = curve.back().second;
        } else {
            for (usize i = 1; i < curve.size(); ++i) {
                if (side <= curve[i].first) {
                    const auto& [x0, y0] = curve[i - 1];
                    const auto& [x1, y1] = curve[i];
                    k = y0 + (y1 - y0) * (side - x0) / std::max(x1 - x0, 1e-6f);
                    break;
                }
            }
        }
        break;
    }
    }
    return std::clamp(k, min_scale, max_scale);
}

Vec2 Viewport::LayoutSize() const {
    const f32 k = Scale();
    return {size_.x / k, size_.y / k};
}

Rect Viewport::SafeRect() const {
    const f32 k = Scale();
    return {safe_.left / k, safe_.top / k, std::max(0.0f, (size_.x - safe_.Horizontal()) / k), std::max(0.0f, (size_.y - safe_.Vertical()) / k)};
}

Widget* Viewport::Add(std::unique_ptr<Widget> root, i32 z) {
    if (!root) return nullptr;
    Widget* w = root.get();
    // Kept in z order; equal z in the order added.
    auto at = std::upper_bound(layers_.begin(), layers_.end(), z, [](i32 value, const LayerEntry& e) { return value < e.z; });
    layers_.insert(at, {std::move(root), z});
    return w;
}

std::unique_ptr<Widget> Viewport::Remove(Widget* root) {
    for (auto it = layers_.begin(); it != layers_.end(); ++it) {
        if (it->widget.get() == root) {
            std::unique_ptr<Widget> out = std::move(it->widget);
            layers_.erase(it);
            return out;
        }
    }
    return nullptr;
}

Widget* Viewport::Find(const std::string& name) const {
    for (const LayerEntry& e : layers_) {
        if (Widget* w = e.widget->Find(name)) return w;
    }
    return nullptr;
}

void Viewport::Layout(const Font& font) {
    LayoutContext ctx{&font, fonts_};
    const Rect area = SafeRect();
    for (LayerEntry& e : layers_) {
        e.widget->Measure(ctx);
        e.widget->Arrange(area, ctx);
    }
}

DrawList Viewport::Paint(const Font& font) const {
    DrawList list;
    PaintContext ctx{&font, 1.0f, fonts_};
    for (const LayerEntry& e : layers_) e.widget->Paint(list, ctx);
    list.Scale(Scale());
    return list;
}

Widget* Viewport::HitTest(Vec2 pixel) const {
    const f32 k = Scale();
    const Vec2 p{pixel.x / k, pixel.y / k};
    for (auto it = layers_.rbegin(); it != layers_.rend(); ++it) {
        if (Widget* w = it->widget->HitTest(p)) return w;
    }
    return nullptr;
}

} // namespace aether::ui
