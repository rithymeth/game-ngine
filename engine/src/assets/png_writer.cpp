#include "aether/assets/png_writer.h"

#include <algorithm>
#include <array>

namespace aether::assets {

namespace {

u32 Crc32(const u8* data, usize size, u32 crc = 0xFFFFFFFFu) {
    static const std::array<u32, 256> table = [] {
        std::array<u32, 256> t{};
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    for (usize i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc;
}

void Put32(std::vector<u8>& out, u32 v) {
    out.push_back(static_cast<u8>(v >> 24));
    out.push_back(static_cast<u8>(v >> 16));
    out.push_back(static_cast<u8>(v >> 8));
    out.push_back(static_cast<u8>(v));
}

void Chunk(std::vector<u8>& out, const char type[4], const std::vector<u8>& data) {
    Put32(out, static_cast<u32>(data.size()));
    const usize start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    Put32(out, ~Crc32(out.data() + start, out.size() - start));
}

} // namespace

namespace {
std::vector<u8> EncodePng(u32 width, u32 height, std::span<const u8> pixels, u32 channels) {
    const std::span<const u8> rgb = pixels;
    if (width == 0 || height == 0 || rgb.size() < static_cast<usize>(width) * height * channels) return {};
    // The scanlines: a filter byte (0, none) before each row.
    std::vector<u8> raw;
    raw.reserve((static_cast<usize>(width) * channels + 1) * height);
    for (u32 y = 0; y < height; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb.begin() + static_cast<std::ptrdiff_t>(static_cast<usize>(y) * width * channels), rgb.begin() + static_cast<std::ptrdiff_t>((static_cast<usize>(y) + 1) * width * channels));
    }
    // zlib stream: header, stored deflate blocks of at most 65,535 bytes, the Adler-32 of the data.
    std::vector<u8> z = {0x78, 0x01};
    u32 a = 1, b = 0;
    for (const u8 byte : raw) {
        a = (a + byte) % 65521u;
        b = (b + a) % 65521u;
    }
    for (usize pos = 0; pos < raw.size();) {
        const usize n = std::min<usize>(65535, raw.size() - pos);
        const bool last = pos + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<u8>(n & 0xFF));
        z.push_back(static_cast<u8>(n >> 8));
        z.push_back(static_cast<u8>(~n & 0xFF));
        z.push_back(static_cast<u8>((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos), raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
    }
    Put32(z, (b << 16) | a);

    std::vector<u8> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<u8> header;
    Put32(header, width);
    Put32(header, height);
    header.insert(header.end(), {8, static_cast<u8>(channels == 4 ? 6 : 2), 0, 0, 0}); // 8 bits, colour type 2 (RGB) or 6 (RGBA), deflate, no filter method, no interlace
    Chunk(png, "IHDR", header);
    Chunk(png, "IDAT", z);
    Chunk(png, "IEND", {});
    return png;
}
} // namespace

std::vector<u8> EncodePngRgb8(u32 width, u32 height, std::span<const u8> rgb) { return EncodePng(width, height, rgb, 3); }
std::vector<u8> EncodePngRgba8(u32 width, u32 height, std::span<const u8> rgba) { return EncodePng(width, height, rgba, 4); }

} // namespace aether::assets
