#include "aether/ui/draw.h"

#include "aether/ui/bidi.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace aether::ui {

f32 SdfCoverage(f32 d, const DrawQuad& q) {
    if (q.sdf_range <= 0.0f) return 1.0f;
    const f32 w = 1.0f / q.sdf_range + q.sdf_softness;
    return std::clamp((d - q.sdf_edge) / w + 0.5f, 0.0f, 1.0f);
}

void FontLibrary::Add(const std::string& name, const Font* font) {
    chains_.clear();
    for (auto& [n, f] : fonts_) {
        if (n == name) {
            f = font;
            return;
        }
    }
    fonts_.emplace_back(name, font);
}

void FontLibrary::Remove(const std::string& name) {
    chains_.clear();
    std::erase_if(fonts_, [&](const auto& e) { return e.first == name; });
}

const Font* FontLibrary::Find(const std::string& name) const {
    for (const auto& [n, f] : fonts_) {
        if (n == name) return f;
    }
    if (name.find(',') == std::string::npos) return nullptr;
    for (const auto& [n, chain] : chains_) {
        if (n == name) return chain.get();
    }
    std::vector<const Font*> found;
    for (usize start = 0; start <= name.size();) {
        usize end = name.find(',', start);
        if (end == std::string::npos) end = name.size();
        std::string part = name.substr(start, end - start);
        const usize a = part.find_first_not_of(' '), b = part.find_last_not_of(' ');
        part = a == std::string::npos ? std::string() : part.substr(a, b - a + 1);
        for (const auto& [n, f] : fonts_) {
            if (n == part) {
                found.push_back(f);
                break;
            }
        }
        start = end + 1;
    }
    if (found.empty()) return nullptr;
    chains_.emplace_back(name, std::make_unique<FallbackFont>(std::move(found)));
    return chains_.back().second.get();
}

f32 FallbackFont::Kerning(u32 a, u32 b, f32 size) const {
    const Font& fa = Source(a);
    return &fa == &Source(b) ? fa.Kerning(a, b, size) : 0.0f; // only within one font
}

bool FallbackFont::HasGlyph(u32 codepoint) const {
    for (const Font* f : fonts_) {
        if (f->HasGlyph(codepoint)) return true;
    }
    return false;
}

const Font& FallbackFont::Source(u32 codepoint) const {
    for (const Font* f : fonts_) {
        if (f->HasGlyph(codepoint)) return f->Source(codepoint);
    }
    return fonts_.empty() ? static_cast<const Font&>(*this) : fonts_[0]->Source(codepoint); // tofu from the first
}

std::vector<std::string> FontLibrary::Names() const {
    std::vector<std::string> out;
    for (const auto& e : fonts_) out.push_back(e.first);
    return out;
}

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
    const int length = extra;
    u32 cp = c & (0x3F >> extra);
    for (; extra > 0; --extra) {
        if (i >= s.size() || (byte(i) & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (byte(i++) & 0x3F);
    }
    // Not the shortest form, a surrogate, or past U+10FFFF: not text.
    const u32 shortest = length == 1 ? 0x80u : length == 2 ? 0x800u : 0x10000u;
    if (cp < shortest || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) return 0xFFFD;
    return cp;
}

namespace {

// The code points of the line text[a, b) in the order they are drawn (right-to-left runs reversed, §29.6).
std::vector<u32> LineInVisualOrder(std::string_view text, usize a, usize b) {
    std::vector<u32> cps;
    for (usize i = a; i < b;) cps.push_back(DecodeUtf8(text, i));
    return ReorderVisual(cps);
}

} // namespace

TextLayout LayoutText(const Font& font, std::string_view text, f32 size, f32 wrap_width) {
    TextLayout out;
    auto width_of = [&](usize a, usize b) {
        f32 w = 0.0f;
        u32 prev = 0;
        for (const u32 cp : LineInVisualOrder(text, a, b)) {
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

Rect DrawList::Transform(const Rect& r) const {
    const Affine& t = transforms_.back();
    return {r.x * t.scale.x + t.translate.x, r.y * t.scale.y + t.translate.y, r.w * t.scale.x, r.h * t.scale.y};
}

void DrawList::PushTransform(Vec2 offset, Vec2 scale, Vec2 pivot) {
    // Local: p' = pivot + (p - pivot) * scale + offset; then the current transform on top.
    const Affine& cur = transforms_.back();
    const Vec2 t{pivot.x * (1.0f - scale.x) + offset.x, pivot.y * (1.0f - scale.y) + offset.y};
    transforms_.push_back({{scale.x * cur.scale.x, scale.y * cur.scale.y}, {t.x * cur.scale.x + cur.translate.x, t.y * cur.scale.y + cur.translate.y}});
}

void DrawList::PopTransform() {
    if (transforms_.size() > 1) transforms_.pop_back();
}

void DrawList::PushClip(const Rect& local) {
    const Rect rect = Transform(local);
    clips.push_back(clips[stack_.back()].Intersect(rect));
    stack_.push_back(static_cast<u32>(clips.size() - 1));
}

void DrawList::PopClip() {
    if (stack_.size() > 1) stack_.pop_back();
}

void DrawList::AddQuad(const Rect& local, Color color, u32 texture, const Rect& uv) {
    const Rect rect = Transform(local);
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

void DrawList::AddSdfQuad(const Rect& local, Color color, u32 texture, const Rect& uv, f32 range, f32 edge, f32 softness) {
    const usize before = quads.size();
    AddQuad(local, color, texture, uv);
    if (quads.size() == before) return;
    // The field's span in the current units: the transform's scale grows it with the glyph.
    const Affine& t = transforms_.back();
    DrawQuad& q = quads.back();
    q.sdf_range = range * std::min(std::abs(t.scale.x), std::abs(t.scale.y));
    q.sdf_edge = edge;
    q.sdf_softness = softness;
}

void DrawList::AddText(const Font& font, std::string_view text, f32 size, const Rect& box, Color color, TextAlign align, f32 wrap_width,
                       const TextEffects* effects) {
    const TextLayout layout = LayoutText(font, text, size, wrap_width);
    // One pass over the glyphs per layer (shadow, outline, fill), so an
    // outline never covers the neighbouring glyph's fill.
    auto pass = [&](Vec2 offset, Color c, f32 grow, f32 softness) {
        if (c.a <= 0.0f) return;
        f32 y = box.y + font.Ascent(size) + offset.y;
        for (const TextLine& line : layout.lines) {
            f32 x = box.x + offset.x;
            if (align == TextAlign::Center) x += (box.w - line.width) * 0.5f;
            else if (align == TextAlign::Right) x += box.w - line.width;
            u32 prev = 0;
            for (const u32 cp : LineInVisualOrder(text, line.begin, line.end)) {
                if (prev != 0) x += font.Kerning(prev, cp, size);
                const Font& src = font.Source(cp); // a fallback chain draws each glyph from the font that has it
                const Glyph g = src.GlyphOf(cp, size);
                const f32 range = src.SdfRange(size), edge = src.SdfEdge();
                const Rect r{x + g.quad.x, y + g.quad.y, g.quad.w, g.quad.h};
                if (range > 0.0f) {
                    // Grow the shape by moving the edge down the field (it can't go past the field's end).
                    const f32 e = std::max(edge - grow / range, 0.02f);
                    AddSdfQuad(r, c, src.Texture(), g.uv, range, e, softness / range);
                } else if (grow > 0.0f) {
                    for (const Vec2 d : {Vec2{-grow, 0}, Vec2{grow, 0}, Vec2{0, -grow}, Vec2{0, grow}}) {
                        AddQuad({r.x + d.x, r.y + d.y, r.w, r.h}, c, src.Texture(), g.uv);
                    }
                } else {
                    AddQuad(r, c, src.Texture(), g.uv);
                }
                x += g.advance;
                prev = cp;
            }
            y += font.LineHeight(size);
        }
    };
    if (effects != nullptr) {
        const Color shadow = effects->shadow_color.WithAlpha(color.a);
        pass(effects->shadow_offset, shadow, effects->outline, effects->shadow_softness);
        if (effects->outline > 0.0f) pass({}, effects->outline_color.WithAlpha(color.a), effects->outline, 0.0f);
    }
    pass({}, color, 0.0f, 0.0f);
}

void DrawList::Scale(f32 s) {
    for (DrawQuad& q : quads) {
        q.rect = {q.rect.x * s, q.rect.y * s, q.rect.w * s, q.rect.h * s};
        q.sdf_range *= s;
    }
    for (usize i = 1; i < clips.size(); ++i) clips[i] = {clips[i].x * s, clips[i].y * s, clips[i].w * s, clips[i].h * s};
}

} // namespace aether::ui
