#include "aether/cook/texture_cook.h"
#include "test_framework.h"

#include <cmath>
#include <string>
#include <vector>

// Phase 25 step 3: cooked textures - block compression (BC1/3/4/5/7) with
// their quality held to PSNR bounds, mips (sRGB-correct, normal maps
// renormalized), the automatic format choice, and the .atex container.

using namespace aether;
using namespace aether::cook;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

std::vector<u8> Gradient(u32 w, u32 h, bool alpha) {
    std::vector<u8> p(static_cast<usize>(w) * h * 4);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            u8* px = &p[(static_cast<usize>(y) * w + x) * 4];
            px[0] = static_cast<u8>(x * 255 / std::max(1u, w - 1));
            px[1] = static_cast<u8>(y * 255 / std::max(1u, h - 1));
            px[2] = static_cast<u8>((x + y) * 255 / std::max(1u, w + h - 2));
            px[3] = alpha ? static_cast<u8>(255 - x * 255 / std::max(1u, w - 1)) : 255;
        }
    }
    return p;
}

f64 Psnr(const std::vector<u8>& a, const std::vector<u8>& b, int channels) {
    f64 se = 0;
    usize n = 0;
    for (usize i = 0; i < a.size(); i += 4) {
        for (int c = 0; c < channels; ++c) {
            const f64 d = static_cast<f64>(a[i + c]) - b[i + c];
            se += d * d;
            ++n;
        }
    }
    const f64 mse = se / static_cast<f64>(n);
    return mse <= 1e-9 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

TextureCookSettings Fixed(TextureFormat f) {
    TextureCookSettings s;
    s.auto_format = false;
    s.format = f;
    return s;
}

} // namespace

AETHER_TEST(TextureCook_FormatsAndSizes) {
    TextureFormat f{};
    CHECK(ParseTextureFormat("BC7", f) && f == TextureFormat::BC7 && ParseTextureFormat("rgba8", f) &&
          f == TextureFormat::RGBA8);
    CHECK(!ParseTextureFormat("astc", f));
    CHECK(BlockBytes(TextureFormat::BC1) == 8 && BlockBytes(TextureFormat::BC4) == 8 &&
          BlockBytes(TextureFormat::BC5) == 16 && BlockBytes(TextureFormat::RGBA8) == 0);
    CHECK(MipBytes(TextureFormat::BC1, 64, 64) == 64 * 64 / 2);        // 4 bpp
    CHECK(MipBytes(TextureFormat::BC7, 64, 64) == 64 * 64);            // 8 bpp
    CHECK(MipBytes(TextureFormat::BC7, 5, 3) == 2 * 1 * 16);           // partial blocks round up
    CHECK(MipBytes(TextureFormat::RGBA8, 5, 3) == 5 * 3 * 4);
}

AETHER_TEST(TextureCook_MipsFilterInTheRightSpace) {
    // A 2x2 of black and white: its mip is mid-grey in linear light, which
    // in sRGB is ~188, not 128.
    const std::vector<u8> checker = {0, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0, 0, 0, 255};
    std::vector<CookedMip> mips = GenerateMips(checker, 2, 2, true, false);
    CHECK(mips.size() == 2 && mips[1].width == 1 && mips[1].height == 1);
    CHECK(mips[1].data[0] >= 186 && mips[1].data[0] <= 190 && mips[1].data[3] == 255);
    mips = GenerateMips(checker, 2, 2, false, false);
    CHECK(mips[1].data[0] == 128);

    // Non-square, non-power-of-two chains run down to 1x1.
    const std::vector<u8> img = Gradient(10, 3, false);
    mips = GenerateMips(img, 10, 3, true, false);
    CHECK(mips.size() == 4 && mips[1].width == 5 && mips[1].height == 1 && mips[3].width == 1 && mips[3].height == 1);

    // Normal maps: averaged normals come back unit length.
    std::vector<u8> normals = {255, 128, 128, 255, 128, 255, 128, 255, 128, 128, 255, 255, 0, 128, 128, 255};
    mips = GenerateMips(normals, 2, 2, false, true);
    const u8* n = mips[1].data.data();
    const f32 nx = n[0] / 127.5f - 1, ny = n[1] / 127.5f - 1, nz = n[2] / 127.5f - 1;
    CHECK(std::fabs(std::sqrt(nx * nx + ny * ny + nz * nz) - 1.0f) < 0.02f);
}

AETHER_TEST(TextureCook_BlockCompressionQuality) {
    const std::vector<u8> rgb = Gradient(64, 64, false);
    const std::vector<u8> rgba = Gradient(64, 64, true);
    struct Case { TextureFormat format; const std::vector<u8>* src; int channels; f64 min_psnr; };
    const Case cases[] = {
        {TextureFormat::BC1, &rgb, 3, 32.0},
        {TextureFormat::BC3, &rgba, 4, 32.0},
        {TextureFormat::BC7, &rgba, 4, 40.0},
        {TextureFormat::BC5, &rgb, 2, 38.0},
        {TextureFormat::BC4, &rgb, 1, 38.0},
        {TextureFormat::RGBA8, &rgba, 4, 99.0},
    };
    for (const Case& c : cases) {
        TextureCookSettings settings = Fixed(c.format);
        settings.mips = false;
        settings.srgb = false;
        const CookedTexture t = CookTexture(*c.src, 64, 64, settings);
        CHECK(t.format == c.format && t.mips.size() == 1);
        CHECK(t.mips[0].data.size() == MipBytes(c.format, 64, 64));
        const f64 psnr = Psnr(*c.src, DecodeMip(t, 0), c.channels);
        if (psnr < c.min_psnr) std::printf("    %s: %.1f dB < %.1f\n", TextureFormatName(c.format), psnr, c.min_psnr);
        CHECK(psnr >= c.min_psnr);
    }

    // Odd sizes: partial edge blocks, every mip decodes to its own size.
    const std::vector<u8> odd = Gradient(13, 7, true);
    const CookedTexture t = CookTexture(odd, 13, 7, Fixed(TextureFormat::BC7));
    CHECK(t.mips.size() == 4 && t.mips[0].data.size() == 4 * 2 * 16);
    for (usize m = 0; m < t.mips.size(); ++m) {
        CHECK(DecodeMip(t, m).size() == static_cast<usize>(t.mips[m].width) * t.mips[m].height * 4);
    }
    // Every channel changes ~20-40 levels a pixel here, near the worst a 4x4
    // block can hold; a smooth image of an odd size holds BC7's usual quality,
    // which shows the partial edge blocks are right.
    CHECK(Psnr(odd, DecodeMip(t, 0), 4) > 28.0);
    const std::vector<u8> smooth = Gradient(61, 37, true);
    TextureCookSettings linear = Fixed(TextureFormat::BC7);
    linear.srgb = false;
    CHECK(Psnr(smooth, DecodeMip(CookTexture(smooth, 61, 37, linear), 0), 4) > 38.0);
    CHECK(DecodeMip(t, 99).empty());
}

AETHER_TEST(TextureCook_AutoFormatAndContainer) {
    std::vector<u8> grey = Gradient(8, 8, false);
    for (usize i = 0; i < grey.size(); i += 4) grey[i + 1] = grey[i + 2] = grey[i];
    CHECK(ChooseTextureFormat(grey, false) == TextureFormat::BC4);
    CHECK(ChooseTextureFormat(Gradient(8, 8, false), false) == TextureFormat::BC7);
    CHECK(ChooseTextureFormat(Gradient(8, 8, true), false) == TextureFormat::BC7);
    CHECK(ChooseTextureFormat(grey, true) == TextureFormat::BC5);

    TextureCookSettings normal;
    normal.normal_map = true;
    std::vector<u8> flat(16 * 16 * 4);
    for (usize i = 0; i < flat.size(); i += 4) flat[i] = 128, flat[i + 1] = 128, flat[i + 2] = 255, flat[i + 3] = 255;
    const CookedTexture nm = CookTexture(flat, 16, 16, normal);
    CHECK(nm.format == TextureFormat::BC5 && nm.normal_map && !nm.srgb);
    const std::vector<u8> decoded = DecodeMip(nm, 0);
    CHECK(decoded[2] >= 250); // z rebuilt from x and y

    const CookedTexture colour = CookTexture(Gradient(32, 16, true), 32, 16, TextureCookSettings{});
    CHECK(colour.format == TextureFormat::BC7 && colour.srgb && colour.mips.size() == 6);
    const std::vector<u8> bytes = SaveAtex(colour);
    CookedTexture loaded;
    std::string error;
    CHECK(LoadAtex(bytes, loaded, &error));
    CHECK(loaded.format == colour.format && loaded.srgb && loaded.width == 32 && loaded.height == 16 &&
          loaded.mips.size() == 6 && loaded.mips[2].data == colour.mips[2].data);

    std::vector<u8> bad = bytes;
    bad[0] = 'X';
    CHECK(!LoadAtex(bad, loaded, &error) && error.find("not an .atex") != std::string::npos);
    bad = bytes;
    bad.resize(bad.size() - 3);
    CHECK(!LoadAtex(bad, loaded, &error));
    bad = bytes;
    bad[8] = 77; // format
    CHECK(!LoadAtex(bad, loaded, &error) && error.find("format") != std::string::npos);
}
