#include "aether/cook/texture_cook.h"

#include <bc7decomp.h>
#include <bc7enc.h>
#include <rgbcx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>

namespace aether::cook {

namespace {

constexpr char kMagic[4] = {'A', 'T', 'E', 'X'};
constexpr u32 kVersion = 1;

void InitEncoders() {
    static std::once_flag once;
    std::call_once(once, [] {
        rgbcx::init();
        bc7enc_compress_block_init();
    });
}

f32 SrgbToLinear(u8 v) {
    const f32 c = static_cast<f32>(v) / 255.0f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

u8 LinearToSrgb(f32 c) {
    c = std::clamp(c, 0.0f, 1.0f);
    const f32 s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return static_cast<u8>(std::lround(std::clamp(s, 0.0f, 1.0f) * 255.0f));
}

const std::array<f32, 256>& SrgbTable() {
    static const std::array<f32, 256> table = [] {
        std::array<f32, 256> t{};
        for (u32 i = 0; i < 256; ++i) t[i] = SrgbToLinear(static_cast<u8>(i));
        return t;
    }();
    return table;
}

void Put32(std::vector<u8>& out, u32 v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
}
u32 Get32(const u8* p) {
    return static_cast<u32>(p[0]) | static_cast<u32>(p[1]) << 8 | static_cast<u32>(p[2]) << 16 |
           static_cast<u32>(p[3]) << 24;
}

// The 4x4 RGBA block at (bx, by), edge pixels repeated past the image.
void FetchBlock(const std::vector<u8>& rgba, u32 width, u32 height, u32 bx, u32 by, u8 out[64]) {
    for (u32 y = 0; y < 4; ++y) {
        const u32 sy = std::min(by * 4 + y, height - 1);
        for (u32 x = 0; x < 4; ++x) {
            const u32 sx = std::min(bx * 4 + x, width - 1);
            std::memcpy(out + (y * 4 + x) * 4, &rgba[(static_cast<usize>(sy) * width + sx) * 4], 4);
        }
    }
}

std::vector<u8> EncodeMip(const CookedMip& mip, TextureFormat format, u32 quality, bool srgb) {
    if (format == TextureFormat::RGBA8) return mip.data;
    const u32 bw = (mip.width + 3) / 4, bh = (mip.height + 3) / 4;
    const u32 block_bytes = BlockBytes(format);
    std::vector<u8> out(static_cast<usize>(bw) * bh * block_bytes);
    bc7enc_compress_block_params bc7;
    bc7enc_compress_block_params_init(&bc7);
    bc7.m_uber_level = std::min<u32>(quality, BC7ENC_MAX_UBER_LEVEL);
    bc7.m_max_partitions = quality == 0 ? 0 : BC7ENC_MAX_PARTITIONS;
    if (!srgb) bc7enc_compress_block_params_init_linear_weights(&bc7);
    const u32 level = std::min<u32>(quality * 4 + 2, 18); // rgbcx levels 0..18
    u8 block[64];
    for (u32 by = 0; by < bh; ++by) {
        for (u32 bx = 0; bx < bw; ++bx) {
            FetchBlock(mip.data, mip.width, mip.height, bx, by, block);
            u8* dst = &out[(static_cast<usize>(by) * bw + bx) * block_bytes];
            switch (format) {
                case TextureFormat::BC1: rgbcx::encode_bc1(level, dst, block, true, false); break;
                case TextureFormat::BC3: rgbcx::encode_bc3(level, dst, block); break;
                case TextureFormat::BC4: rgbcx::encode_bc4(dst, block); break;
                case TextureFormat::BC5: rgbcx::encode_bc5(dst, block); break;
                case TextureFormat::BC7: bc7enc_compress_block(dst, block, &bc7); break;
                case TextureFormat::RGBA8: break;
            }
        }
    }
    return out;
}

} // namespace

const char* TextureFormatName(TextureFormat f) {
    switch (f) {
        case TextureFormat::RGBA8: return "rgba8";
        case TextureFormat::BC1: return "bc1";
        case TextureFormat::BC3: return "bc3";
        case TextureFormat::BC4: return "bc4";
        case TextureFormat::BC5: return "bc5";
        case TextureFormat::BC7: return "bc7";
    }
    return "?";
}

bool ParseTextureFormat(const std::string& name, TextureFormat& out) {
    for (TextureFormat f : {TextureFormat::RGBA8, TextureFormat::BC1, TextureFormat::BC3, TextureFormat::BC4,
                            TextureFormat::BC5, TextureFormat::BC7}) {
        std::string lower;
        for (char c : name) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == TextureFormatName(f)) {
            out = f;
            return true;
        }
    }
    return false;
}

u32 BlockBytes(TextureFormat f) {
    switch (f) {
        case TextureFormat::BC1:
        case TextureFormat::BC4: return 8;
        case TextureFormat::BC3:
        case TextureFormat::BC5:
        case TextureFormat::BC7: return 16;
        case TextureFormat::RGBA8: return 0;
    }
    return 0;
}

usize MipBytes(TextureFormat f, u32 width, u32 height) {
    if (f == TextureFormat::RGBA8) return static_cast<usize>(width) * height * 4;
    return static_cast<usize>((width + 3) / 4) * ((height + 3) / 4) * BlockBytes(f);
}

TextureFormat ChooseTextureFormat(std::span<const u8> rgba, bool normal_map) {
    if (normal_map) return TextureFormat::BC5;
    bool grey = true;
    for (usize i = 0; i + 3 < rgba.size(); i += 4) {
        if (rgba[i + 3] != 255) return TextureFormat::BC7; // has alpha
        if (rgba[i] != rgba[i + 1] || rgba[i] != rgba[i + 2]) grey = false;
    }
    return grey ? TextureFormat::BC4 : TextureFormat::BC7;
}

std::vector<CookedMip> GenerateMips(std::span<const u8> rgba, u32 width, u32 height, bool srgb, bool normal_map) {
    std::vector<CookedMip> mips;
    mips.push_back({width, height, std::vector<u8>(rgba.begin(), rgba.end())});
    const auto& to_linear = SrgbTable();
    while (mips.back().width > 1 || mips.back().height > 1) {
        const CookedMip& src = mips.back();
        CookedMip dst;
        dst.width = std::max(1u, src.width / 2);
        dst.height = std::max(1u, src.height / 2);
        dst.data.resize(static_cast<usize>(dst.width) * dst.height * 4);
        for (u32 y = 0; y < dst.height; ++y) {
            for (u32 x = 0; x < dst.width; ++x) {
                f32 sum[4] = {0, 0, 0, 0};
                u32 n = 0;
                // A 2x2 box (a 1-wide source collapses only the other way).
                for (u32 dy = 0; dy < 2; ++dy) {
                    for (u32 dx = 0; dx < 2; ++dx) {
                        const u32 sx = std::min(x * 2 + dx, src.width - 1);
                        const u32 sy = std::min(y * 2 + dy, src.height - 1);
                        const u8* p = &src.data[(static_cast<usize>(sy) * src.width + sx) * 4];
                        for (int c = 0; c < 3; ++c) {
                            sum[c] += normal_map ? p[c] / 127.5f - 1.0f
                                      : srgb     ? to_linear[p[c]]
                                                 : p[c] / 255.0f;
                        }
                        sum[3] += p[3] / 255.0f;
                        ++n;
                    }
                }
                u8* out = &dst.data[(static_cast<usize>(y) * dst.width + x) * 4];
                if (normal_map) {
                    f32 nx = sum[0] / n, ny = sum[1] / n, nz = sum[2] / n;
                    const f32 len = std::sqrt(nx * nx + ny * ny + nz * nz);
                    if (len > 1e-6f) nx /= len, ny /= len, nz /= len;
                    else nx = 0, ny = 0, nz = 1;
                    out[0] = static_cast<u8>(std::lround((nx + 1.0f) * 127.5f));
                    out[1] = static_cast<u8>(std::lround((ny + 1.0f) * 127.5f));
                    out[2] = static_cast<u8>(std::lround((nz + 1.0f) * 127.5f));
                } else {
                    for (int c = 0; c < 3; ++c) {
                        const f32 v = sum[c] / n;
                        out[c] = srgb ? LinearToSrgb(v) : static_cast<u8>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
                    }
                }
                out[3] = static_cast<u8>(std::lround(std::clamp(sum[3] / n, 0.0f, 1.0f) * 255.0f));
            }
        }
        mips.push_back(std::move(dst));
    }
    return mips;
}

CookedTexture CookTexture(std::span<const u8> rgba, u32 width, u32 height, const TextureCookSettings& settings) {
    InitEncoders();
    CookedTexture texture;
    texture.width = width;
    texture.height = height;
    texture.normal_map = settings.normal_map;
    texture.format = settings.auto_format ? ChooseTextureFormat(rgba, settings.normal_map) : settings.format;
    // Single- and two-channel formats hold data, not colour.
    texture.srgb = settings.srgb && !settings.normal_map && texture.format != TextureFormat::BC4 &&
                   texture.format != TextureFormat::BC5;
    if (width == 0 || height == 0 || rgba.size() < static_cast<usize>(width) * height * 4) return texture;
    std::vector<CookedMip> levels;
    if (settings.mips) {
        levels = GenerateMips(rgba, width, height, texture.srgb, settings.normal_map);
    } else {
        levels.push_back({width, height, std::vector<u8>(rgba.begin(), rgba.begin() + static_cast<usize>(width) * height * 4)});
    }
    for (CookedMip& level : levels) {
        CookedMip mip{level.width, level.height, EncodeMip(level, texture.format, settings.quality, texture.srgb)};
        texture.mips.push_back(std::move(mip));
    }
    return texture;
}

std::vector<u8> DecodeMip(const CookedTexture& texture, usize index) {
    if (index >= texture.mips.size()) return {};
    const CookedMip& mip = texture.mips[index];
    if (texture.format == TextureFormat::RGBA8) return mip.data;
    InitEncoders();
    std::vector<u8> out(static_cast<usize>(mip.width) * mip.height * 4);
    const u32 bw = (mip.width + 3) / 4, bh = (mip.height + 3) / 4;
    const u32 block_bytes = BlockBytes(texture.format);
    u8 block[64];
    for (u32 by = 0; by < bh; ++by) {
        for (u32 bx = 0; bx < bw; ++bx) {
            const u8* src = &mip.data[(static_cast<usize>(by) * bw + bx) * block_bytes];
            std::memset(block, 0, sizeof(block));
            switch (texture.format) {
                case TextureFormat::BC1: rgbcx::unpack_bc1(src, block, true); break;
                case TextureFormat::BC3: rgbcx::unpack_bc3(src, block); break;
                case TextureFormat::BC4:
                    rgbcx::unpack_bc4(src, block, 4);
                    for (int i = 0; i < 16; ++i) block[i * 4 + 1] = block[i * 4 + 2] = block[i * 4], block[i * 4 + 3] = 255;
                    break;
                case TextureFormat::BC5:
                    rgbcx::unpack_bc5(src, block, 0, 1, 4);
                    for (int i = 0; i < 16; ++i) {
                        u8* p = block + i * 4;
                        if (texture.normal_map) {
                            const f32 nx = p[0] / 127.5f - 1.0f, ny = p[1] / 127.5f - 1.0f;
                            const f32 nz = std::sqrt(std::max(0.0f, 1.0f - nx * nx - ny * ny));
                            p[2] = static_cast<u8>(std::lround((nz + 1.0f) * 127.5f));
                        } else {
                            p[2] = 0;
                        }
                        p[3] = 255;
                    }
                    break;
                case TextureFormat::BC7:
                    bc7decomp::unpack_bc7(src, reinterpret_cast<bc7decomp::color_rgba*>(block));
                    break;
                case TextureFormat::RGBA8: break;
            }
            for (u32 y = 0; y < 4; ++y) {
                for (u32 x = 0; x < 4; ++x) {
                    const u32 px = bx * 4 + x, py = by * 4 + y;
                    if (px < mip.width && py < mip.height) {
                        std::memcpy(&out[(static_cast<usize>(py) * mip.width + px) * 4], block + (y * 4 + x) * 4, 4);
                    }
                }
            }
        }
    }
    return out;
}

std::vector<u8> SaveAtex(const CookedTexture& texture) {
    std::vector<u8> out(kMagic, kMagic + 4);
    Put32(out, kVersion);
    out.push_back(static_cast<u8>(texture.format));
    out.push_back(static_cast<u8>((texture.srgb ? 1 : 0) | (texture.normal_map ? 2 : 0)));
    out.push_back(0);
    out.push_back(0);
    Put32(out, texture.width);
    Put32(out, texture.height);
    Put32(out, static_cast<u32>(texture.mips.size()));
    for (const CookedMip& mip : texture.mips) {
        Put32(out, mip.width);
        Put32(out, mip.height);
        Put32(out, static_cast<u32>(mip.data.size()));
        out.insert(out.end(), mip.data.begin(), mip.data.end());
    }
    return out;
}

bool LoadAtex(std::span<const u8> bytes, CookedTexture& out, std::string* error) {
    const auto fail = [&](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (bytes.size() < 24 || std::memcmp(bytes.data(), kMagic, 4) != 0) return fail("not an .atex texture");
    if (Get32(bytes.data() + 4) != kVersion) return fail("unsupported .atex version");
    CookedTexture t;
    if (bytes[8] > static_cast<u8>(TextureFormat::BC7)) return fail("unknown texture format");
    t.format = static_cast<TextureFormat>(bytes[8]);
    t.srgb = (bytes[9] & 1) != 0;
    t.normal_map = (bytes[9] & 2) != 0;
    t.width = Get32(bytes.data() + 12);
    t.height = Get32(bytes.data() + 16);
    const u32 count = Get32(bytes.data() + 20);
    usize p = 24;
    for (u32 i = 0; i < count; ++i) {
        if (bytes.size() - p < 12) return fail("the mip chain ends early");
        CookedMip mip;
        mip.width = Get32(bytes.data() + p);
        mip.height = Get32(bytes.data() + p + 4);
        const u32 size = Get32(bytes.data() + p + 8);
        p += 12;
        if (mip.width == 0 || mip.height == 0 || size != MipBytes(t.format, mip.width, mip.height) ||
            bytes.size() - p < size) {
            return fail("a mip's size doesn't match its format");
        }
        mip.data.assign(bytes.begin() + static_cast<std::ptrdiff_t>(p), bytes.begin() + static_cast<std::ptrdiff_t>(p + size));
        p += size;
        t.mips.push_back(std::move(mip));
    }
    out = std::move(t);
    return true;
}

} // namespace aether::cook
