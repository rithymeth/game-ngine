#include "aether/assets/image.h"

#include "aether/core/log.h"

#include <stb_image.h>

#include <limits>

namespace aether::assets {

bool DecodeImageFile(const std::string& path, ImageData& out_image) {
    return DecodeImageFile(path, out_image, 16 * 1024 * 1024);
}

bool DecodeImageFile(const std::string& path, ImageData& out_image, u64 max_pixels) {
    int width = 0;
    int height = 0;
    int channels_in_file = 0;
    if (!stbi_info(path.c_str(), &width, &height, &channels_in_file) || width <= 0 || height <= 0 ||
        static_cast<u64>(width) * static_cast<u64>(height) > max_pixels) {
        AETHER_LOG_ERROR("Assets", "Image \"%s\" is invalid or exceeds the pixel budget", path.c_str());
        return false;
    }
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
    const u64 pixel_count = static_cast<u64>(width) * static_cast<u64>(height);
    if (pixel_count > max_pixels || pixel_count > std::numeric_limits<usize>::max() / 4) {
        stbi_image_free(pixels);
        AETHER_LOG_ERROR("Assets", "Image \"%s\" exceeds the pixel budget", path.c_str());
        return false;
    }

    out_image.width = static_cast<u32>(width);
    out_image.height = static_cast<u32>(height);
    usize byte_count = static_cast<usize>(width) * static_cast<usize>(height) * 4;
    out_image.pixels.assign(pixels, pixels + byte_count);
    stbi_image_free(pixels);
    return true;
}

bool DecodeImageFromMemory(std::span<const u8> bytes, ImageData& out_image, u64 max_pixels) {
    if (bytes.empty() || bytes.size() > static_cast<usize>(std::numeric_limits<int>::max())) return false;
    int width = 0, height = 0, channels = 0;
    const auto* input = reinterpret_cast<const stbi_uc*>(bytes.data());
    const int input_size = static_cast<int>(bytes.size());
    if (!stbi_info_from_memory(input, input_size, &width, &height, &channels) || width <= 0 || height <= 0) return false;
    const u64 pixel_count = static_cast<u64>(width) * static_cast<u64>(height);
    if (pixel_count > max_pixels || pixel_count > std::numeric_limits<usize>::max() / 4) return false;

    stbi_uc* pixels = stbi_load_from_memory(input, input_size, &width, &height, &channels, 4);
    if (!pixels) return false;
    const u64 decoded_pixels = width > 0 && height > 0 ? static_cast<u64>(width) * static_cast<u64>(height) : 0;
    if (decoded_pixels == 0 || decoded_pixels > max_pixels || decoded_pixels > std::numeric_limits<usize>::max() / 4) {
        stbi_image_free(pixels);
        return false;
    }
    const usize byte_count = static_cast<usize>(width) * static_cast<usize>(height) * 4;
    ImageData decoded;
    decoded.width = static_cast<u32>(width);
    decoded.height = static_cast<u32>(height);
    decoded.pixels.assign(pixels, pixels + byte_count);
    stbi_image_free(pixels);
    out_image = std::move(decoded);
    return true;
}

} // namespace aether::assets
