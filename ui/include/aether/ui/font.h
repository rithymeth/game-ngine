#pragma once

#include "aether/ui/draw.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace aether::ui {

// Distance-field fonts from TrueType (Phase 18 step 5, §18.5).
//
// Glyphs are rasterized once, at `base_size`, as signed distance fields
// (stb_truetype) into a one-channel atlas, when they're first drawn. The
// UI shader turns distance into coverage (SdfCoverage), so text stays sharp
// at any size or zoom, and outlines and soft shadows cost nothing extra.
struct SdfFontSettings {
    f32 base_size = 48.0f; // pixel size glyphs are rasterized at
    i32 padding = 6;       // atlas pixels of field outside each glyph's edge (limits outlines)
    i32 atlas_width = 512, atlas_height = 256;
    i32 max_atlas_height = 4096; // the atlas doubles in height up to this when it fills
};

// A rectangle of atlas pixels.
struct AtlasRect {
    i32 x = 0, y = 0, w = 0, h = 0;
    bool Empty() const { return w <= 0 || h <= 0; }
};

class SdfFont final : public Font {
public:
    // Null (and `error` says why) if the data isn't a TrueType/OpenType font.
    static std::unique_ptr<SdfFont> FromMemory(std::vector<u8> ttf, const SdfFontSettings& settings = {}, std::string* error = nullptr);
    static std::unique_ptr<SdfFont> FromFile(const std::string& path, const SdfFontSettings& settings = {}, std::string* error = nullptr);
    ~SdfFont() override;
    SdfFont(const SdfFont&) = delete;
    SdfFont& operator=(const SdfFont&) = delete;

    // --- Font -----------------------------------------------------------------------
    f32 Ascent(f32 size) const override;
    f32 LineHeight(f32 size) const override;
    // Rasterizes the glyph into the atlas the first time. Code points the
    // font lacks draw as U+FFFD, else '?'.
    Glyph GlyphOf(u32 codepoint, f32 size) const override;
    f32 Kerning(u32 a, u32 b, f32 size) const override;
    u32 Texture() const override { return texture_; }
    f32 SdfRange(f32 size) const override;
    f32 SdfEdge() const override;

    bool HasGlyph(u32 codepoint) const;
    std::string FamilyName() const; // the name table's family name, ID 1 ("Roboto Medium"; "" if it has none)
    const SdfFontSettings& Settings() const { return settings_; }
    // Rasterize ahead of time (a loading screen), so the first frame that shows the text doesn't.
    void Prewarm(std::string_view utf8) const;
    usize GlyphCount() const; // rasterized so far

    // --- The atlas, for the renderer --------------------------------------------------
    i32 AtlasWidth() const { return width_; }
    i32 AtlasHeight() const { return height_; }
    const std::vector<u8>& AtlasPixels() const { return pixels_; } // one byte (the distance) per pixel, rows top to bottom
    // What changed since ClearDirty: upload `DirtyRect`, or the whole atlas
    // (a new texture) if it `Resized`.
    bool Dirty() const { return !dirty_.Empty(); }
    AtlasRect DirtyRect() const { return dirty_; }
    bool Resized() const { return resized_; }
    void ClearDirty() {
        dirty_ = {};
        resized_ = false;
    }
    // The renderer's texture id for the atlas (what quads draw with).
    void SetTexture(u32 id) { texture_ = id; }
    // The field at an atlas point (0..1 per axis, bilinear): what the shader samples.
    f32 SampleDistance(Vec2 uv) const;
    // Glyphs that didn't fit in the largest atlas.
    const std::vector<std::string>& Problems() const { return problems_; }

private:
    struct Entry;
    struct Impl;
    SdfFont() = default;
    const Entry& Get(u32 codepoint) const;
    bool Pack(i32 w, i32 h, i32& x, i32& y) const;
    void MarkDirty(const AtlasRect& r) const;

    std::unique_ptr<Impl> impl_;
    SdfFontSettings settings_;
    u32 texture_ = 0;
    // The glyph cache fills while drawing (from const methods): not thread-safe.
    mutable i32 width_ = 0, height_ = 0;
    mutable std::vector<u8> pixels_;
    mutable AtlasRect dirty_;
    mutable bool resized_ = false;
    mutable i32 pen_x_ = 1, pen_y_ = 1, row_h_ = 0;
    mutable std::vector<std::string> problems_;
};

} // namespace aether::ui
