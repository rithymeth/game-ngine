#pragma once

#include "aether/core/base.h"

#include <string>
#include <span>
#include <vector>

namespace aether::assets {

// Decoded RGBA8 image data, tightly packed (no row padding), top-to-bottom
// row order — matching what gfx::Texture's constructor already expects.
struct ImageData {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> pixels;
};

// Decodes an image file (PNG/JPG/BMP/TGA — whatever stb_image supports) from
// disk into RGBA8 pixels, converting any other channel count (grayscale,
// RGB, ...) up to 4 channels automatically. Returns false (leaving
// out_image untouched) if the file doesn't exist or fails to decode; check
// the return value rather than out_image.pixels.empty(), since a decoded
// zero-area image is a malformed-file case this treats as failure too.
[[nodiscard]] bool DecodeImageFile(const std::string& path, ImageData& out_image);
[[nodiscard]] bool DecodeImageFile(const std::string& path, ImageData& out_image, u64 max_pixels);
// Decodes an in-memory image after checking its dimensions. The pixel budget
// bounds allocations when this is used on untrusted/imported data.
[[nodiscard]] bool DecodeImageFromMemory(std::span<const u8> bytes, ImageData& out_image, u64 max_pixels = 16 * 1024 * 1024);

// How BuildMipChain averages texels: colour stored as sRGB (averaged in linear light, alpha linearly), plain data (each channel
// linearly: metallic-roughness, occlusion, masks), or a tangent-space normal map (xyz decoded, averaged, renormalised; alpha linearly).
enum class MipFilter : u8 { Srgb, Linear, NormalMap };

// Every mip level of an RGBA8 image, level 0 (the image) first, down to 1x1: level i is max(1, width >> i) by max(1, height >> i),
// each texel the 2x2 box average of the level above (clamped at an odd edge). The layout IDevice::CreateTextureWithMips takes;
// `mip_count` receives the number of levels. Empty (mip_count 0) when the sizes don't match the pixels.
[[nodiscard]] std::vector<u8> BuildMipChain(u32 width, u32 height, std::span<const u8> rgba8, MipFilter filter, u32& mip_count);

// Decoded high-dynamic-range image data: linear RGB floats, tightly packed, top row first.
struct HdrImageData {
    u32 width = 0;
    u32 height = 0;
    std::vector<f32> rgb;
};

// Whether the bytes are a Radiance RGBE (.hdr) image.
[[nodiscard]] bool IsHdrImage(std::span<const u8> bytes);
// Decodes a Radiance RGBE (.hdr) image from memory to linear floats, unclamped. False (out_image untouched) for anything else, a
// damaged file or more than max_pixels pixels.
[[nodiscard]] bool DecodeHdrImageFromMemory(std::span<const u8> bytes, HdrImageData& out_image, u64 max_pixels = 16 * 1024 * 1024);

} // namespace aether::assets
