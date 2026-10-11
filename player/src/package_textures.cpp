#include "package_textures.h"

#include "aether/cook/texture_cook.h"

namespace aether::player {

bool LoadPackageCookedTexture(const GamePackage& package, const std::string& path, cook::CookedTexture& out, std::string* error) {
    const auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    const GameManifest::Asset* asset = package.FindAsset(path);
    if (asset == nullptr || asset->cooked.empty()) return fail("'" + path + "' has no cooked copy");
    std::vector<u8> atex;
    std::string why;
    if (!package.Files().Read(asset->cooked, atex, &why)) return fail("can't read " + asset->cooked + ": " + why);
    if (!cook::LoadAtex(atex, out, &why)) return fail(asset->cooked + ": " + why);
    return true;
}

bool PackageHasTextureSource(const GamePackage& package, const std::string& path) { return package.Files().Exists("Content/" + path); }

bool LoadPackageTexture(const GamePackage& package, const std::string& path, assets::ImageData& out, std::string* error) {
    const auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    // The source image, when the pak kept it.
    std::vector<u8> bytes;
    if (package.ReadContent(path, bytes) && assets::DecodeImageFromMemory(bytes, out)) return true;
    // Else the cooked copy (a cook that stripped the sources, for a mobile build).
    const GameManifest::Asset* asset = package.FindAsset(path);
    if (asset == nullptr || asset->cooked.empty()) return fail("'" + path + "' is not in the package, and has no cooked copy");
    std::vector<u8> atex;
    std::string why;
    cook::CookedTexture texture;
    if (!package.Files().Read(asset->cooked, atex, &why)) return fail("can't read " + asset->cooked + ": " + why);
    if (!cook::LoadAtex(atex, texture, &why)) return fail(asset->cooked + ": " + why);
    if (texture.mips.empty() || texture.width == 0 || texture.height == 0) return fail(asset->cooked + " has no pixels");
    std::vector<u8> pixels = cook::DecodeMip(texture, 0);
    if (pixels.size() != static_cast<usize>(texture.width) * texture.height * 4) return fail("can't decode " + asset->cooked);
    out.width = texture.width;
    out.height = texture.height;
    out.pixels = std::move(pixels);
    return true;
}

} // namespace aether::player
