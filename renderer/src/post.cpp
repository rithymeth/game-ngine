#include "aether/renderer/post.h"

#include <algorithm>
#include <cmath>

namespace aether {

namespace {

// out = M * c, with M given row by row.
Vec3 Mul(const f32 m[3][3], const Vec3& c) {
    return Vec3(m[0][0] * c.x + m[0][1] * c.y + m[0][2] * c.z, m[1][0] * c.x + m[1][1] * c.y + m[1][2] * c.z,
                m[2][0] * c.x + m[2][1] * c.y + m[2][2] * c.z);
}

f32 Saturate(f32 v) { return std::clamp(v, 0.0f, 1.0f); }

} // namespace

// --- Auto exposure ------------------------------------------------------------------------

f32 Luminance(const Vec3& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

std::vector<u32> LuminanceHistogram(const std::vector<f32>& luminances, const ExposureSettings& s) {
    // Bin 0 is black; bins 1..n-1 span [min_log2, max_log2].
    std::vector<u32> histogram(std::max(s.bins, 3u), 0);
    const f32 range = s.max_log2 - s.min_log2;
    const u32 inner = static_cast<u32>(histogram.size()) - 1;
    for (const f32 l : luminances) {
        if (!(l > 1e-6f)) {
            ++histogram[0];
            continue;
        }
        const f32 t = std::clamp((std::log2(l) - s.min_log2) / range, 0.0f, 1.0f);
        ++histogram[1 + std::min(inner - 1, static_cast<u32>(t * static_cast<f32>(inner)))];
    }
    return histogram;
}

f32 AverageLuminance(const std::vector<u32>& histogram, const ExposureSettings& s) {
    u64 total = 0;
    for (u32 n : histogram) total += n;
    if (total == 0 || histogram.size() < 3) return std::exp2(s.min_log2);
    const f64 lo = static_cast<f64>(total) * std::clamp(s.low_percent, 0.0f, 1.0f);
    const f64 hi = static_cast<f64>(total) * std::clamp(s.high_percent, s.low_percent, 1.0f);
    const u32 inner = static_cast<u32>(histogram.size()) - 1;
    f64 acc = 0.0, weight = 0.0, weighted_log = 0.0;
    for (u32 i = 0; i < histogram.size(); ++i) {
        const f64 start = acc, end = acc + histogram[i];
        acc = end;
        const f64 take = std::max(0.0, std::min(end, hi) - std::max(start, lo));
        if (take <= 0.0 || i == 0) continue; // black pixels don't pull the average down
        const f64 center = s.min_log2 + (static_cast<f64>(i - 1) + 0.5) / inner * (s.max_log2 - s.min_log2);
        weighted_log += take * center;
        weight += take;
    }
    return weight > 0.0 ? static_cast<f32>(std::exp2(weighted_log / weight)) : std::exp2(s.min_log2);
}

f32 EV100FromLuminance(f32 average_luminance) {
    return std::log2(std::max(average_luminance, 1e-8f) * 100.0f / 12.5f);
}

f32 ExposureFromEV100(f32 ev100, f32 compensation) {
    return std::exp2(compensation) / (1.2f * std::exp2(ev100));
}

f32 AdaptEV100(f32 current, f32 target, f32 dt, const ExposureSettings& s) {
    target = std::clamp(target, s.min_ev100, s.max_ev100);
    const f32 rate = target > current ? s.speed_up : s.speed_down;
    return current + (target - current) * (1.0f - std::exp(-std::max(dt, 0.0f) * rate));
}

// --- Tone mapping ------------------------------------------------------------------------------

Vec3 TonemapACES(const Vec3& linear) {
    static const f32 kIn[3][3] = {{0.59719f, 0.35458f, 0.04823f}, {0.07600f, 0.90834f, 0.01566f}, {0.02840f, 0.13383f, 0.83777f}};
    static const f32 kOut[3][3] = {{1.60475f, -0.53108f, -0.07367f}, {-0.10208f, 1.10813f, -0.00605f}, {-0.00327f, -0.07276f, 1.07602f}};
    auto fit = [](f32 v) {
        const f32 a = v * (v + 0.0245786f) - 0.000090537f;
        const f32 b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
        return a / b;
    };
    Vec3 c = Mul(kIn, Vec3(std::max(linear.x, 0.0f), std::max(linear.y, 0.0f), std::max(linear.z, 0.0f)));
    c = Vec3(fit(c.x), fit(c.y), fit(c.z));
    c = Mul(kOut, c);
    return Vec3(Saturate(c.x), Saturate(c.y), Saturate(c.z));
}

Vec3 TonemapAgX(const Vec3& linear) {
    // Inset, log2 encoding over [-12.47, 4.03] EV, the base contrast curve,
    // outset, and back to linear (the curve's output is display-encoded).
    static const f32 kInset[3][3] = {{0.842479062253094f, 0.0784335999999992f, 0.0792237451477643f},
                                     {0.0423282422610123f, 0.878468636469772f, 0.0791661274605434f},
                                     {0.0423756549057051f, 0.0784336f, 0.879142973793104f}};
    static const f32 kOutset[3][3] = {{1.19687900512017f, -0.0980208811401368f, -0.0990297440797205f},
                                      {-0.0528968517574562f, 1.15190312990417f, -0.0989611768448433f},
                                      {-0.0529716355144438f, -0.0980434501171241f, 1.15107367264116f}};
    constexpr f32 kMinEv = -12.47393f, kMaxEv = 4.026069f;
    auto curve = [](f32 x) {
        const f32 x2 = x * x, x4 = x2 * x2;
        return 15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4 - 6.868f * x2 * x + 0.4298f * x2 + 0.1191f * x - 0.00232f;
    };
    Vec3 c = Mul(kInset, Vec3(std::max(linear.x, 1e-10f), std::max(linear.y, 1e-10f), std::max(linear.z, 1e-10f)));
    auto encode = [&](f32 v) { return (std::clamp(std::log2(std::max(v, 1e-10f)), kMinEv, kMaxEv) - kMinEv) / (kMaxEv - kMinEv); };
    c = Vec3(curve(encode(c.x)), curve(encode(c.y)), curve(encode(c.z)));
    c = Mul(kOutset, c);
    auto linearize = [](f32 v) { return std::pow(Saturate(v), 2.2f); };
    return Vec3(linearize(c.x), linearize(c.y), linearize(c.z));
}

Vec3 Tonemap(const Vec3& linear, Tonemapper mapper) {
    switch (mapper) {
    case Tonemapper::ACES: return TonemapACES(linear);
    case Tonemapper::AgX: return TonemapAgX(linear);
    case Tonemapper::None: return Vec3(Saturate(linear.x), Saturate(linear.y), Saturate(linear.z));
    }
    return linear;
}

f32 LinearToSrgb(f32 v) {
    v = Saturate(v);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

f32 SrgbToLinear(f32 v) {
    v = Saturate(v);
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

Vec3 AdjustSaturation(const Vec3& rgb, f32 saturation) {
    const f32 l = Luminance(rgb);
    return Vec3(l + (rgb.x - l) * saturation, l + (rgb.y - l) * saturation, l + (rgb.z - l) * saturation);
}

f32 VignetteFactor(f32 u, f32 v, f32 strength) {
    const f32 dx = u - 0.5f, dy = v - 0.5f;
    const f32 d2 = (dx * dx + dy * dy) / 0.5f; // 0 at the center, 1 in the corners
    return 1.0f - std::clamp(strength, 0.0f, 1.0f) * std::clamp(d2, 0.0f, 1.0f);
}

// --- Bloom ------------------------------------------------------------------------------------------

std::vector<Extent> BloomChain(u32 width, u32 height, u32 max_levels, u32 min_size) {
    std::vector<Extent> chain;
    u32 w = width, h = height;
    while (chain.size() < max_levels) {
        w = (w + 1) / 2;
        h = (h + 1) / 2;
        if (w < min_size || h < min_size) break;
        chain.push_back({w, h});
    }
    return chain;
}

// --- TAA ----------------------------------------------------------------------------------------------

f32 Halton(u32 index, u32 base) {
    f32 f = 1.0f, r = 0.0f;
    while (index > 0) {
        f /= static_cast<f32>(base);
        r += f * static_cast<f32>(index % base);
        index /= base;
    }
    return r;
}

Vec3 TaaJitter(u64 frame, u32 count) {
    const u32 i = static_cast<u32>(frame % std::max(count, 1u)) + 1;
    return Vec3(Halton(i, 2) - 0.5f, Halton(i, 3) - 0.5f, 0.0f);
}

Mat4 JitterProjection(const Mat4& projection, f32 offset_x_pixels, f32 offset_y_pixels, u32 width, u32 height) {
    // Adds offset * w to clip x and y, so after the divide every point moves
    // by exactly the offset in NDC, for perspective and orthographic alike.
    const f32 ox = 2.0f * offset_x_pixels / static_cast<f32>(std::max(width, 1u));
    const f32 oy = 2.0f * offset_y_pixels / static_cast<f32>(std::max(height, 1u));
    Mat4 m = projection;
    for (Vec4& col : m.cols) col = Vec4(col.x + ox * col.w, col.y + oy * col.w, col.z, col.w);
    return m;
}

} // namespace aether
