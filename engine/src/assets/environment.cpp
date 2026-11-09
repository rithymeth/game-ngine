#include "aether/assets/environment.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <thread>

namespace aether::assets {

namespace {

constexpr f32 kPi = 3.14159265358979f;

// A source mip pyramid (2x2 box downsamples) for filtered importance sampling.
struct Pyramid {
    std::vector<EnvironmentImage> levels;
};

Vec3 Texel(const EnvironmentImage& image, u32 x, u32 y) {
    const usize i = (static_cast<usize>(y) * image.width + x) * 3;
    return Vec3(image.rgb[i], image.rgb[i + 1], image.rgb[i + 2]);
}

// Bilinear, wrapping in u and clamping in v.
Vec3 Bilinear(const EnvironmentImage& image, f32 u, f32 v) {
    const f32 x = u * static_cast<f32>(image.width) - 0.5f, y = v * static_cast<f32>(image.height) - 0.5f;
    const f32 fx = std::floor(x), fy = std::floor(y);
    const f32 tx = x - fx, ty = y - fy;
    const i64 w = image.width, h = image.height;
    const auto wrap = [&](i64 i) { return static_cast<u32>(((i % w) + w) % w); };
    const auto clampy = [&](i64 i) { return static_cast<u32>(std::clamp<i64>(i, 0, h - 1)); };
    const u32 x0 = wrap(static_cast<i64>(fx)), x1 = wrap(static_cast<i64>(fx) + 1);
    const u32 y0 = clampy(static_cast<i64>(fy)), y1 = clampy(static_cast<i64>(fy) + 1);
    const Vec3 a = Texel(image, x0, y0) * (1.0f - tx) + Texel(image, x1, y0) * tx;
    const Vec3 b = Texel(image, x0, y1) * (1.0f - tx) + Texel(image, x1, y1) * tx;
    return a * (1.0f - ty) + b * ty;
}

Pyramid BuildPyramid(const EnvironmentImage& source) {
    Pyramid pyramid;
    pyramid.levels.push_back(source);
    while (pyramid.levels.back().width > 8 && pyramid.levels.back().height > 1) {
        const EnvironmentImage& from = pyramid.levels.back();
        EnvironmentImage to;
        to.width = std::max(1u, from.width / 2);
        to.height = std::max(1u, from.height / 2);
        to.rgb.resize(static_cast<usize>(to.width) * to.height * 3);
        for (u32 y = 0; y < to.height; ++y) {
            for (u32 x = 0; x < to.width; ++x) {
                const u32 x0 = std::min(x * 2, from.width - 1), x1 = std::min(x * 2 + 1, from.width - 1);
                const u32 y0 = std::min(y * 2, from.height - 1), y1 = std::min(y * 2 + 1, from.height - 1);
                const Vec3 c = (Texel(from, x0, y0) + Texel(from, x1, y0) + Texel(from, x0, y1) + Texel(from, x1, y1)) * 0.25f;
                const usize i = (static_cast<usize>(y) * to.width + x) * 3;
                to.rgb[i] = c.x, to.rgb[i + 1] = c.y, to.rgb[i + 2] = c.z;
            }
        }
        pyramid.levels.push_back(std::move(to));
    }
    return pyramid;
}

// Trilinear in the pyramid at a fractional level.
Vec3 SampleLod(const Pyramid& pyramid, const Vec3& direction, f32 lod) {
    f32 u = 0.0f, v = 0.0f;
    DirectionToEquirect(direction, u, v);
    const f32 top = static_cast<f32>(pyramid.levels.size() - 1);
    lod = std::clamp(lod, 0.0f, top);
    const u32 l0 = static_cast<u32>(lod);
    const u32 l1 = std::min<u32>(l0 + 1, static_cast<u32>(pyramid.levels.size() - 1));
    const f32 t = lod - static_cast<f32>(l0);
    const Vec3 a = Bilinear(pyramid.levels[l0], u, v);
    if (t <= 0.0f || l1 == l0) return a;
    return a * (1.0f - t) + Bilinear(pyramid.levels[l1], u, v) * t;
}

f32 RadicalInverse(u32 bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<f32>(bits) * 2.3283064365386963e-10f;
}

template <typename Fn>
void ParallelRows(u32 rows, Fn&& fn) {
    const u32 threads = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
    if (threads == 1 || rows < 16) {
        for (u32 y = 0; y < rows; ++y) fn(y);
        return;
    }
    std::vector<std::thread> pool;
    for (u32 t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            for (u32 y = t; y < rows; y += threads) fn(y);
        });
    }
    for (std::thread& thread : pool) thread.join();
}

void StoreHalf(std::vector<u16>& out, usize texel, const Vec3& c) {
    out[texel * 4 + 0] = FloatToHalf(c.x);
    out[texel * 4 + 1] = FloatToHalf(c.y);
    out[texel * 4 + 2] = FloatToHalf(c.z);
    out[texel * 4 + 3] = FloatToHalf(1.0f);
}

f32 SanitizeRadiance(f32 value) { return std::isfinite(value) ? std::clamp(value, 0.0f, 65000.0f) : 0.0f; }

EnvironmentImage Rotate(EnvironmentImage image, f32 rotation_degrees) {
    if (!std::isfinite(rotation_degrees) || std::fmod(rotation_degrees, 360.0f) == 0.0f || image.width == 0) return image;
    // Turning the sky by +a moves what was at direction u to u + a / 360: the output at u reads the input at u - a / 360.
    const f32 shift = rotation_degrees / 360.0f;
    EnvironmentImage out = image;
    for (u32 y = 0; y < image.height; ++y) {
        for (u32 x = 0; x < image.width; ++x) {
            const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(image.width) - shift;
            const f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(image.height);
            const Vec3 c = Bilinear(image, u - std::floor(u), v);
            const usize i = (static_cast<usize>(y) * image.width + x) * 3;
            out.rgb[i] = c.x, out.rgb[i + 1] = c.y, out.rgb[i + 2] = c.z;
        }
    }
    return out;
}

void ShBasis(const Vec3& n, f32 (&y)[9]) {
    y[0] = 0.282095f;
    y[1] = 0.488603f * n.y;
    y[2] = 0.488603f * n.z;
    y[3] = 0.488603f * n.x;
    y[4] = 1.092548f * n.x * n.y;
    y[5] = 1.092548f * n.y * n.z;
    y[6] = 0.315392f * (3.0f * n.z * n.z - 1.0f);
    y[7] = 1.092548f * n.x * n.z;
    y[8] = 0.546274f * (n.x * n.x - n.y * n.y);
}

} // namespace

u16 FloatToHalf(f32 value) {
    const u32 bits = std::bit_cast<u32>(value);
    const u32 sign = (bits >> 16) & 0x8000u;
    const u32 exponent = (bits >> 23) & 0xFFu;
    u32 mantissa = bits & 0x7FFFFFu;
    if (exponent == 0xFFu) return static_cast<u16>(sign | 0x7C00u | (mantissa != 0 ? 0x200u : 0u)); // inf / NaN
    const i32 e = static_cast<i32>(exponent) - 127 + 15;
    if (e >= 31) return static_cast<u16>(sign | 0x7C00u);
    if (e <= 0) {
        if (e < -10) return static_cast<u16>(sign);
        mantissa |= 0x800000u;
        const u32 shift = static_cast<u32>(14 - e);
        u32 half = mantissa >> shift;
        const u32 rest = mantissa & ((1u << shift) - 1u), halfway = 1u << (shift - 1);
        if (rest > halfway || (rest == halfway && (half & 1u))) ++half;
        return static_cast<u16>(sign | half);
    }
    u32 half = (static_cast<u32>(e) << 10) | (mantissa >> 13);
    const u32 rest = mantissa & 0x1FFFu;
    if (rest > 0x1000u || (rest == 0x1000u && (half & 1u))) ++half; // may carry into the exponent, which is right
    return static_cast<u16>(sign | half);
}

f32 HalfToFloat(u16 value) {
    const u32 sign = (static_cast<u32>(value) & 0x8000u) << 16;
    const u32 exponent = (value >> 10) & 0x1Fu;
    const u32 mantissa = value & 0x3FFu;
    if (exponent == 0) {
        const f32 m = static_cast<f32>(mantissa) / 1024.0f * (1.0f / 16384.0f);
        return sign ? -m : m;
    }
    if (exponent == 31) return std::bit_cast<f32>(sign | 0x7F800000u | (mantissa << 13));
    return std::bit_cast<f32>(sign | ((exponent - 15 + 127) << 23) | (mantissa << 13));
}

Vec3 EquirectToDirection(f32 u, f32 v) {
    const f32 phi = (u - 0.5f) * 2.0f * kPi, theta = v * kPi;
    return Vec3(std::sin(theta) * std::sin(phi), std::cos(theta), -std::sin(theta) * std::cos(phi));
}

void DirectionToEquirect(const Vec3& d, f32& u, f32& v) {
    const f32 length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    const f32 y = length > 0.0f ? std::clamp(d.y / length, -1.0f, 1.0f) : 1.0f;
    u = std::atan2(d.x, -d.z) / (2.0f * kPi) + 0.5f;
    v = std::acos(y) / kPi;
}

EnvironmentImage MakeProceduralSky(u32 width) {
    width = std::clamp(width, 8u, 4096u);
    EnvironmentImage sky;
    sky.width = width;
    sky.height = std::max(1u, width / 2);
    sky.rgb.resize(static_cast<usize>(sky.width) * sky.height * 3);
    const Vec3 zenith(0.14f, 0.28f, 0.60f), horizon(0.70f, 0.66f, 0.58f), ground(0.18f, 0.16f, 0.14f);
    for (u32 y = 0; y < sky.height; ++y) {
        const f32 up = std::cos((static_cast<f32>(y) + 0.5f) / static_cast<f32>(sky.height) * kPi);
        Vec3 c;
        if (up >= 0.0f) {
            const f32 t = std::sqrt(up);
            c = horizon + (zenith - horizon) * t;
        } else {
            const f32 g = std::min(-up * 3.0f, 1.0f);
            c = horizon + (ground - horizon) * g;
        }
        for (u32 x = 0; x < sky.width; ++x) {
            const usize i = (static_cast<usize>(y) * sky.width + x) * 3;
            sky.rgb[i] = c.x, sky.rgb[i + 1] = c.y, sky.rgb[i + 2] = c.z;
        }
    }
    return sky;
}

EnvironmentImage MakeEnvironment(u32 width, u32 height, const std::vector<f32>& rgb, f32 rotation_degrees) {
    if (width == 0 || height == 0 || rgb.size() != static_cast<usize>(width) * height * 3) return {};
    EnvironmentImage image;
    image.width = width;
    image.height = height;
    image.rgb.resize(rgb.size());
    for (usize i = 0; i < rgb.size(); ++i) image.rgb[i] = SanitizeRadiance(rgb[i]);
    return Rotate(std::move(image), rotation_degrees);
}

EnvironmentImage MakeEnvironmentFromSrgb(u32 width, u32 height, const std::vector<u8>& rgba8, f32 rotation_degrees) {
    if (width == 0 || height == 0 || rgba8.size() != static_cast<usize>(width) * height * 4) return {};
    f32 table[256];
    for (u32 i = 0; i < 256; ++i) {
        const f32 c = static_cast<f32>(i) / 255.0f;
        table[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }
    std::vector<f32> rgb(static_cast<usize>(width) * height * 3);
    for (usize p = 0; p < static_cast<usize>(width) * height; ++p) {
        for (usize c = 0; c < 3; ++c) rgb[p * 3 + c] = table[rgba8[p * 4 + c]];
    }
    return MakeEnvironment(width, height, rgb, rotation_degrees);
}

PrefilteredEnvironment PrefilterEnvironment(const EnvironmentImage& source, const PrefilterSettings& settings) {
    PrefilteredEnvironment out;
    if (source.width == 0 || source.height == 0 || source.rgb.size() != static_cast<usize>(source.width) * source.height * 3) return out;
    const Pyramid pyramid = BuildPyramid(source);

    // Level 0: the source resampled to a power-of-two width between 64 and max_width.
    const u32 max_width = std::max(64u, std::bit_floor(std::max(settings.max_width, 64u)));
    const u32 width = std::clamp(std::bit_floor(source.width), 64u, max_width);
    const u32 smallest = std::clamp(std::bit_floor(std::max(settings.smallest_width, 2u)), 2u, width);
    out.width = width;
    out.height = width / 2;
    out.mip_count = 1;
    while ((width >> out.mip_count) >= smallest) ++out.mip_count;
    usize total = 0;
    for (u32 i = 0; i < out.mip_count; ++i) total += static_cast<usize>(width >> i) * std::max(1u, (width / 2) >> i);
    out.halves.resize(total * 4);

    const f32 source_texel_solid_angle = 4.0f * kPi / (static_cast<f32>(source.width) * static_cast<f32>(source.height));
    const u32 samples = std::clamp(settings.samples, 1u, 4096u);
    usize offset = 0;
    for (u32 level = 0; level < out.mip_count; ++level) {
        const u32 w = width >> level, h = std::max(1u, (width / 2) >> level);
        const f32 roughness = out.mip_count > 1 ? static_cast<f32>(level) / static_cast<f32>(out.mip_count - 1) : 0.0f;
        const f32 alpha = roughness * roughness;
        // The GGX samples in tangent space (N = V = R = +Z), with the source level each one reads (filtered importance sampling).
        struct Sample {
            Vec3 l;
            f32 n_dot_l;
            f32 lod;
        };
        std::vector<Sample> lobe;
        if (level > 0) {
            for (u32 i = 0; i < samples; ++i) {
                const f32 xi_x = (static_cast<f32>(i) + 0.5f) / static_cast<f32>(samples), xi_y = RadicalInverse(i);
                const f32 phi = 2.0f * kPi * xi_x;
                const f32 cos_theta = std::sqrt((1.0f - xi_y) / (1.0f + (alpha * alpha - 1.0f) * xi_y));
                const f32 sin_theta = std::sqrt(std::max(0.0f, 1.0f - cos_theta * cos_theta));
                const Vec3 hv(sin_theta * std::cos(phi), sin_theta * std::sin(phi), cos_theta);
                const Vec3 l = hv * (2.0f * cos_theta) - Vec3(0, 0, 1); // reflect V = +Z about H
                if (l.z <= 0.0f) continue;
                const f32 a2 = alpha * alpha;
                const f32 d = cos_theta * cos_theta * (a2 - 1.0f) + 1.0f;
                const f32 ggx = a2 / (kPi * d * d);
                const f32 pdf = ggx * 0.25f; // D * NoH / (4 VoH), with N = V
                const f32 sample_solid_angle = 1.0f / (static_cast<f32>(samples) * pdf + 1e-6f);
                const f32 lod = 0.5f * std::log2(sample_solid_angle / source_texel_solid_angle) + 1.0f;
                lobe.push_back({l, l.z, lod});
            }
        }
        // Level 0 reads the pyramid at the level matching its texel size.
        const f32 base_lod = std::max(0.0f, std::log2(static_cast<f32>(source.width) / static_cast<f32>(w)));
        ParallelRows(h, [&](u32 y) {
            for (u32 x = 0; x < w; ++x) {
                const Vec3 n = EquirectToDirection((static_cast<f32>(x) + 0.5f) / static_cast<f32>(w), (static_cast<f32>(y) + 0.5f) / static_cast<f32>(h));
                Vec3 color;
                if (lobe.empty()) {
                    color = SampleLod(pyramid, n, base_lod);
                } else {
                    const Vec3 up = std::fabs(n.y) < 0.999f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
                    const Vec3 tx = up.Cross(n).Normalized();
                    const Vec3 ty = n.Cross(tx);
                    f32 weight = 0.0f;
                    for (const Sample& s : lobe) {
                        const Vec3 l = tx * s.l.x + ty * s.l.y + n * s.l.z;
                        color = color + SampleLod(pyramid, l, std::max(s.lod, base_lod)) * s.n_dot_l;
                        weight += s.n_dot_l;
                    }
                    color = weight > 0.0f ? color * (1.0f / weight) : SampleLod(pyramid, n, base_lod);
                }
                StoreHalf(out.halves, offset + static_cast<usize>(y) * w + x, color);
            }
        });
        offset += static_cast<usize>(w) * h;
    }

    // Irradiance SH from a small pyramid level (at most 128 wide): radiance projected with each texel's solid angle, then the cosine
    // lobe's band factors (pi, 2pi/3, pi/4).
    usize sh_level = 0;
    while (sh_level + 1 < pyramid.levels.size() && pyramid.levels[sh_level].width > 128) ++sh_level;
    const EnvironmentImage& small = pyramid.levels[sh_level];
    f64 sh[9][3]{};
    for (u32 y = 0; y < small.height; ++y) {
        const f32 theta = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(small.height) * kPi;
        const f32 solid_angle = (2.0f * kPi / static_cast<f32>(small.width)) * (kPi / static_cast<f32>(small.height)) * std::sin(theta);
        for (u32 x = 0; x < small.width; ++x) {
            const Vec3 n = EquirectToDirection((static_cast<f32>(x) + 0.5f) / static_cast<f32>(small.width), (static_cast<f32>(y) + 0.5f) / static_cast<f32>(small.height));
            const Vec3 c = Texel(small, x, y);
            f32 basis[9];
            ShBasis(n, basis);
            for (usize i = 0; i < 9; ++i) {
                sh[i][0] += static_cast<f64>(c.x * basis[i] * solid_angle);
                sh[i][1] += static_cast<f64>(c.y * basis[i] * solid_angle);
                sh[i][2] += static_cast<f64>(c.z * basis[i] * solid_angle);
            }
        }
    }
    const f32 band[9] = {kPi, 2.0f * kPi / 3.0f, 2.0f * kPi / 3.0f, 2.0f * kPi / 3.0f, kPi / 4.0f, kPi / 4.0f, kPi / 4.0f, kPi / 4.0f, kPi / 4.0f};
    for (usize i = 0; i < 9; ++i) {
        for (usize c = 0; c < 3; ++c) out.irradiance_sh[i][c] = static_cast<f32>(sh[i][c]) * band[i];
        out.irradiance_sh[i][3] = 0.0f;
    }
    return out;
}

Vec3 EvaluateIrradiance(const f32 (&sh)[9][4], const Vec3& n) {
    f32 basis[9];
    ShBasis(n, basis);
    Vec3 e;
    for (usize i = 0; i < 9; ++i) e = e + Vec3(sh[i][0], sh[i][1], sh[i][2]) * basis[i];
    return Vec3(std::max(e.x, 0.0f), std::max(e.y, 0.0f), std::max(e.z, 0.0f));
}

namespace {

constexpr char kEnvironmentMagic[4] = {'A', 'E', 'N', 'V'};
constexpr usize kEnvironmentHeaderBytes = 4 + 4 * 7 + sizeof(f32) * 9 * 4;
constexpr u32 kMaxEnvironmentWidth = 16384;

void PutU32(std::vector<u8>& out, u32 value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(value >> (8 * i)));
}

u32 GetU32(std::span<const u8> bytes, usize at) {
    return static_cast<u32>(bytes[at]) | static_cast<u32>(bytes[at + 1]) << 8 | static_cast<u32>(bytes[at + 2]) << 16 |
           static_cast<u32>(bytes[at + 3]) << 24;
}

// The half-float texels of a chain of `levels` levels whose first is width x width / 2, as PrefilterEnvironment lays it out.
usize EnvironmentTexels(u32 width, u32 levels) {
    usize total = 0;
    for (u32 i = 0; i < levels; ++i) total += static_cast<usize>(width >> i) * std::max(1u, (width / 2) >> i);
    return total;
}

} // namespace

std::vector<u8> SaveEnvironment(const PrefilteredEnvironment& environment, const PrefilterSettings& settings) {
    std::vector<u8> out;
    if (environment.mip_count == 0 || environment.halves.size() != EnvironmentTexels(environment.width, environment.mip_count) * 4) return out;
    out.reserve(kEnvironmentHeaderBytes + environment.halves.size() * 2);
    out.insert(out.end(), kEnvironmentMagic, kEnvironmentMagic + 4);
    PutU32(out, kEnvironmentFormatVersion);
    PutU32(out, settings.max_width);
    PutU32(out, settings.smallest_width);
    PutU32(out, settings.samples);
    PutU32(out, environment.width);
    PutU32(out, environment.height);
    PutU32(out, environment.mip_count);
    for (const auto& coefficient : environment.irradiance_sh) {
        for (const f32 value : coefficient) {
            u32 bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            PutU32(out, bits);
        }
    }
    for (const u16 half : environment.halves) {
        out.push_back(static_cast<u8>(half));
        out.push_back(static_cast<u8>(half >> 8));
    }
    return out;
}

bool LoadEnvironment(std::span<const u8> bytes, PrefilteredEnvironment& out, PrefilterSettings* settings, std::string* error) {
    const auto fail = [&](const char* message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (bytes.size() < kEnvironmentHeaderBytes || std::memcmp(bytes.data(), kEnvironmentMagic, 4) != 0) return fail("not an .aenv file");
    if (GetU32(bytes, 4) != kEnvironmentFormatVersion) return fail("an .aenv of another version");
    PrefilterSettings read;
    read.max_width = GetU32(bytes, 8);
    read.smallest_width = GetU32(bytes, 12);
    read.samples = GetU32(bytes, 16);
    const u32 width = GetU32(bytes, 20), height = GetU32(bytes, 24), levels = GetU32(bytes, 28);
    // Level 0 is a power of two, at least 64 wide and twice as wide as tall; the chain ends no smaller than 2 wide.
    if (width < 64 || width > kMaxEnvironmentWidth || !std::has_single_bit(width) || height != width / 2) return fail("bad environment size");
    if (levels == 0 || levels > 32 || (width >> (levels - 1)) < 2) return fail("bad level count");
    const usize halves = EnvironmentTexels(width, levels) * 4;
    if (bytes.size() != kEnvironmentHeaderBytes + halves * 2) return fail("size does not match the levels");
    PrefilteredEnvironment result;
    result.width = width;
    result.height = height;
    result.mip_count = levels;
    usize at = 32;
    for (auto& coefficient : result.irradiance_sh) {
        for (f32& value : coefficient) {
            const u32 bits = GetU32(bytes, at);
            at += 4;
            std::memcpy(&value, &bits, sizeof(value));
            if (!std::isfinite(value)) return fail("a non-finite SH coefficient");
        }
    }
    result.halves.resize(halves);
    for (usize i = 0; i < halves; ++i) result.halves[i] = static_cast<u16>(bytes[at + i * 2] | bytes[at + i * 2 + 1] << 8);
    if (settings != nullptr) *settings = read;
    out = std::move(result);
    return true;
}

} // namespace aether::assets
