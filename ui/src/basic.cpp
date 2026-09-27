#include "aether/ui/basic.h"

namespace aether::ui {

namespace {
const Font* Pick(const std::string& name, const Font* fallback, const FontLibrary* fonts) {
    if (!name.empty() && fonts != nullptr) {
        if (const Font* f = fonts->Find(name)) return f;
    }
    return fallback;
}
} // namespace

Vec2 Text::ComputeDesired(const LayoutContext& ctx) {
    const Font* f = Pick(font, ctx.font, ctx.fonts);
    if (f == nullptr) return {};
    return LayoutText(*f, text, size, wrap_width).size;
}

void Text::PaintSelf(DrawList& list, const PaintContext& ctx) const {
    const Font* f = Pick(font, ctx.font, ctx.fonts);
    if (f != nullptr) list.AddText(*f, text, size, Geometry(), color.WithAlpha(ctx.opacity), justify, wrap_width, effects.Any() ? &effects : nullptr);
}

} // namespace aether::ui
