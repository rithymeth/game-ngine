#pragma once

#include "aether/assets/image.h"
#include "aether/cook/texture_cook.h"
#include "aether/player/game.h"

#include <string>

// Textures from a cooked package (§25.3). A cook keeps each texture's source image (Content/<path>) and, beside it, a cooked, block-compressed
// copy (Cooked/<guid>.atex, BC or ASTC with mips). A cook made with `strip_source_textures` (for a mobile build, say) keeps only the cooked copy.
// The scene renderer asks for the pixels by content path through this, so either kind of pak works.

namespace aether::player {

// The texture's RGBA8 pixels: the source image if the pak has it (exact), else the cooked copy's top mip, decoded (BC and ASTC alike; BC4 comes
// back grey, BC5 as red and green with blue rebuilt for normal maps). False when the pak has neither or the data is damaged; `error` says why.
bool LoadPackageTexture(const GamePackage& package, const std::string& path, assets::ImageData& out, std::string* error = nullptr);

// The cooked copy as the cooker wrote it (format, mips), for a renderer that can use the compressed blocks directly. False when the asset has no
// cooked copy or it is damaged.
bool LoadPackageCookedTexture(const GamePackage& package, const std::string& path, cook::CookedTexture& out, std::string* error = nullptr);
// Whether the pak carries the texture's source image (a cook with `strip_source_textures` does not).
bool PackageHasTextureSource(const GamePackage& package, const std::string& path);

} // namespace aether::player
