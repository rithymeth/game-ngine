#pragma once

#include "aether/ui/widget.h"

#include <memory>
#include <utility>
#include <vector>

namespace aether::ui {

// How layout units map to screen pixels (Phase 18 step 1). Layouts are made
// for a reference resolution; the scale is the screen's size against it.
struct ScaleSettings {
    enum class Rule : u8 {
        None,         // 1 layout unit = 1 pixel
        ShortestSide, // the screen's shortest side against the reference's
        LongestSide,
        Width,
        Height,
        Curve, // `curve`: the screen's shortest side in pixels -> scale, piecewise linear
    };
    Rule rule = Rule::ShortestSide;
    Vec2 reference{1920.0f, 1080.0f};
    std::vector<std::pair<f32, f32>> curve;
    f32 min_scale = 0.1f, max_scale = 10.0f;
    f32 ScaleFor(Vec2 screen) const;
};

// The screen the UI draws on: widget layers (in z order), each filling the
// safe area; the scale; layout, painting (to pixels) and hit testing.
class Viewport {
public:
    ScaleSettings scale_settings;
    // Right-to-left layout (§29.6): the layers are laid out as usual, then mirrored
    // about the safe area's centre, so boxes run right to left, Left-aligned
    // children sit on the right, and so on. Text widgets resolve their Start / End
    // alignment against it.
    FlowDirection direction = FlowDirection::LeftToRight;

    void SetSize(Vec2 pixels) { size_ = pixels; }
    Vec2 Size() const { return size_; }
    // Insets in pixels where the screen can't show things (notches, TV overscan).
    void SetSafeArea(const Margin& pixels) { safe_ = pixels; }
    f32 Scale() const { return scale_settings.ScaleFor(size_); }
    Vec2 LayoutSize() const; // the screen in layout units
    Rect SafeRect() const;   // the safe area in layout units

    Widget* Add(std::unique_ptr<Widget> root, i32 z = 0);
    std::unique_ptr<Widget> Remove(Widget* root);
    usize LayerCount() const { return layers_.size(); }
    Widget* Layer(usize i) const { return i < layers_.size() ? layers_[i].widget.get() : nullptr; }
    Widget* Find(const std::string& name) const;

    // Named fonts for Text widgets that pick one (not owned).
    void SetFonts(const FontLibrary* fonts) { fonts_ = fonts; }
    const FontLibrary* Fonts() const { return fonts_; }

    void Layout(const Font& font);
    // Pixels (layout, then scaled).
    DrawList Paint(const Font& font) const;
    // The widget at a pixel (the top layer's first).
    Widget* HitTest(Vec2 pixel) const;

private:
    struct LayerEntry {
        std::unique_ptr<Widget> widget;
        i32 z = 0;
    };
    Vec2 size_{1920.0f, 1080.0f};
    Margin safe_;
    std::vector<LayerEntry> layers_;
    const FontLibrary* fonts_ = nullptr;
};

} // namespace aether::ui
