#include "aether/assets/texture_importer.h"

#include "aether/assets/image.h"

#include <array>
#include <initializer_list>
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace aether::assets {

namespace {

constexpr char kMagic[4] = {'A', 'T', 'E', 'X'};
constexpr u32 kFormatVersion = 1;
constexpr u32 kFlagSrgb = 1u;

// sRGB <-> linear per IEC 61966-2-1, via a 256-entry decode table and an
// exact encode.
const std::array<f32, 256>& SrgbToLinearTable() {
    static const std::array<f32, 256> table = [] {
        std::array<f32, 256> t{};
        for (int i = 0; i < 256; ++i) {
            f32 c = static_cast<f32>(i) / 255.0f;
            t[static_cast<usize>(i)] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();
    return table;
}

u8 LinearToSrgb8(f32 linear) {
    linear = std::clamp(linear, 0.0f, 1.0f);
    f32 c = linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
    return static_cast<u8>(std::lround(c * 255.0f));
}

void AppendU32(std::vector<u8>& out, u32 value) {
    const u8* b = reinterpret_cast<const u8*>(&value);
    out.insert(out.end(), b, b + 4);
}

bool ReadU32(const std::vector<u8>& in, usize& offset, u32& value) {
    if (offset > in.size() || in.size() - offset < 4) {
        return false;
    }
    std::memcpy(&value, in.data() + offset, 4);
    offset += 4;
    return true;
}

template <typename T>
T SettingOr(const nlohmann::json& settings, const char* name, T fallback, std::vector<std::string>& warnings,
            bool (nlohmann::json::*is_type)() const noexcept) {
    auto it = settings.find(name);
    if (it == settings.end()) {
        return fallback;
    }
    if (!((*it).*is_type)()) {
        warnings.push_back(std::string("Setting \"") + name + "\" has the wrong type; using the default");
        return fallback;
    }
    return it->get<T>();
}

ImportResult ImportDecoded(ImageData image, const nlohmann::json& settings);

} // namespace

std::vector<u8> DownsampleRGBA8(const std::vector<u8>& pixels, u32 width, u32 height, bool srgb) {
    if (width == 0 || height == 0) return {};
    const u64 pixel_count = static_cast<u64>(width) * height;
    if (pixel_count > std::numeric_limits<usize>::max() / 4 || pixels.size() != static_cast<usize>(pixel_count) * 4)
        return {};

    const u32 out_w = std::max<u32>(1, width / 2);
    const u32 out_h = std::max<u32>(1, height / 2);
    const auto& to_linear = SrgbToLinearTable();
    std::vector<u8> out(static_cast<usize>(out_w) * out_h * 4);
    for (u32 y = 0; y < out_h; ++y) {
        for (u32 x = 0; x < out_w; ++x) {
            // The 2x2 source block, clamped at the edges (odd sizes, and the
            // 1-pixel side of a non-square chain).
            const u32 x0 = std::min(x * 2, width - 1), x1 = std::min(x * 2 + 1, width - 1);
            const u32 y0 = std::min(y * 2, height - 1), y1 = std::min(y * 2 + 1, height - 1);
            const u32 xs[2] = {x0, x1};
            const u32 ys[2] = {y0, y1};
            for (int channel = 0; channel < 4; ++channel) {
                f32 sum = 0.0f;
                for (u32 sy : ys) {
                    for (u32 sx : xs) {
                        u8 v = pixels[(static_cast<usize>(sy) * width + sx) * 4 + static_cast<usize>(channel)];
                        sum += (srgb && channel < 3) ? to_linear[v] : static_cast<f32>(v) / 255.0f;
                    }
                }
                const f32 average = sum / 4.0f;
                out[(static_cast<usize>(y) * out_w + x) * 4 + static_cast<usize>(channel)] =
                    (srgb && channel < 3) ? LinearToSrgb8(average)
                                          : static_cast<u8>(std::lround(std::clamp(average, 0.0f, 1.0f) * 255.0f));
            }
        }
    }
    return out;
}

std::vector<u8> EncodeTextureData(const TextureData& texture) {
    std::vector<u8> out(kMagic, kMagic + 4);
    AppendU32(out, kFormatVersion);
    AppendU32(out, texture.width);
    AppendU32(out, texture.height);
    AppendU32(out, static_cast<u32>(texture.mips.size()));
    AppendU32(out, texture.srgb ? kFlagSrgb : 0u);
    for (const std::vector<u8>& mip : texture.mips) {
        AppendU32(out, static_cast<u32>(mip.size()));
        out.insert(out.end(), mip.begin(), mip.end());
    }
    return out;
}

bool DecodeTextureData(const std::vector<u8>& bytes, TextureData& out) {
    if (bytes.size() < 4 || std::memcmp(bytes.data(), kMagic, 4) != 0) {
        return false;
    }
    usize offset = 4;
    u32 version = 0, width = 0, height = 0, mip_count = 0, flags = 0;
    if (!ReadU32(bytes, offset, version) || version != kFormatVersion || !ReadU32(bytes, offset, width) ||
        !ReadU32(bytes, offset, height) || !ReadU32(bytes, offset, mip_count) || !ReadU32(bytes, offset, flags) ||
        width == 0 || height == 0 || mip_count == 0 || (flags & ~kFlagSrgb) != 0) {
        return false;
    }
    u32 largest_dimension = std::max(width, height);
    u32 max_mips = 1;
    while (largest_dimension > 1) {
        largest_dimension >>= 1;
        ++max_mips;
    }
    if (mip_count > max_mips) return false;
    TextureData texture;
    texture.width = width;
    texture.height = height;
    texture.srgb = (flags & kFlagSrgb) != 0;
    for (u32 level = 0; level < mip_count; ++level) {
        u32 size = 0;
        const u64 pixels = static_cast<u64>(texture.MipWidth(level)) * texture.MipHeight(level);
        if (pixels > std::numeric_limits<usize>::max() / 4) {
            return false;
        }
        const usize expected = static_cast<usize>(pixels) * 4;
        if (!ReadU32(bytes, offset, size) || size != expected || offset > bytes.size() ||
            size > bytes.size() - offset) {
            return false;
        }
        texture.mips.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                  bytes.begin() + static_cast<std::ptrdiff_t>(offset + size));
        offset += size;
    }
    if (offset != bytes.size()) {
        return false;
    }
    out = std::move(texture);
    return true;
}

namespace {

// Says why a file would not decode, from its first bytes: what it is (a Git LFS pointer, an HTML page, a format this engine does not read, text) or, for a PNG, a JPEG or a BMP, how big it
// says it is (to tell "too big" from "damaged"). The decoder itself only says no. `bytes` is as much of the start of the file as was read; `file_size` its whole size.
std::string WhyNotAnImage(std::span<const u8> bytes, u64 file_size, u64 max_pixels) {
    auto starts = [&](const char* text) { const usize n = std::strlen(text); return bytes.size() >= n && std::memcmp(bytes.data(), text, n) == 0; };
    auto u32be = [&](usize at) { return (static_cast<u64>(bytes[at]) << 24) | (static_cast<u64>(bytes[at + 1]) << 16) | (static_cast<u64>(bytes[at + 2]) << 8) | bytes[at + 3]; };
    auto u32le = [&](usize at) { return (static_cast<u64>(bytes[at + 3]) << 24) | (static_cast<u64>(bytes[at + 2]) << 16) | (static_cast<u64>(bytes[at + 1]) << 8) | bytes[at]; };
    auto too_big = [&](const char* format, u64 width, u64 height) {
        return std::string(format) + " is " + std::to_string(width) + " x " + std::to_string(height) + " (" + std::to_string(width * height / 1000000) + " megapixels), over the budget of " +
               std::to_string(max_pixels / 1000000) + " megapixels: make it smaller";
    };
    if (file_size == 0 || bytes.empty()) return "the file is empty (0 bytes): the export or the copy did not finish";
    if (starts("version https://git-lfs")) return "this is a Git LFS pointer file (a few lines of text), not the image: run \"git lfs pull\" to fetch the real file";
    if (starts("<!DOCTYPE") || starts("<html") || starts("<HTML") || starts("<?xml")) return "this is a web page or XML document, not an image (a failed download, or a link saved as a file?)";
    if (starts("\x89PNG\r\n\x1a\n")) {
        if (bytes.size() >= 24) {
            const u64 w = u32be(16), h = u32be(20);
            if (w == 0 || h == 0) return "this is a PNG whose header says it is " + std::to_string(w) + " x " + std::to_string(h) + ": damaged";
            if (w * h > max_pixels) return too_big("this PNG", w, h);
            return "this is a " + std::to_string(w) + " x " + std::to_string(h) + " PNG that cannot be decoded: it is truncated or damaged (" + std::to_string(file_size) + " bytes)";
        }
        return "this starts like a PNG but ends inside its header (" + std::to_string(file_size) + " bytes): it is truncated";
    }
    if (bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
        for (usize at = 2; at + 9 < bytes.size();) { // walk the markers to the frame header, which holds the size
            if (bytes[at] != 0xFF) break;
            const u8 marker = bytes[at + 1];
            if ((marker >= 0xC0 && marker <= 0xCF) && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
                const u64 h = (static_cast<u64>(bytes[at + 5]) << 8) | bytes[at + 6], w = (static_cast<u64>(bytes[at + 7]) << 8) | bytes[at + 8];
                if (w * h > max_pixels) return too_big("this JPEG", w, h);
                return "this is a " + std::to_string(w) + " x " + std::to_string(h) + " JPEG that cannot be decoded: it is truncated or damaged (" + std::to_string(file_size) + " bytes)" +
                       ((marker == 0xC2) ? "; it is a progressive JPEG, which may be what the decoder dislikes: re-save it as a baseline JPEG" : "");
            }
            at += 2 + ((static_cast<usize>(bytes[at + 2]) << 8) | bytes[at + 3]);
        }
        return "this is a JPEG that cannot be decoded: it is truncated or damaged (" + std::to_string(file_size) + " bytes)";
    }
    if (starts("BM") && bytes.size() >= 26) {
        const u64 w = u32le(18), h = u32le(22);
        if (w * h > max_pixels) return too_big("this BMP", w, h);
        return "this is a BMP that cannot be decoded: it is truncated, damaged, or uses a compression this engine does not read (re-save it as PNG)";
    }
    auto bytes_are = [&](std::initializer_list<u8> expected) { return bytes.size() >= expected.size() && std::equal(expected.begin(), expected.end(), bytes.begin()); };
    const char* unsupported = starts("RIFF") && bytes.size() >= 12 && std::memcmp(bytes.data() + 8, "WEBP", 4) == 0 ? "WebP"
                              : bytes_are({'I', 'I', '*', 0}) || bytes_are({'M', 'M', 0, '*'}) ? "TIFF"
                              : starts("DDS ") ? "DDS" : starts("8BPS") ? "Photoshop (PSD)" : bytes_are({0, 0, 1, 0}) ? "ICO"
                              : starts("%PDF") ? "PDF" : starts("KTX ") || bytes_are({0xAB, 'K', 'T', 'X'}) ? "KTX" : nullptr;
    if (unsupported != nullptr) return std::string("the file is in ") + unsupported + " format, which this engine does not import: convert it to PNG, JPEG, BMP or TGA";
    // Plain text (a script, a config, a note renamed to .png) or unknown bytes.
    bool printable = true;
    const usize look = std::min<usize>(bytes.size(), 64);
    for (usize i = 0; i < look; ++i)
        if (!((bytes[i] >= 32 && bytes[i] < 127) || bytes[i] == '\n' || bytes[i] == '\r' || bytes[i] == '\t')) printable = false;
    if (printable) {
        std::string head(reinterpret_cast<const char*>(bytes.data()), std::min<usize>(bytes.size(), 24));
        std::replace(head.begin(), head.end(), '\n', ' ');
        std::replace(head.begin(), head.end(), '\r', ' ');
        return "this is a text file, not an image (it starts with \"" + head + "\")";
    }
    std::string hex;
    char two[4];
    for (usize i = 0; i < std::min<usize>(bytes.size(), 8); ++i) { std::snprintf(two, sizeof(two), "%02X ", bytes[i]); hex += two; }
    return "this is not a recognised image (it starts with bytes " + hex + "): the engine reads PNG, JPEG, BMP, TGA, GIF, PSD, PIC and PNM";
}

} // namespace

nlohmann::json TextureImporter::DefaultSettings() const {
    return {{"srgb", true}, {"generate_mips", true}, {"max_size", 0}};
}

ImportResult TextureImporter::Import(const ImportContext& context) const {
    ImageData image;
    if (!DecodeImageFile(context.source.string(), image, 16 * 1024 * 1024)) {
        ImportResult result;
        // Why, from the first bytes of the file.
        std::ifstream in(context.source, std::ios::binary);
        const bool opened = in.is_open();
        std::vector<u8> head(64 * 1024);
        in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
        head.resize(static_cast<usize>(in.gcount()));
        std::error_code size_error;
        const u64 file_size = std::filesystem::file_size(context.source, size_error);
        result.error = opened ? WhyNotAnImage(head, size_error ? head.size() : file_size, 16 * 1024 * 1024) : "the file could not be opened (" + context.source.string() + "): it is missing or not readable";
        if (!opened && !std::filesystem::exists(context.source)) result.error = "the file is missing: " + context.source.string();
        return result;
    }
    return ImportDecoded(std::move(image), context.settings);
}

ImportResult TextureImporter::ImportFromMemory(std::span<const u8> bytes, const nlohmann::json& settings, u64 max_pixels) const {
    ImageData image;
    if (!DecodeImageFromMemory(bytes, image, max_pixels)) {
        ImportResult result;
        result.error = WhyNotAnImage(bytes.first(std::min<usize>(bytes.size(), 64 * 1024)), bytes.size(), max_pixels);
        return result;
    }
    return ImportDecoded(std::move(image), settings);
}

namespace {

ImportResult ImportDecoded(ImageData image, const nlohmann::json& settings) {
    ImportResult result;
    const bool srgb = SettingOr<bool>(settings, "srgb", true, result.warnings, &nlohmann::json::is_boolean);
    const bool mips = SettingOr<bool>(settings, "generate_mips", true, result.warnings, &nlohmann::json::is_boolean);
    const i64 max_size = SettingOr<i64>(settings, "max_size", 0, result.warnings, &nlohmann::json::is_number_integer);

    TextureData texture;
    texture.srgb = srgb;
    texture.width = image.width;
    texture.height = image.height;
    std::vector<u8> level = std::move(image.pixels);
    // Enforce max_size by halving the base level until it fits.
    while (max_size > 0 && (texture.width > static_cast<u64>(max_size) || texture.height > static_cast<u64>(max_size))) {
        level = DownsampleRGBA8(level, texture.width, texture.height, srgb);
        texture.width = std::max<u32>(1, texture.width / 2);
        texture.height = std::max<u32>(1, texture.height / 2);
    }
    texture.mips.push_back(level);
    if (mips) {
        u32 w = texture.width, h = texture.height;
        while (w > 1 || h > 1) {
            level = DownsampleRGBA8(level, w, h, srgb);
            w = std::max<u32>(1, w / 2);
            h = std::max<u32>(1, h / 2);
            texture.mips.push_back(level);
        }
    }
    result.data = EncodeTextureData(texture);
    result.ok = true;
    return result;
}

} // namespace

} // namespace aether::assets
