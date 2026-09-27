#include "aether/ui/font.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <unordered_map>

namespace aether::ui {

namespace {
// stb_truetype's field: 128 on the edge, rising 128 / padding per pixel inside.
constexpr u8 kOnEdge = 128;
} // namespace

struct SdfFont::Entry {
    f32 advance = 0.0f;        // at base size
    i32 x = 0, y = 0, w = 0, h = 0; // atlas pixels (w = 0: nothing to draw)
    i32 xoff = 0, yoff = 0;    // from the pen at the baseline, base-size pixels
};

struct SdfFont::Impl {
    std::vector<u8> data;
    stbtt_fontinfo info{};
    f32 scale = 1.0f; // font units to base-size pixels
    f32 ascent = 0.0f, descent = 0.0f, line_gap = 0.0f;
    std::unordered_map<u32, Entry> glyphs;
};

std::unique_ptr<SdfFont> SdfFont::FromMemory(std::vector<u8> ttf, const SdfFontSettings& settings, std::string* error) {
    auto fail = [&](const char* why) -> std::unique_ptr<SdfFont> {
        if (error != nullptr) *error = why;
        return nullptr;
    };
    if (ttf.size() < 12) return fail("not a font: too small");
    const u32 tag = (u32(ttf[0]) << 24) | (u32(ttf[1]) << 16) | (u32(ttf[2]) << 8) | u32(ttf[3]);
    // TrueType 1.0, 'true', 'OTTO' (CFF outlines) and collections.
    if (tag != 0x00010000u && tag != 0x74727565u && tag != 0x4F54544Fu && tag != 0x74746366u) return fail("not a TrueType or OpenType font");
    if (settings.base_size < 4.0f || settings.padding < 1 || settings.atlas_width < 16 || settings.atlas_height < 16) {
        return fail("bad settings: base size, padding or atlas size too small");
    }
    std::unique_ptr<SdfFont> font(new SdfFont());
    font->impl_ = std::make_unique<Impl>();
    Impl& im = *font->impl_;
    im.data = std::move(ttf);
    const int offset = stbtt_GetFontOffsetForIndex(im.data.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&im.info, im.data.data(), offset)) return fail("the font's tables couldn't be read");
    im.scale = stbtt_ScaleForPixelHeight(&im.info, settings.base_size);
    int a = 0, d = 0, g = 0;
    stbtt_GetFontVMetrics(&im.info, &a, &d, &g);
    im.ascent = static_cast<f32>(a) * im.scale;
    im.descent = static_cast<f32>(d) * im.scale;
    im.line_gap = static_cast<f32>(g) * im.scale;
    font->settings_ = settings;
    font->width_ = settings.atlas_width;
    font->height_ = settings.atlas_height;
    font->pixels_.assign(static_cast<usize>(font->width_) * static_cast<usize>(font->height_), 0);
    font->resized_ = true; // a new texture
    font->dirty_ = {0, 0, font->width_, font->height_};
    return font;
}

std::unique_ptr<SdfFont> SdfFont::FromFile(const std::string& path, const SdfFontSettings& settings, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (error != nullptr) *error = "can't open '" + path + "'";
        return nullptr;
    }
    std::vector<u8> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return FromMemory(std::move(bytes), settings, error);
}

SdfFont::~SdfFont() = default;

f32 SdfFont::Ascent(f32 size) const { return impl_->ascent * size / settings_.base_size; }

f32 SdfFont::LineHeight(f32 size) const { return (impl_->ascent - impl_->descent + impl_->line_gap) * size / settings_.base_size; }

f32 SdfFont::SdfRange(f32 size) const {
    // The 0..1 field spans 255 / (128 / padding) atlas pixels, each size / base_size layout units.
    return 255.0f * static_cast<f32>(settings_.padding) / static_cast<f32>(kOnEdge) * size / settings_.base_size;
}

f32 SdfFont::SdfEdge() const { return static_cast<f32>(kOnEdge) / 255.0f; }

bool SdfFont::HasGlyph(u32 cp) const { return stbtt_FindGlyphIndex(&impl_->info, static_cast<int>(cp)) != 0; }

std::string SdfFont::FamilyName() const {
    int len = 0;
    const char* name = stbtt_GetFontNameString(&impl_->info, &len, STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP, STBTT_MS_LANG_ENGLISH, 1);
    std::string out;
    if (name == nullptr) return out;
    // UTF-16 big-endian; the family names we show are ASCII.
    for (int i = 0; i + 1 < len; i += 2) {
        const u32 c = (u32(u8(name[i])) << 8) | u32(u8(name[i + 1]));
        out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    }
    return out;
}

void SdfFont::MarkDirty(const AtlasRect& r) const {
    if (dirty_.Empty()) {
        dirty_ = r;
        return;
    }
    const i32 x0 = std::min(dirty_.x, r.x), y0 = std::min(dirty_.y, r.y);
    const i32 x1 = std::max(dirty_.x + dirty_.w, r.x + r.w), y1 = std::max(dirty_.y + dirty_.h, r.y + r.h);
    dirty_ = {x0, y0, x1 - x0, y1 - y0};
}

bool SdfFont::Pack(i32 w, i32 h, i32& x, i32& y) const {
    // Shelves: left to right along a row, then a new row under the tallest glyph so far.
    if (w + 2 > width_) return false;
    if (pen_x_ + w + 1 > width_) {
        pen_x_ = 1;
        pen_y_ += row_h_ + 1;
        row_h_ = 0;
    }
    while (pen_y_ + h + 1 > height_) {
        if (height_ * 2 > settings_.max_atlas_height) return false;
        height_ *= 2; // rows keep their place; only v coordinates change
        pixels_.resize(static_cast<usize>(width_) * static_cast<usize>(height_), 0);
        resized_ = true;
        dirty_ = {0, 0, width_, height_};
    }
    x = pen_x_;
    y = pen_y_;
    pen_x_ += w + 1;
    row_h_ = std::max(row_h_, h);
    return true;
}

const SdfFont::Entry& SdfFont::Get(u32 cp) const {
    Impl& im = *impl_;
    if (auto it = im.glyphs.find(cp); it != im.glyphs.end()) return it->second;
    if (cp < 0x20) {
        // Control characters (a tab) take a space's room and draw nothing.
        Entry space = Get(' ');
        space.w = 0;
        return im.glyphs.emplace(cp, space).first->second;
    }
    int index = stbtt_FindGlyphIndex(&im.info, static_cast<int>(cp));
    if (index == 0 && cp != 0xFFFD && cp != '?') {
        // Missing: the replacement character's glyph, else a question mark.
        const Entry& fallback = Get(HasGlyph(0xFFFD) ? 0xFFFD : '?');
        return im.glyphs.emplace(cp, fallback).first->second;
    }
    Entry e;
    int advance = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&im.info, index, &advance, &lsb);
    e.advance = static_cast<f32>(advance) * im.scale;
    int w = 0, h = 0, xoff = 0, yoff = 0;
    const f32 per_pixel = static_cast<f32>(kOnEdge) / static_cast<f32>(settings_.padding);
    unsigned char* sdf = stbtt_GetGlyphSDF(&im.info, im.scale, index, settings_.padding, kOnEdge, per_pixel, &w, &h, &xoff, &yoff);
    if (sdf != nullptr && w > 0 && h > 0) {
        i32 x = 0, y = 0;
        if (Pack(w, h, x, y)) {
            for (int row = 0; row < h; ++row) {
                std::memcpy(&pixels_[static_cast<usize>(y + row) * static_cast<usize>(width_) + static_cast<usize>(x)], sdf + row * w, static_cast<usize>(w));
            }
            e.x = x, e.y = y, e.w = w, e.h = h, e.xoff = xoff, e.yoff = yoff;
            MarkDirty({x, y, w, h});
        } else {
            char code[16];
            std::snprintf(code, sizeof code, "U+%04X", cp);
            problems_.push_back(std::string("glyph ") + code + " doesn't fit in the font atlas");
        }
    }
    if (sdf != nullptr) stbtt_FreeSDF(sdf, nullptr);
    return im.glyphs.emplace(cp, e).first->second;
}

Glyph SdfFont::GlyphOf(u32 cp, f32 size) const {
    const Entry& e = Get(cp);
    const f32 k = size / settings_.base_size;
    Glyph g;
    g.advance = e.advance * k;
    if (e.w > 0) {
        g.quad = {static_cast<f32>(e.xoff) * k, static_cast<f32>(e.yoff) * k, static_cast<f32>(e.w) * k, static_cast<f32>(e.h) * k};
        const f32 iw = 1.0f / static_cast<f32>(width_), ih = 1.0f / static_cast<f32>(height_);
        g.uv = {static_cast<f32>(e.x) * iw, static_cast<f32>(e.y) * ih, static_cast<f32>(e.w) * iw, static_cast<f32>(e.h) * ih};
    }
    return g;
}

f32 SdfFont::Kerning(u32 a, u32 b, f32 size) const {
    const int k = stbtt_GetCodepointKernAdvance(&impl_->info, static_cast<int>(a), static_cast<int>(b));
    return static_cast<f32>(k) * impl_->scale * size / settings_.base_size;
}

void SdfFont::Prewarm(std::string_view text) const {
    for (usize i = 0; i < text.size();) (void)Get(DecodeUtf8(text, i));
}

usize SdfFont::GlyphCount() const { return impl_->glyphs.size(); }

f32 SdfFont::SampleDistance(Vec2 uv) const {
    const f32 fx = uv.x * static_cast<f32>(width_) - 0.5f, fy = uv.y * static_cast<f32>(height_) - 0.5f;
    const i32 x0 = static_cast<i32>(std::floor(fx)), y0 = static_cast<i32>(std::floor(fy));
    const f32 tx = fx - static_cast<f32>(x0), ty = fy - static_cast<f32>(y0);
    auto at = [&](i32 x, i32 y) {
        x = std::clamp(x, 0, width_ - 1);
        y = std::clamp(y, 0, height_ - 1);
        return static_cast<f32>(pixels_[static_cast<usize>(y) * static_cast<usize>(width_) + static_cast<usize>(x)]) / 255.0f;
    };
    const f32 top = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * tx;
    const f32 bottom = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * tx;
    return top + (bottom - top) * ty;
}

} // namespace aether::ui
