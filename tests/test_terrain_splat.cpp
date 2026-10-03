#include "aether/terrain/splat.h"
#include "aether/terrain/terrain.h"
#include "test_framework.h"

#include <algorithm>
#include <array>

using namespace aether;
using namespace aether::terrain;

AETHER_TEST(SplatmapLayerChannelMapping) {
    AETHER_CHECK(LayerToChannel(0) == 0); // R
    AETHER_CHECK(LayerToChannel(1) == 1); // G
    AETHER_CHECK(LayerToChannel(2) == 2); // B
    AETHER_CHECK(LayerToChannel(3) == 3); // A
    AETHER_CHECK(ChannelToLayer(0) == 0);
    AETHER_CHECK(ChannelToLayer(2) == 2);
}

AETHER_TEST(SplatmapPackUnpackRoundTrip) {
    f32 weights[4] = {0.5f, 0.3f, 0.15f, 0.05f};
    u32 packed = PackWeights(4, {weights, 4});
    f32 out[4] = {0, 0, 0, 0};
    UnpackWeights(packed, 4, out);
    AETHER_CHECK_NEAR(out[0], 0.5f, 0.01f);
    AETHER_CHECK_NEAR(out[1], 0.3f, 0.01f);
    AETHER_CHECK_NEAR(out[2], 0.15f, 0.01f);
    AETHER_CHECK_NEAR(out[3], 0.05f, 0.01f);
}

AETHER_TEST(SplatmapNormalizeWeights) {
    std::array<f32, 4> w = {0.5f, 0.5f, 0.5f, 0.5f}; // sum = 2.0
    NormalizeWeights(w);
    AETHER_CHECK_NEAR(w[0] + w[1] + w[2] + w[3], 1.0f, 1e-5f);
    AETHER_CHECK_NEAR(w[0], 0.25f, 1e-5f);

    std::array<f32, 4> zero = {0.0f, 0.0f, 0.0f, 0.0f};
    NormalizeWeights(zero);
    // Sum 0 -> should stay near 0.
    AETHER_CHECK(zero[0] < 1e-6f && zero[1] < 1e-6f);
}

AETHER_TEST(SplatmapGeneration) {
    std::vector<SplatmapLayer> layers(2);
    layers[0].name = "Grass";
    layers[1].name = "Rock";
    auto sm = BuildSplatmap(64, layers, {});
    AETHER_CHECK(sm.width == 64);
    AETHER_CHECK(sm.height == 64);
    AETHER_CHECK(sm.pixels.size() == 64u * 64u * 4u);

    // Weights should be near 0..1.
    for (u8 p : sm.pixels) {
        AETHER_CHECK(p <= 255);
    }
}

AETHER_TEST(SplatmapSampleWeights) {
    SplatmapData sm;
    sm.width = 4;
    sm.height = 4;
    sm.pixels = std::vector<u8>{
        255, 0,   0, 0,   128, 128, 0, 0,   0, 255, 0, 0,   0, 0, 255, 0,  // row 0
          0, 255, 0, 0,     0,   0, 255, 0,   0, 255, 0, 0,   0, 0, 255, 0,  // row 1
        128, 128, 0, 0,     0,   0,   0, 255, 255, 0, 0, 0,   0, 255, 0, 0,  // row 2
          0,   0, 255, 0, 255,   0,   0,   0,   0, 255, 0, 0, 255, 0, 0, 0   // row 3
    };

    // Corner samples.
    auto w00 = sm.GetWeights(0.0f, 0.0f);
    AETHER_CHECK_NEAR(w00[0], 1.0f, 0.05f);

    // UV (1, 1) is the far corner: pixel (3, 3).
    auto w11 = sm.GetWeights(1.0f, 1.0f);
    AETHER_CHECK_NEAR(w11[0], 1.0f, 0.05f);
    // Pixel (1, 1) is at UV (1/3, 1/3).
    auto w_px11 = sm.GetWeights(1.0f / 3.0f, 1.0f / 3.0f);
    AETHER_CHECK_NEAR(w_px11[2], 1.0f, 0.05f);

    // Bilinear sample in center.
    auto wct = sm.GetWeights(0.5f, 0.5f);
    AETHER_CHECK(wct[0] > 0.0f || wct[1] > 0.0f || wct[2] > 0.0f || wct[3] > 0.0f);
}

AETHER_TEST(BrushWeightAtRadius) {
    // At center, weight should equal strength.
    AETHER_CHECK_NEAR(BrushWeight(0.0f, 0.0f, 2.0f, 0.5f, 0.5f), 0.5f, 1e-4f);
    // Beyond radius, weight should be zero.
    AETHER_CHECK_NEAR(BrushWeight(3.0f, 0.0f, 2.0f, 0.5f, 0.5f), 0.0f, 1e-6f);
    // Halfway out, a soft brush (falloff 1) is lower than its center; a hard one (falloff 0) isn't.
    AETHER_CHECK(BrushWeight(1.0f, 0.0f, 2.0f, 1.0f, 1.0f) < 1.0f);
    AETHER_CHECK_NEAR(BrushWeight(1.0f, 0.0f, 2.0f, 1.0f, 0.0f), 1.0f, 1e-6f);
    // A negative strength (lowering) keeps its sign.
    AETHER_CHECK_NEAR(BrushWeight(0.0f, 0.0f, 2.0f, -0.5f, 0.5f), -0.5f, 1e-6f);
}

AETHER_TEST(TerrainBrushStroke) {
    SplatmapData sm;
    sm.width = 64;
    sm.height = 64;
    sm.pixels.resize(64 * 64 * 4, 0);
    // Initialize to layer 0 = 1.0 everywhere.
    for (usize i = 0; i < sm.pixels.size(); i += 4) {
        sm.pixels[i + 0] = 255;
        sm.pixels[i + 1] = 0;
        sm.pixels[i + 2] = 0;
        sm.pixels[i + 3] = 0;
    }

    TerrainBrush brush;
    brush.radius = 5.0f;
    brush.strength = 1.0f;
    brush.falloff = 0.0f;
    brush.layer_index = 1;

    // Paint at center.
    ApplyBrushStroke(sm, 0.5f, 0.5f, brush);

    // Center should now have layer 1 active.
    auto w = sm.GetWeights(0.5f, 0.5f);
    AETHER_CHECK(w[1] > 0.5f); // layer 1 is dominant
}

AETHER_TEST(TerrainHeightBrushFlatten) {
    Heightmap hm;
    hm.width = 33;
    hm.height = 33;
    hm.cell_size = 1.0f;
    hm.heights.resize(33 * 33, 0.0f);

    // Build a hill in the center.
    const usize center = 16 * 33 + 16;
    hm.heights[center] = 5.0f;
    // Simple gradient.
    for (u32 z = 0; z < 33; ++z) {
        for (u32 x = 0; x < 33; ++x) {
            const usize idx = z * 33 + x;
            const f32 dist = std::sqrt(
                static_cast<f32>((x - 16) * (x - 16) + (z - 16) * (z - 16)));
            hm.heights[idx] = std::max(0.0f, 5.0f - dist * 0.5f);
        }
    }

    TerrainBrush brush;
    brush.radius = 4.0f;
    brush.strength = 1.0f;
    brush.falloff = 0.0f;
    brush.flatten = true;

    ApplyHeightBrush(hm, 16.0f, 16.0f, brush, 1.0f);

    // Flattening pulls the slope toward the height under the center (5): the peak stays, the slope rises.
    const usize c2 = 16 * 33 + 16;
    AETHER_CHECK_NEAR(hm.heights[c2], 5.0f, 1e-4f);
    const usize side = 16 * 33 + 18; // 2 cells out: 4.0 before
    AETHER_CHECK(hm.heights[side] > 4.0f && hm.heights[side] <= 5.0f);
}

AETHER_TEST(TerrainHeightBrushRaiseLowerSmooth) {
    Heightmap hm;
    hm.width = 17, hm.height = 17, hm.cell_size = 1.0f;
    hm.heights.assign(17u * 17u, 0.0f);
    TerrainBrush brush;
    brush.radius = 4.0f;
    brush.strength = 1.0f;
    brush.falloff = 0.5f;
    // Flat ground at height 0 can be raised...
    ApplyHeightBrush(hm, 8.0f, 8.0f, brush, 2.0f);
    const usize c = 8 * 17 + 8;
    AETHER_CHECK_NEAR(hm.heights[c], 2.0f, 1e-4f);  // full strength x vertical_scale
    AETHER_CHECK(hm.heights[8 * 17 + 11] > 0.0f && hm.heights[8 * 17 + 11] < 2.0f); // the fading rim
    AETHER_CHECK_NEAR(hm.heights[0], 0.0f, 1e-6f);  // outside the brush
    // ...and lowered.
    brush.strength = -1.0f;
    ApplyHeightBrush(hm, 8.0f, 8.0f, brush, 2.0f);
    AETHER_CHECK_NEAR(hm.heights[c], 0.0f, 1e-4f);
    // Smoothing a spike spreads it evenly (it reads the heights from before the stroke).
    hm.heights.assign(17u * 17u, 0.0f);
    hm.heights[c] = 9.0f;
    TerrainBrush smooth;
    smooth.radius = 3.0f;
    smooth.strength = 1.0f;
    smooth.falloff = 0.0f;
    smooth.smooth = true;
    ApplyHeightBrush(hm, 8.0f, 8.0f, smooth, 1.0f);
    AETHER_CHECK_NEAR(hm.heights[c], 1.0f, 1e-4f);        // the 3x3 average
    AETHER_CHECK_NEAR(hm.heights[c - 1], 1.0f, 1e-4f);    // left and right neighbours got the same share
    AETHER_CHECK_NEAR(hm.heights[c + 1], 1.0f, 1e-4f);
}

AETHER_TEST(SplatmapFromWeights) {
    std::vector<SplatmapLayer> layers(3);
    // None: the first layer.
    SplatmapData base = BuildSplatmap(4, layers, {});
    AETHER_CHECK(base.pixels[0] == 255 && base.pixels[1] == 0 && base.pixels[2] == 0);
    // One per layer: that mix everywhere, normalized.
    SplatmapData mix = BuildSplatmap(4, layers, {1.0f, 1.0f, 2.0f});
    AETHER_CHECK(mix.pixels[0] == 64 && mix.pixels[1] == 64 && mix.pixels[2] == 128 && mix.pixels[3] == 0);
    AETHER_CHECK(mix.pixels[15 * 4 + 2] == 128);
    // Per pixel: pixel 1 is all rock, the rest grass.
    std::vector<f32> painted(4u * 4u * 3u, 0.0f);
    for (usize p = 0; p < 16; ++p) painted[p * 3] = 1.0f;
    painted[1 * 3] = 0.0f, painted[1 * 3 + 1] = 1.0f;
    SplatmapData pm = BuildSplatmap(4, layers, painted);
    AETHER_CHECK(pm.pixels[0] == 255 && pm.pixels[4] == 0 && pm.pixels[5] == 255);
}
