#pragma once

#include "aether/core/base.h"

#include <span>
#include <string>
#include <vector>

// Cooked textures (Phase 25 step 3, docs/design/PHASE_SPECS.md §25.3): the
// GPU-ready form a packaged game loads - block-compressed, with its mip
// chain - in an .atex container. Block compression keeps textures
// compressed in GPU memory too: BC1 at 8:1, BC3/BC5/BC7 at 4:1, BC4 at 2:1
// against RGBA8.
namespace aether::cook {

enum class TextureFormat : u8 {
    RGBA8 = 0, // uncompressed
    BC1 = 1,   // RGB (1-bit alpha), 4 bpp
    BC3 = 2,   // RGBA, 8 bpp
    BC4 = 3,   // one channel (red), 4 bpp
    BC5 = 4,   // two channels (red, green: normal maps), 8 bpp
    BC7 = 5,   // RGBA at high quality, 8 bpp
};
const char* TextureFormatName(TextureFormat f);
bool ParseTextureFormat(const std::string& name, TextureFormat& out);
// Bytes per 4x4 block, or 0 for RGBA8.
u32 BlockBytes(TextureFormat f);
// Bytes of one mip of this size.
usize MipBytes(TextureFormat f, u32 width, u32 height);

struct TextureCookSettings {
    // Auto picks from the content: BC5 for normal maps, BC4 for grey
    // images without alpha, BC7 otherwise.
    bool auto_format = true;
    TextureFormat format = TextureFormat::BC7;
    bool mips = true;
    bool srgb = true;         // colour data: mips are filtered in linear light
    bool normal_map = false;  // mips are renormalized; BC5 under auto
    u32 quality = 2;          // 0 (fastest) .. 4 (best), for BC7 and BC1/BC3
};

struct CookedMip {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> data;
};

struct CookedTexture {
    TextureFormat format = TextureFormat::RGBA8;
    bool srgb = false;
    bool normal_map = false;
    u32 width = 0;
    u32 height = 0;
    std::vector<CookedMip> mips; // largest first, down to 1x1
};

// The format Auto chooses for this image.
TextureFormat ChooseTextureFormat(std::span<const u8> rgba, bool normal_map);
// The mip chain, largest first (the image itself) down to 1x1: a 2x2 box
// filter, in linear light for sRGB colour; normal maps are renormalized.
std::vector<CookedMip> GenerateMips(std::span<const u8> rgba, u32 width, u32 height, bool srgb, bool normal_map);

// Cooks an RGBA8 image (rows top to bottom). Any size, not only powers of
// two or multiples of four: edge blocks repeat the last row and column.
CookedTexture CookTexture(std::span<const u8> rgba, u32 width, u32 height, const TextureCookSettings& settings);

// Decodes one mip back to RGBA8 (BC4 to grey, BC5 to red/green with blue
// rebuilt as a normal's z for normal maps, else 0).
std::vector<u8> DecodeMip(const CookedTexture& texture, usize mip);

// The .atex container: "ATEX", u32 version, u8 format, u8 flags (1 sRGB,
// 2 normal map), u16 0, u32 width, u32 height, u32 mip count, then per mip
// u32 width, u32 height, u32 size and its bytes (little-endian).
std::vector<u8> SaveAtex(const CookedTexture& texture);
bool LoadAtex(std::span<const u8> bytes, CookedTexture& out, std::string* error = nullptr);

} // namespace aether::cook
