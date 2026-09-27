#include "aether/ui/draw.h"

#include <cmath>
#include <limits>

namespace aether::ui {

Glyph BuiltinFont::GlyphOf(u32 codepoint, f32 size) const {
    Glyph g;
    g.advance = 0.6f * size;
    if (codepoint != ' ' && codepoint != '\t') g.quad = {0.05f * size, -0.7f * size, 0.5f * size, 0.7f * size};
    return g;
}

u32 DecodeUtf8(std::string_view s, usize& i) {
    const auto byte = [&](usize k) { return static_cast<u8>(s[k]); };
    const u8 c = byte(i++);
    if (c < 0x80) return c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
    if (extra < 0 || c >= 0xF8) return 0xFFFD;
    u32 cp = c & (0x3F >> extra);
    for (; extra > 0; --extra) {
        if (i >= s.size() || (byte(i) & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (byte(i++) & 0x3F);
    }
    return cp;
}

TextLayout LayoutText(const Font& font, std::string_view text, f32 size, f32 wrap_width) {
    TextLayout out;
    auto width_of = [&](usize a, usize b) {
        f32 w = 0.0f;
        u32 prev = 0;
        for (usize i = a; i < b;) {
            const u32 cp = DecodeUtf8(text, i);
            if (prev != 0) w += font.Kerning(prev, cp, size);
            w += font.GlyphOf(cp, size).advance;
            prev = cp;
        }
        return w;
    };
    usize line_start = 0;
    while (true) {
        const usize nl = text.find('\n', line_start);
        const usize para_end = nl == std::string_view::npos ? text.size() : nl;
        if (wrap_width <= 0.0f) {
            out.lines.push_back({line_start, para_end, width_of(line_start, para_end)});
        } else {
            // Greedy word wrap: take words while the line fits; a word too long for any line gets one of its own.
            usize begin = line_start;
            while (true) {
                usize end = begin, scan = begin;
                while (scan < para_end) {
                    const usize space = text.find(' ', scan);
                    const usize word_end = space == std::string_view::npos || space > para_end ? para_end : space;
                    if (end > begin && width_of(begin, word_end) > wrap_width) break;
                    end = word_end;
                    scan = word_end < para_end ? word_end + 1 : para_end;
                    if (width_of(begin, end) > wrap_width) break;
                }
                out.lines.push_back({begin, end, width_of(begin, end)});
                begin = end;
                while (begin < para_end && text[begin] == ' ') ++begin; // spaces at a break vanish
                if (begin >= para_end) break;
            }
        }
        if (nl == std::string_view::npos) break;
        line_start = nl + 1;
    }
    for (const TextLine& l : out.lines) out.size.x = std::max(out.size.x, l.width);
    out.size.y = font.LineHeight(size) * static_cast<f32>(out.lines.size());
    return out;
}

DrawList::DrawList() {
    constexpr f32 big = std::numeric_limits<f32>::max() / 4;
    clips.push_back({-big, -big, 2 * big, 2 * big});
    stack_.push_back(0);
}

void DrawList::PushClip(const Rect& rect) {
    clips.push_back(clips[stack_.back()].Intersect(rect));
    stack_.push_back(static_cast<u32>(clips.size() - 1));
}

void DrawList::PopClip() {
    if (stack_.size() > 1) stack_.pop_back();
}

void DrawList::AddQuad(const Rect& rect, Color color, u32 texture, const Rect& uv) {
    if (rect.Empty() || color.a <= 0.0f) return;
    if (clips[stack_.back()].Intersect(rect).Empty()) return;
    quads.push_back({rect, uv, color, texture, stack_.back()});
}

void DrawList::AddBrush(const Rect& r, const Brush& b, f32 opacity) {
    const Color c = b.tint.WithAlpha(opacity);
    switch (b.kind) {
    case Brush::Kind::None: return;
    case Brush::Kind::Color: AddQuad(r, c, 0); return;
    case Brush::Kind::Image: AddQuad(r, c, b.texture, b.uv); return;
    case Brush::Kind::Box:
    case Brush::Kind::Frame: {
        // Nine pieces: corner sizes from the slice margins (shrunk if the rectangle is too small).
        const f32 tw = std::max(b.image_size.x, 1.0f), th = std::max(b.image_size.y, 1.0f);
        f32 l = b.slice.left * b.slice_scale, rr = b.slice.right * b.slice_scale, t = b.slice.top * b.slice_scale, bb = b.slice.bottom * b.slice_scale;
        if (l + rr > r.w && l + rr > 0.0f) {
            const f32 k = r.w / (l + rr);
            l *= k, rr *= k;
        }
        if (t + bb > r.h && t + bb > 0.0f) {
            const f32 k = r.h / (t + bb);
            t *= k, bb *= k;
        }
        const f32 xs[4] = {r.x, r.x + l, r.Right() - rr, r.Right()};
        const f32 ys[4] = {r.y, r.y + t, r.Bottom() - bb, r.Bottom()};
        const f32 us[4] = {b.uv.x, b.uv.x + b.slice.left / tw * b.uv.w, b.uv.x + (1.0f - b.slice.right / tw) * b.uv.w, b.uv.x + b.uv.w};
        const f32 vs[4] = {b.uv.y, b.uv.y + b.slice.top / th * b.uv.h, b.uv.y + (1.0f - b.slice.bottom / th) * b.uv.h, b.uv.y + b.uv.h};
        for (int yi = 0; yi < 3; ++yi) {
            for (int xi = 0; xi < 3; ++xi) {
                if (b.kind == Brush::Kind::Frame && xi == 1 && yi == 1) continue;
                AddQuad({xs[xi], ys[yi], xs[xi + 1] - xs[xi], ys[yi + 1] - ys[yi]}, c, b.texture, {us[xi], vs[yi], us[xi + 1] - us[xi], vs[yi + 1] - vs[yi]});
            }
        }
        return;
    }
    }
}

void DrawList::AddText(const Font& font, std::string_view text, f32 size, const Rect& box, Color color, TextAlign align, f32 wrap_width) {
    const TextLayout layout = LayoutText(font, text, size, wrap_width);
    f32 y = box.y + font.Ascent(size);
    for (const TextLine& line : layout.lines) {
        f32 x = box.x;
        if (align == TextAlign::Center) x += (box.w - line.width) * 0.5f;
        else if (align == TextAlign::Right) x += box.w - line.width;
        u32 prev = 0;
        for (usize i = line.begin; i < line.end;) {
            const u32 cp = DecodeUtf8(text, i);
            if (prev != 0) x += font.Kerning(prev, cp, size);
            const Glyph g = font.GlyphOf(cp, size);
            AddQuad({x + g.quad.x, y + g.quad.y, g.quad.w, g.quad.h}, color, font.Texture(), g.uv);
            x += g.advance;
            prev = cp;
        }
        y += font.LineHeight(size);
    }
}

void DrawList::Scale(f32 s) {
    for (DrawQuad& q : quads) q.rect = {q.rect.x * s, q.rect.y * s, q.rect.w * s, q.rect.h * s};
    for (usize i = 1; i < clips.size(); ++i) clips[i] = {clips[i].x * s, clips[i].y * s, clips[i].w * s, clips[i].h * s};
}

} // namespace aether::ui
