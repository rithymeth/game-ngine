#pragma once

#include "aether/ui/widget.h"

#include <string>

namespace aether::ui {

// Text in the layout's font (or `font`, by name, from the viewport's font
// library): its desired size is the text's, broken at '\n' and, with
// wrap_width, between words. `effects` draws an outline and a shadow.
class Text final : public Widget {
public:
    explicit Text(std::string text = {}) : text(std::move(text)) {}
    const char* TypeName() const override { return "Text"; }
    std::string text;
    // A string table key (Phase 29, §29.3). With one, the text shown is the
    // key's translation in the current language (Localization::Active()),
    // `text` being the source text it falls back to; with none, `text` is shown
    // as it is. Setting `text` at runtime (UI.SetText, a binding) clears the key.
    std::string text_key;
    std::string Shown() const;
    f32 size = 24.0f;
    Color color;
    TextAlign justify = TextAlign::Left;
    f32 wrap_width = 0.0f;
    std::string font; // "" = the default
    TextEffects effects;

protected:
    Vec2 ComputeDesired(const LayoutContext& ctx) override;
    void PaintSelf(DrawList& list, const PaintContext& ctx) const override;
};

// A brush: its desired size is `desired_size`, else the brush's image size.
class Image final : public Widget {
public:
    explicit Image(Brush brush = Brush::Solid({})) : brush(brush) {}
    const char* TypeName() const override { return "Image"; }
    Brush brush;
    Vec2 desired_size;
    bool hit_test = true;

protected:
    Vec2 ComputeDesired(const LayoutContext&) override { return desired_size.x > 0.0f || desired_size.y > 0.0f ? desired_size : brush.image_size; }
    void PaintSelf(DrawList& list, const PaintContext& ctx) const override { list.AddBrush(Geometry(), brush, ctx.opacity); }
    bool HitsSelf() const override { return hit_test; }
};

} // namespace aether::ui
