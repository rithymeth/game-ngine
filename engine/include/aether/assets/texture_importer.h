#pragma once

#include "aether/assets/importer.h"

#include <algorithm>

namespace aether::assets {

// A processed texture: RGBA8 mip chain, largest first.
struct TextureData {
    u32 width = 0;
    u32 height = 0;
    bool srgb = true;
    std::vector<std::vector<u8>> mips;

    u32 MipWidth(usize level) const { return std::max<u32>(1, width >> level); }
    u32 MipHeight(usize level) const { return std::max<u32>(1, height >> level); }
};

// The texture importer's output format ("ATEX" v1). BC-compressed formats
// (docs/ROADMAP.md Phase 8) come with an encoder; this is uncompressed RGBA8.
std::vector<u8> EncodeTextureData(const TextureData& texture);
bool DecodeTextureData(const std::vector<u8>& bytes, TextureData& out);

// Downsamples one RGBA8 level to half size (rounding down, min 1), with a
// 2x2 box filter. sRGB colour is averaged in linear space (averaging encoded
// values darkens it); alpha is always linear.
std::vector<u8> DownsampleRGBA8(const std::vector<u8>& pixels, u32 width, u32 height, bool srgb);

// Settings: "srgb" (bool, default true: colour textures; false for normal
// maps and masks), "generate_mips" (bool, default true), "max_size" (int,
// default 0 = no limit: halve until both sides fit).
class TextureImporter final : public IAssetImporter {
public:
    const char* Name() const override { return "Texture"; }
    u32 Version() const override { return 1; }
    nlohmann::json DefaultSettings() const override;
    ImportResult Import(const ImportContext& context) const override;
};

} // namespace aether::assets
