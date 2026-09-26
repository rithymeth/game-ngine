#include "aether/assets/image.h"

#include "aether/core/log.h"

#include <stb_image.h>

namespace aether::assets {

bool DecodeImageFile(const std::string& path, ImageData& out_image) {
    int width = 0;
    int height = 0;
    int channels_in_file = 0;
    // Force 4 channels (RGBA8) regardless of the source format, so callers
    // never need to branch on channel count — matching gfx::Texture's
    // fixed-RGBA8 upload path.
    stbi_uc* pixels = stbi_load(path.c_str(), &width, &height, &channels_in_file, 4);
    if (!pixels) {
        AETHER_LOG_ERROR("Assets", "Failed to load image \"%s\": %s", path.c_str(), stbi_failure_reason());
        return false;
    }
    if (width <= 0 || height <= 0) {
        stbi_image_free(pixels);
        AETHER_LOG_ERROR("Assets", "Image \"%s\" decoded with invalid dimensions (%dx%d)", path.c_str(), width,
                          height);
        return false;
    }

    out_image.width = static_cast<u32>(width);
    out_image.height = static_cast<u32>(height);
    usize byte_count = static_cast<usize>(width) * static_cast<usize>(height) * 4;
    out_image.pixels.assign(pixels, pixels + byte_count);
    stbi_image_free(pixels);
    return true;
}

} // namespace aether::assets
