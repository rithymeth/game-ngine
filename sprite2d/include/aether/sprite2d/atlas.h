#pragma once

#include "aether/assets/asset_guid.h"
#include "aether/assets/image.h"
#include "aether/core/base.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Sprite atlases (Phase 26 step 5, docs/design/PHASE_SPECS.md §26.6): one
// texture holding many named frames, with named animation clips over them.
// Saved as a `.aatlas` JSON asset; the texture is referenced by GUID, so
// the cooker follows it. Frame rectangles are in pixels, from the texture's
// top-left corner; a pivot is a fraction of the frame, from its top-left.

namespace aether::sprite2d {

struct SpriteFrame {
    std::string name;
    i32 x = 0, y = 0, w = 0, h = 0;
    f32 pivot_x = 0.5f, pivot_y = 0.5f;
};

struct SpriteClip {
    std::string name;
    std::vector<std::string> frames; // frame names, in play order
    f32 fps = 12.0f;
    bool loop = true;
};

struct SpriteAtlas {
    assets::AssetGuid texture;
    u32 width = 0, height = 0; // the texture's size in pixels
    std::vector<SpriteFrame> frames;
    std::vector<SpriteClip> clips;

    const SpriteFrame* FindFrame(std::string_view name) const;
    const SpriteClip* FindClip(std::string_view name) const;
    // The frame's texture coordinates {u0, v0, u1, v1}, v growing downward.
    // False if the atlas has no size.
    bool Uv(const SpriteFrame& frame, f32 out[4]) const;
};

nlohmann::json AtlasToJson(const SpriteAtlas& atlas);
bool AtlasFromJson(const nlohmann::json& data, SpriteAtlas& out, std::string* error = nullptr);
bool LoadAtlas(const std::filesystem::path& file, SpriteAtlas& out, std::string* error = nullptr);
bool SaveAtlas(const std::filesystem::path& file, const SpriteAtlas& atlas, std::string* error = nullptr);

// The clip's frame at `time` seconds: the index into SpriteClip::frames.
// A looping clip wraps; otherwise it holds its last frame and `finished`
// is set. An empty clip gives 0.
usize ClipFrameAt(const SpriteClip& clip, f32 time, bool* finished = nullptr);

// --- Packing -----------------------------------------------------------------

struct PackInput {
    std::string name;
    u32 w = 0, h = 0;
};
struct PackedRect {
    std::string name;
    i32 x = 0, y = 0;
    u32 w = 0, h = 0;
};
struct PackResult {
    u32 width = 0, height = 0; // a power of two each
    std::vector<PackedRect> rects; // in the order of the inputs
};

// Shelf-packs the rectangles (tallest first), `padding` pixels apart, into
// the smallest power-of-two texture up to `max_size`. False if they don't fit.
bool PackRects(const std::vector<PackInput>& inputs, u32 padding, u32 max_size, PackResult& out,
               std::string* error = nullptr);

// Draws each image (the same order as the packing's inputs) into one RGBA
// texture; the padding stays transparent.
bool ComposeAtlasImage(const std::vector<assets::ImageData>& images, const PackResult& packed, assets::ImageData& out,
                       std::string* error = nullptr);

// An atlas whose frames are the packed rects, over `texture`.
SpriteAtlas AtlasFromPack(const assets::AssetGuid& texture, const PackResult& packed);
// An atlas cutting the texture into a grid of cells (a sprite sheet): frames
// "<prefix>_0", "<prefix>_1", ... in reading order.
SpriteAtlas AtlasFromGrid(const assets::AssetGuid& texture, u32 width, u32 height, u32 cell_w, u32 cell_h,
                          const std::string& prefix = "frame");

} // namespace aether::sprite2d
