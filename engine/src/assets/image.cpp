#include "aether/assets/image.h"

#include "aether/core/log.h"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace aether::assets {

namespace {

// stb_image can reserve the declared IDAT size before it discovers that a
// truncated PNG does not contain those bytes. Validate chunk bounds against
// the actual input first so tiny malformed files cannot request GiB of RAM.
bool PngChunksFitInput(std::span<const u8> bytes) {
    constexpr u8 signature[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (bytes.size() < sizeof(signature) ||
        !std::equal(std::begin(signature), std::end(signature), bytes.begin())) {
        return true; // Let stb_image handle other formats and invalid signatures.
    }

    usize offset = sizeof(signature);
    while (offset <= bytes.size() && bytes.size() - offset >= 12) {
        const u64 chunk_size = (static_cast<u64>(bytes[offset]) << 24) |
                               (static_cast<u64>(bytes[offset + 1]) << 16) |
                               (static_cast<u64>(bytes[offset + 2]) << 8) |
                               static_cast<u64>(bytes[offset + 3]);
        // Each chunk also needs a four-byte type and four-byte CRC.
        if (chunk_size > bytes.size() - offset - 12) return false;
        const bool is_end = bytes[offset + 4] == 'I' && bytes[offset + 5] == 'E' &&
                            bytes[offset + 6] == 'N' && bytes[offset + 7] == 'D';
        if (is_end) return chunk_size == 0;
        offset += static_cast<usize>(chunk_size) + 12;
    }
    return false;
}

// stbi_info reports a top-down BMP's height as negative (the file stores it that way; stbi_load flips it positive). That is a valid
// image, so the height is taken as its size; INT_MIN has no positive and stays rejected.
bool NormalizeInfoHeight(int& height) {
    if (height == std::numeric_limits<int>::min()) return false;
    if (height < 0) height = -height;
    return height > 0;
}

} // namespace


bool DecodeImageFile(const std::string& path, ImageData& out_image) {
    return DecodeImageFile(path, out_image, 16 * 1024 * 1024);
}

bool DecodeImageFile(const std::string& path, ImageData& out_image, u64 max_pixels) {
    int width = 0;
    int height = 0;
    int channels_in_file = 0;
    if (!stbi_info(path.c_str(), &width, &height, &channels_in_file) || width <= 0 || !NormalizeInfoHeight(height) ||
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

std::vector<u8> BuildMipChain(u32 width, u32 height, std::span<const u8> rgba8, MipFilter filter, u32& mip_count) {
    mip_count = 0;
    if (width == 0 || height == 0 || rgba8.size() != static_cast<usize>(width) * height * 4) return {};
    static const auto tables = [] {
        struct Tables {
            f32 to_linear[256];
            u8 to_srgb[4096]; // linear quantised to 12 bits
        } t{};
        for (u32 i = 0; i < 256; ++i) {
            const f32 c = static_cast<f32>(i) / 255.0f;
            t.to_linear[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        for (u32 i = 0; i < 4096; ++i) {
            const f32 l = static_cast<f32>(i) / 4095.0f;
            const f32 s = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
            t.to_srgb[i] = static_cast<u8>(std::lround(std::clamp(s, 0.0f, 1.0f) * 255.0f));
        }
        return t;
    }();
    const auto to_byte = [](f32 v) { return static_cast<u8>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };

    u32 levels = 1;
    while ((std::max(width, height) >> levels) > 0) ++levels;
    usize total = 0;
    for (u32 i = 0; i < levels; ++i) total += static_cast<usize>(std::max(1u, width >> i)) * std::max(1u, height >> i) * 4;
    std::vector<u8> out(total);
    std::copy(rgba8.begin(), rgba8.end(), out.begin());
    usize previous = 0, current = rgba8.size();
    u32 pw = width, ph = height;
    for (u32 level = 1; level < levels; ++level) {
        const u32 w = std::max(1u, width >> level), h = std::max(1u, height >> level);
        for (u32 y = 0; y < h; ++y) {
            for (u32 x = 0; x < w; ++x) {
                const u32 xs[2] = {std::min(x * 2, pw - 1), std::min(x * 2 + 1, pw - 1)};
                const u32 ys[2] = {std::min(y * 2, ph - 1), std::min(y * 2 + 1, ph - 1)};
                f32 sum[4] = {0, 0, 0, 0};
                for (const u32 sy : ys) {
                    for (const u32 sx : xs) {
                        const u8* t = &out[previous + (static_cast<usize>(sy) * pw + sx) * 4];
                        for (u32 c = 0; c < 3; ++c) {
                            sum[c] += filter == MipFilter::Srgb ? tables.to_linear[t[c]]
                                      : filter == MipFilter::NormalMap ? static_cast<f32>(t[c]) / 127.5f - 1.0f
                                                                       : static_cast<f32>(t[c]) / 255.0f;
                        }
                        sum[3] += static_cast<f32>(t[3]) / 255.0f;
                    }
                }
                u8* d = &out[current + (static_cast<usize>(y) * w + x) * 4];
                if (filter == MipFilter::NormalMap) {
                    const f32 length = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
                    const f32 n[3] = {length > 1e-6f ? sum[0] / length : 0.0f, length > 1e-6f ? sum[1] / length : 0.0f,
                                      length > 1e-6f ? sum[2] / length : 1.0f};
                    for (u32 c = 0; c < 3; ++c) d[c] = to_byte(n[c] * 0.5f + 0.5f);
                } else if (filter == MipFilter::Srgb) {
                    for (u32 c = 0; c < 3; ++c) d[c] = tables.to_srgb[static_cast<u32>(std::lround(std::clamp(sum[c] * 0.25f, 0.0f, 1.0f) * 4095.0f))];
                } else {
                    for (u32 c = 0; c < 3; ++c) d[c] = to_byte(sum[c] * 0.25f);
                }
                d[3] = to_byte(sum[3] * 0.25f);
            }
        }
        previous = current;
        current += static_cast<usize>(w) * h * 4;
        pw = w, ph = h;
    }
    mip_count = levels;
    return out;
}

bool IsHdrImage(std::span<const u8> bytes) {
    if (bytes.empty() || bytes.size() > static_cast<usize>(std::numeric_limits<int>::max())) return false;
    return stbi_is_hdr_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size())) != 0;
}

bool DecodeHdrImageFromMemory(std::span<const u8> bytes, HdrImageData& out_image, u64 max_pixels) {
    if (!IsHdrImage(bytes)) return false;
    int width = 0, height = 0, channels = 0;
    const auto* input = reinterpret_cast<const stbi_uc*>(bytes.data());
    const int input_size = static_cast<int>(bytes.size());
    if (!stbi_info_from_memory(input, input_size, &width, &height, &channels) || width <= 0 || height <= 0) return false;
    const u64 pixel_count = static_cast<u64>(width) * static_cast<u64>(height);
    if (pixel_count > max_pixels || pixel_count > std::numeric_limits<usize>::max() / (3 * sizeof(f32))) return false;
    f32* pixels = stbi_loadf_from_memory(input, input_size, &width, &height, &channels, 3);
    if (!pixels) return false;
    if (width <= 0 || height <= 0 || static_cast<u64>(width) * static_cast<u64>(height) != pixel_count) {
        stbi_image_free(pixels);
        return false;
    }
    HdrImageData decoded;
    decoded.width = static_cast<u32>(width);
    decoded.height = static_cast<u32>(height);
    decoded.rgb.assign(pixels, pixels + static_cast<usize>(pixel_count) * 3);
    stbi_image_free(pixels);
    out_image = std::move(decoded);
    return true;
}

bool DecodeImageFromMemory(std::span<const u8> bytes, ImageData& out_image, u64 max_pixels) {
    if (bytes.empty() || bytes.size() > static_cast<usize>(std::numeric_limits<int>::max()) ||
        !PngChunksFitInput(bytes)) return false;
    int width = 0, height = 0, channels = 0;
    const auto* input = reinterpret_cast<const stbi_uc*>(bytes.data());
    const int input_size = static_cast<int>(bytes.size());
    if (!stbi_info_from_memory(input, input_size, &width, &height, &channels) || width <= 0 || !NormalizeInfoHeight(height)) return false;
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
