#pragma once

#include "aether/core/base.h"

#include <span>
#include <vector>

namespace aether::assets {

// A PNG (8-bit RGB) of `rgb`, `width * height * 3` bytes row by row from the top. Lossless and dependency-free: the pixel rows are stored as
// uncompressed deflate blocks, so the file is about as large as the raw pixels (a generated texture is cooked to a block-compressed format before
// it ships, so the size only matters on disk during authoring). Empty when the size is zero or `rgb` is shorter than it says.
std::vector<u8> EncodePngRgb8(u32 width, u32 height, std::span<const u8> rgb);

// The same for 8-bit RGBA, `width * height * 4` bytes (a water texture's alpha is its opacity).
std::vector<u8> EncodePngRgba8(u32 width, u32 height, std::span<const u8> rgba);

} // namespace aether::assets
