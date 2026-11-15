#include "aether/renderer/post.h"
#include "aether/scene/gameplay.h"
#include "test_framework.h"

#include <cmath>

using namespace aether;

// Phase 14 step 4: post-processing math (exposure, tone mapping, grading,
// bloom chain, TAA jitter).

AETHER_TEST(Post_AutoExposureMeasuresAndAdapts) {
    ExposureSettings s;
    // A uniform scene: the average is that luminance, and the resulting
    // exposure maps it to 1 / 9.6 (EV100's K = 12.5 and the 1.2 lens factor).
    for (const f32 l : {0.05f, 0.18f, 1.0f, 40.0f}) {
        const std::vector<f32> image(1000, l);
        const f32 avg = AverageLuminance(LuminanceHistogram(image, s), s);
        AETHER_CHECK(std::fabs(std::log2(avg) - std::log2(l)) < 0.1f); // within the bin width
        const f32 exposure = ExposureFromEV100(EV100FromLuminance(l));
        AETHER_CHECK(std::fabs(exposure * l - 1.0f / 9.6f) < 1e-4f);
    }
    // One EV of compensation doubles it.
    AETHER_CHECK(std::fabs(ExposureFromEV100(5.0f, 1.0f) / ExposureFromEV100(5.0f) - 2.0f) < 1e-5f);

    // The brightest 5% (the sun) and black pixels don't pull the average.
    std::vector<f32> scene(1000, 0.5f);
    for (int i = 0; i < 40; ++i) scene[i] = 5000.0f;
    for (int i = 40; i < 200; ++i) scene[i] = 0.0f;
    const f32 avg = AverageLuminance(LuminanceHistogram(scene, s), s);
    AETHER_CHECK(std::fabs(std::log2(avg) - std::log2(0.5f)) < 0.1f);
    // All black: the bottom of the range, not a NaN.
    AETHER_CHECK(AverageLuminance(LuminanceHistogram(std::vector<f32>(10, 0.0f), s), s) == std::exp2(s.min_log2));
    AETHER_CHECK(Luminance(Vec3(1, 1, 1)) > 0.999f && Luminance(Vec3(1, 1, 1)) < 1.001f);

    // Adaptation: brightening (to a higher EV) is faster than darkening, and both converge.
    const f32 up = AdaptEV100(0.0f, 10.0f, 0.5f, s), down = AdaptEV100(10.0f, 0.0f, 0.5f, s);
    AETHER_CHECK(up > 10.0f - down); // moved further in the same time
    f32 ev = 0.0f;
    for (int i = 0; i < 600; ++i) ev = AdaptEV100(ev, 8.0f, 1.0f / 60.0f, s);
    AETHER_CHECK(std::fabs(ev - 8.0f) < 1e-3f);
    AETHER_CHECK(AdaptEV100(0.0f, 100.0f, 1000.0f, s) == s.max_ev100); // clamped
}

AETHER_TEST(Post_TonemappersAreWellBehaved) {
    for (const Tonemapper mapper : {Tonemapper::ACES, Tonemapper::AgX}) {
        const Vec3 black = Tonemap(Vec3(0, 0, 0), mapper);
        AETHER_CHECK(black.x < 0.02f && black.y < 0.02f && black.z < 0.02f);
        const Vec3 huge = Tonemap(Vec3(1e6f, 1e6f, 1e6f), mapper);
        AETHER_CHECK(huge.x <= 1.0f && huge.x > 0.8f);
        const Vec3 grey = Tonemap(Vec3(0.18f, 0.18f, 0.18f), mapper);
        AETHER_CHECK(grey.x > 0.05f && grey.x < 0.4f);
        AETHER_CHECK(std::fabs(grey.x - grey.y) < 0.02f && std::fabs(grey.y - grey.z) < 0.02f); // grey stays neutral
        // Monotonic: brighter in, brighter (or equal) out.
        f32 previous = -1.0f;
        for (f32 v = 0.001f; v < 200.0f; v *= 1.25f) {
            const f32 out = Tonemap(Vec3(v, v, v), mapper).y;
            AETHER_CHECK(out >= previous - 1e-5f && !std::isnan(out));
            previous = out;
        }
        // Negative input (from filtering) doesn't make NaNs.
        const Vec3 negative = Tonemap(Vec3(-1, 0.5f, 2), mapper);
        AETHER_CHECK(!std::isnan(negative.x) && negative.x >= 0.0f);
    }
    // ACES compresses highlights less abruptly than a clamp: 2.0 and 4.0 differ.
    AETHER_CHECK(TonemapACES(Vec3(4, 4, 4)).x > TonemapACES(Vec3(2, 2, 2)).x);
    AETHER_CHECK(Tonemap(Vec3(2, 0.5f, -1), Tonemapper::None).x == 1.0f);
    // AgX desaturates very bright saturated colors toward white (its point):
    // a bright red's green and blue rise.
    const Vec3 red = TonemapAgX(Vec3(50, 0, 0));
    AETHER_CHECK(red.x > 0.8f && red.y > 0.05f);

    // sRGB encoding.
    AETHER_CHECK(LinearToSrgb(0.0f) == 0.0f && std::fabs(LinearToSrgb(1.0f) - 1.0f) < 1e-5f);
    AETHER_CHECK(std::fabs(LinearToSrgb(0.5f) - 0.7354f) < 1e-3f && std::fabs(LinearToSrgb(0.001f) - 0.01292f) < 1e-5f);
    for (f32 v = 0.0f; v <= 1.0f; v += 0.05f) AETHER_CHECK(std::fabs(SrgbToLinear(LinearToSrgb(v)) - v) < 1e-4f);
}

AETHER_TEST(Post_GradingBloomAndJitter) {
    // Saturation keeps luminance.
    const Vec3 c(0.8f, 0.3f, 0.1f);
    const Vec3 grey = AdjustSaturation(c, 0.0f);
    AETHER_CHECK(std::fabs(grey.x - grey.y) < 1e-6f && std::fabs(Luminance(grey) - Luminance(c)) < 1e-5f);
    AETHER_CHECK(std::fabs(Luminance(AdjustSaturation(c, 1.7f)) - Luminance(c)) < 1e-5f);
    // Vignette: untouched at the center, darkest in the corners.
    AETHER_CHECK(VignetteFactor(0.5f, 0.5f, 0.8f) == 1.0f && std::fabs(VignetteFactor(0, 0, 0.8f) - 0.2f) < 1e-5f);
    AETHER_CHECK(VignetteFactor(1, 0.5f, 0.8f) > VignetteFactor(1, 1, 0.8f));

    // Bloom chain from 1080p: halves, rounded up, stopping at 8 px or 6 levels.
    const std::vector<Extent> chain = BloomChain(1920, 1080);
    AETHER_CHECK(chain.size() == 6 && chain[0] == (Extent{960, 540}) && chain[3] == (Extent{120, 68}) && chain[5] == (Extent{30, 17}));
    AETHER_CHECK(BloomChain(40, 20).size() == 1 && BloomChain(4, 4).empty());

    // Halton sequences and the jitter pattern.
    AETHER_CHECK(Halton(1, 2) == 0.5f && Halton(2, 2) == 0.25f && Halton(3, 2) == 0.75f);
    AETHER_CHECK(std::fabs(Halton(1, 3) - 1.0f / 3.0f) < 1e-6f && std::fabs(Halton(2, 3) - 2.0f / 3.0f) < 1e-6f);
    f32 sx = 0.0f, sy = 0.0f;
    for (u64 frame = 0; frame < 8; ++frame) {
        const Vec3 j = TaaJitter(frame);
        AETHER_CHECK(std::fabs(j.x) <= 0.5f && std::fabs(j.y) <= 0.5f);
        sx += j.x;
        sy += j.y;
    }
    AETHER_CHECK(std::fabs(sx / 8) < 0.07f && std::fabs(sy / 8) < 0.07f); // centered on average
    AETHER_CHECK(TaaJitter(3).x == TaaJitter(11).x); // repeats every 8 frames

    // The jittered projection moves every point by exactly the offset in
    // pixels, at any depth, for perspective and orthographic projections.
    for (const bool perspective : {true, false}) {
        const Mat4 p = perspective ? Mat4::PerspectiveRH(1.0f, 16.0f / 9.0f, 0.1f, 100.0f) : OrthographicRH(20, 11.25f, 0.1f, 100.0f);
        const Mat4 jittered = JitterProjection(p, 0.25f, -0.4f, 1920, 1080);
        for (const f32 z : {-0.5f, -5.0f, -50.0f}) {
            const Vec4 a = p * Vec4(1, 2, z, 1), b = jittered * Vec4(1, 2, z, 1);
            const f32 dx = (b.x / b.w - a.x / a.w) * 0.5f * 1920.0f, dy = (b.y / b.w - a.y / a.w) * 0.5f * 1080.0f;
            AETHER_CHECK(std::fabs(dx - 0.25f) < 1e-3f && std::fabs(dy + 0.4f) < 1e-3f);
            AETHER_CHECK(std::fabs(b.z / b.w - a.z / a.w) < 1e-6f); // depth untouched
        }
    }
}

// The post pass's lens effects, as CPU references: the film grain, the luminance sharpen and the chromatic offset (the GPU pass is checked against
// them in test_post_pass.cpp).
AETHER_TEST(Post_LensEffectReferencesAreWellBehaved) {
    // The grain's hash is deterministic, covers [-0.5, 0.5) with a mean near 0, and depends on the column, the row and the frame.
    AETHER_CHECK(GrainHash(12345u) == GrainHash(12345u) && GrainHash(0u) != GrainHash(1u));
    f64 sum = 0.0;
    f32 lo = 1.0f, hi = -1.0f;
    for (u32 y = 0; y < 64; ++y) {
        for (u32 x = 0; x < 64; ++x) {
            const f32 n = FilmGrainNoise(x, y, 7);
            lo = std::min(lo, n);
            hi = std::max(hi, n);
            sum += n;
        }
    }
    AETHER_CHECK(lo >= -0.5f && hi < 0.5f && lo < -0.45f && hi > 0.45f && std::fabs(sum / 4096.0) < 0.02);
    AETHER_CHECK(FilmGrainNoise(3, 4, 5) != FilmGrainNoise(4, 3, 5) && FilmGrainNoise(3, 4, 5) != FilmGrainNoise(3, 4, 6) &&
                 FilmGrainNoise(3, 4, 5) != FilmGrainNoise(3, 5, 5));

    // Grain is for the midtones: nothing at black or white, strongest at middle grey; strength 0 changes nothing; the result stays in 0..1.
    AETHER_CHECK(FilmGrainResponse(0.0f) == 0.0f && FilmGrainResponse(1.0f) == 0.0f && FilmGrainResponse(0.5f) == 1.0f && FilmGrainResponse(5.0f) == 0.0f);
    AETHER_CHECK(ApplyFilmGrain(0.4f, 0.0f, 0.5f) == 0.4f && ApplyFilmGrain(0.0f, 1.0f, 0.5f) == 0.0f && ApplyFilmGrain(1.0f, 1.0f, -0.5f) == 1.0f);
    AETHER_CHECK(ApplyFilmGrain(0.5f, 1.0f, 0.5f) > 0.5f && ApplyFilmGrain(0.5f, 1.0f, -0.5f) < 0.5f);
    for (const f32 e : {0.05f, 0.5f, 0.95f}) {
        for (const f32 n : {-0.5f, 0.0f, 0.4999f}) {
            const f32 g = ApplyFilmGrain(e, 1.0f, n);
            AETHER_CHECK(g >= 0.0f && g <= 1.0f);
        }
    }

    // Sharpen: strength 0 and a flat area leave the luminance alone; a pixel brighter than its surroundings gets brighter, a darker one darker;
    // and no input (a very bright one, a negative one) makes a NaN or a runaway.
    AETHER_CHECK(std::fabs(SharpenLuminance(0.8f, 0.2f, 0.0f) - 0.8f) < 1e-5f && std::fabs(SharpenLuminance(0.5f, 0.5f, 1.0f) - 0.5f) < 1e-5f);
    AETHER_CHECK(SharpenLuminance(0.8f, 0.2f, 0.5f) > 0.8f && SharpenLuminance(0.2f, 0.8f, 0.5f) < 0.2f);
    AETHER_CHECK(SharpenLuminance(0.8f, 0.2f, 1.0f) > SharpenLuminance(0.8f, 0.2f, 0.5f));
    for (const f32 l : {-3.0f, 0.0f, 1e-6f, 4.0f, 1e6f}) {
        for (const f32 n : {0.0f, 0.5f, 1e6f}) {
            const f32 s = SharpenLuminance(l, n, 1.0f);
            AETHER_CHECK(std::isfinite(s) && s >= 0.0f);
        }
    }
    AETHER_CHECK(SharpenLuminance(1e6f, 0.0f, 1.0f) < 1e6f * 4.0f); // the highlight is not blown up further

    // Chromatic aberration: nothing at the centre or at strength 0, opposite on opposite sides, growing with strength, 2.5% of the frame at the most.
    const Vec3 centre = ChromaticOffset(0.5f, 0.5f, 1.0f);
    AETHER_CHECK(centre.x == 0.0f && centre.y == 0.0f && ChromaticOffset(0.9f, 0.1f, 0.0f).x == 0.0f);
    const Vec3 left = ChromaticOffset(0.0f, 0.5f, 1.0f), right = ChromaticOffset(1.0f, 0.5f, 1.0f);
    AETHER_CHECK(std::fabs(left.x + right.x) < 1e-6f && std::fabs(right.x - 0.025f) < 1e-6f && left.y == 0.0f);
    AETHER_CHECK(std::fabs(ChromaticOffset(1.0f, 0.5f, 0.5f).x - 0.0125f) < 1e-6f && ChromaticOffset(1.0f, 0.5f, 5.0f).x == right.x);
}
