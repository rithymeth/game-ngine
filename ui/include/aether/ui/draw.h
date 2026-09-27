#pragma once

#include "aether/ui/geometry.h"

#include <string>
#include <string_view>
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
struct DrawQuad {
    Rect rect;
    Rect uv{0, 0, 1, 1};
    Color color;
    u32 texture = 0;
    u32 clip = 0;
};

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
};

// A stand-in until real fonts (step 5): fixed-width boxes, so layout is
// predictable. Advance 0.6 × size, line height 1.25 × size, ascent 0.95 × size.
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

    void PushClip(const Rect& rect); // intersected with the current clip
    void PopClip();
    u32 CurrentClip() const { return stack_.back(); }

    void AddQuad(const Rect& rect, Color color, u32 texture = 0, const Rect& uv = {0, 0, 1, 1});
    void AddBrush(const Rect& rect, const Brush& brush, f32 opacity = 1.0f);
    void AddText(const Font& font, std::string_view text, f32 size, const Rect& box, Color color, TextAlign align = TextAlign::Left,
                 f32 wrap_width = 0.0f);
    // Scales everything (layout units to pixels).
    void Scale(f32 s);

private:
    std::vector<u32> stack_;
};

} // namespace aether::ui
