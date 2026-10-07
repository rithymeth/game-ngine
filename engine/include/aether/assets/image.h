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
bool DecodeImageFile(const std::string& path, ImageData& out_image);
bool DecodeImageFile(const std::string& path, ImageData& out_image, u64 max_pixels);
// Decodes an in-memory image after checking its dimensions. The pixel budget
// bounds allocations when this is used on untrusted/imported data.
bool DecodeImageFromMemory(std::span<const u8> bytes, ImageData& out_image, u64 max_pixels = 16 * 1024 * 1024);

} // namespace aether::assets
