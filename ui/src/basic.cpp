#include "aether/ui/basic.h"

#include "aether/loc/localization.h"

namespace aether::ui {

namespace {
const Font* Pick(const std::string& name, const Font* fallback, const FontLibrary* fonts) {
    if (!name.empty() && fonts != nullptr) {
        if (const Font* f = fonts->Find(name)) return f;
    }
    return fallback;
}
} // namespace

std::string Text::Shown() const {
    const loc::Localization* l = text_key.empty() ? nullptr : loc::Localization::Active();
    return l ? l->Text(text_key, text) : text;
}

Vec2 Text::ComputeDesired(const LayoutContext& ctx) {
    const Font* f = Pick(font, ctx.font, ctx.fonts);
    if (f == nullptr) return {};
    return LayoutText(*f, Shown(), size, wrap_width).size;
}

void Text::PaintSelf(DrawList& list, const PaintContext& ctx) const {
    const Font* f = Pick(font, ctx.font, ctx.fonts);
    TextAlign align = justify;
    if (align == TextAlign::Start) align = ctx.direction == FlowDirection::RightToLeft ? TextAlign::Right : TextAlign::Left;
    else if (align == TextAlign::End) align = ctx.direction == FlowDirection::RightToLeft ? TextAlign::Left : TextAlign::Right;
    if (f != nullptr) list.AddText(*f, Shown(), size, Geometry(), color.WithAlpha(ctx.opacity), align, wrap_width, effects.Any() ? &effects : nullptr);
}

} // namespace aether::ui
