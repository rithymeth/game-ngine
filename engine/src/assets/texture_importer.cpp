#include "aether/assets/texture_importer.h"

#include "aether/assets/image.h"

#include <array>
#include <cmath>
#include <cstring>

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
    if (offset + 4 > in.size()) {
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

} // namespace

std::vector<u8> DownsampleRGBA8(const std::vector<u8>& pixels, u32 width, u32 height, bool srgb) {
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
        width == 0 || height == 0 || mip_count == 0 || mip_count > 32) {
        return false;
    }
    TextureData texture;
    texture.width = width;
    texture.height = height;
    texture.srgb = (flags & kFlagSrgb) != 0;
    for (u32 level = 0; level < mip_count; ++level) {
        u32 size = 0;
        const usize expected = static_cast<usize>(texture.MipWidth(level)) * texture.MipHeight(level) * 4;
        if (!ReadU32(bytes, offset, size) || size != expected || offset + size > bytes.size()) {
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

nlohmann::json TextureImporter::DefaultSettings() const {
    return {{"srgb", true}, {"generate_mips", true}, {"max_size", 0}};
}

ImportResult TextureImporter::Import(const ImportContext& context) const {
    ImportResult result;
    const bool srgb = SettingOr<bool>(context.settings, "srgb", true, result.warnings, &nlohmann::json::is_boolean);
    const bool mips =
        SettingOr<bool>(context.settings, "generate_mips", true, result.warnings, &nlohmann::json::is_boolean);
    const i64 max_size =
        SettingOr<i64>(context.settings, "max_size", 0, result.warnings, &nlohmann::json::is_number_integer);

    ImageData image;
    if (!DecodeImageFile(context.source.string(), image)) {
        result.error = "not a readable image";
        return result;
    }

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

} // namespace aether::assets
