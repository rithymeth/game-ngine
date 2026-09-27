#pragma once

#include "aether/ui/geometry.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aether::ui {

// What a widget paints with.
struct Brush {
    enum class Kind : u8 {
        None,  // nothing
        Color, // a solid rectangle
        Image, // a texture stretched over the rectangle
        Box,   // a 9-slice: corners keep their size, edges stretch one way, the middle both
        Frame, // a 9-slice without its middle
    };
    Kind kind = Kind::None;
    Color tint;
    u32 texture = 0;       // the renderer's texture id (0 = white)
    std::string image;     // the texture's asset path, saved in themes and layouts (resolved to `texture` on load)
    Vec2 image_size;       // the texture's size in pixels (Image's natural size; 9-slice margins are in these pixels)
    Rect uv{0, 0, 1, 1};   // the part of the texture used
    Margin slice;          // 9-slice margins, in texture pixels
    f32 slice_scale = 1.0f; // how big the corners draw (layout units per texture pixel)

    static Brush Solid(Color c) {
        Brush b;
        b.kind = Kind::Color;
        b.tint = c;
        return b;
    }
};

// One textured, tinted rectangle, clipped by clips[clip].
//
// With `sdf_range` > 0 the texture is a signed distance field (a font atlas,
// step 5): its value d (0..1) is the distance to a glyph's edge, `sdf_edge`
// on the edge, and the whole 0..1 span covers `sdf_range` pixels on screen.
// The shader's coverage is SdfCoverage below: sharp at any size, and
// `sdf_softness` (in field units) blurs it for soft shadows.
struct DrawQuad {
    Rect rect;
    Rect uv{0, 0, 1, 1};
    Color color;
    u32 texture = 0;
    u32 clip = 0;
    f32 sdf_range = 0.0f;
    f32 sdf_edge = 0.5f;
    f32 sdf_softness = 0.0f;
};
// saturate((d - edge) / w + 0.5), w = 1 / range + softness: what the UI
// shader does per pixel, for tests and software drawing.
f32 SdfCoverage(f32 distance, const DrawQuad& quad);

// Text metrics and glyphs. Sizes are the font's pixel size in layout units.
struct Glyph {
    f32 advance = 0.0f;
    Rect quad; // relative to the pen at the baseline; empty for spaces
    Rect uv{0, 0, 1, 1};
};
class Font {
public:
    virtual ~Font() = default;
    virtual f32 Ascent(f32 size) const = 0;     // baseline below the line's top
    virtual f32 LineHeight(f32 size) const = 0; // line to line
    virtual Glyph GlyphOf(u32 codepoint, f32 size) const = 0;
    virtual f32 Kerning(u32, u32, f32) const { return 0.0f; }
    virtual u32 Texture() const { return 0; }
    // A distance-field font (SdfFont): how many layout units the field's
    // 0..1 span covers at `size`, and its edge value. 0: a plain bitmap.
    virtual f32 SdfRange(f32) const { return 0.0f; }
    virtual f32 SdfEdge() const { return 0.5f; }
};

// Named fonts ("Roboto", "Title"), for Text widgets and text styles that
// pick one. The library doesn't own them.
class FontLibrary {
public:
    void Add(const std::string& name, const Font* font);
    void Remove(const std::string& name);
    const Font* Find(const std::string& name) const; // null if unknown
    std::vector<std::string> Names() const;

private:
    std::vector<std::pair<std::string, const Font*>> fonts_;
};

// Outlines and drop shadows, drawn behind the text. With a distance-field
// font the outline is the glyphs' own shape grown by `outline` (up to the
// field's range) and the shadow can be soft; with others, offset copies.
struct TextEffects {
    f32 outline = 0.0f; // layout units
    Color outline_color{0.0f, 0.0f, 0.0f, 1.0f};
    Vec2 shadow_offset;
    Color shadow_color{0.0f, 0.0f, 0.0f, 0.0f}; // transparent: no shadow
    f32 shadow_softness = 0.0f;                 // layout units
    bool Any() const { return outline > 0.0f || shadow_color.a > 0.0f; }
    bool operator==(const TextEffects& o) const {
        return outline == o.outline && outline_color == o.outline_color && shadow_offset == o.shadow_offset && shadow_color == o.shadow_color &&
               shadow_softness == o.shadow_softness;
    }
};

// Fixed-width boxes, so layout is predictable (tests, and before fonts load). Advance 0.6 × size, line height 1.25 × size, ascent 0.95 × size.
class BuiltinFont final : public Font {
public:
    f32 Ascent(f32 size) const override { return 0.95f * size; }
    f32 LineHeight(f32 size) const override { return 1.25f * size; }
    Glyph GlyphOf(u32 codepoint, f32 size) const override;
};

// Text broken into lines.
struct TextLine {
    usize begin = 0, end = 0; // byte range in the text
    f32 width = 0.0f;
};
struct TextLayout {
    std::vector<TextLine> lines;
    Vec2 size;
};
// Lines at '\n' and, with `wrap_width` > 0, between words (a word too long
// for a line gets one of its own).
TextLayout LayoutText(const Font& font, std::string_view text, f32 size, f32 wrap_width = 0.0f);
// Next code point of UTF-8 text at `i` (invalid bytes read as U+FFFD).
u32 DecodeUtf8(std::string_view text, usize& i);

enum class TextAlign : u8 { Left, Center, Right };

// What the renderer draws: quads in order, each with a clip rectangle. Clip
// 0 is everything. Quads wholly outside their clip, empty or transparent are
// dropped.
class DrawList {
public:
    DrawList();
    std::vector<DrawQuad> quads;
    std::vector<Rect> clips;

    void PushClip(const Rect& rect); // intersected with the current clip (rect in the current transform)
    void PopClip();
    u32 CurrentClip() const { return stack_.back(); }
    // Render transforms (step 4): later quads and clips are scaled about
    // `pivot` then moved by `offset` (composed with the current one).
    void PushTransform(Vec2 offset, Vec2 scale, Vec2 pivot);
    void PopTransform();
    Rect Transform(const Rect& r) const;

    void AddQuad(const Rect& rect, Color color, u32 texture = 0, const Rect& uv = {0, 0, 1, 1});
    void AddBrush(const Rect& rect, const Brush& brush, f32 opacity = 1.0f);
    // A distance-field quad (see DrawQuad): `range` in the current layout units.
    void AddSdfQuad(const Rect& rect, Color color, u32 texture, const Rect& uv, f32 range, f32 edge, f32 softness = 0.0f);
    void AddText(const Font& font, std::string_view text, f32 size, const Rect& box, Color color, TextAlign align = TextAlign::Left,
                 f32 wrap_width = 0.0f, const TextEffects* effects = nullptr);
    // Scales everything (layout units to pixels).
    void Scale(f32 s);

private:
    struct Affine {
        Vec2 scale{1, 1}, translate;
    };
    std::vector<u32> stack_;
    std::vector<Affine> transforms_{Affine{}};
};

} // namespace aether::ui
