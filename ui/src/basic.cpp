#include "aether/ui/basic.h"

namespace aether::ui {

Vec2 Text::ComputeDesired(const LayoutContext& ctx) {
    if (ctx.font == nullptr) return {};
    return LayoutText(*ctx.font, text, size, wrap_width).size;
}

void Text::PaintSelf(DrawList& list, const PaintContext& ctx) const {
    if (ctx.font != nullptr) list.AddText(*ctx.font, text, size, Geometry(), color.WithAlpha(ctx.opacity), justify, wrap_width);
}

} // namespace aether::ui
