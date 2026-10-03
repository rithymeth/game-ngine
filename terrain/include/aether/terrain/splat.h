#pragma once

#include "aether/core/base.h"
#include "aether/math/math.h"
#include "aether/terrain/terrain.h"

#include <span>
#include <vector>

namespace aether {
namespace terrain {

using std::span;

// A splatmap: a 2D texture where each pixel encodes which terrain layers
// blend at that position. Alpha blending between up to 4 layers per pixel.
// Stored as RGBA8 for GPU upload (each channel = weight for one layer).

struct SplatmapLayer {
    std::string name;         // display name
    u64 albedo_texture_id;   // asset ID of the albedo texture
    u64 normal_texture_id;     // asset ID of the normal map
    Vec3 albedo_tint{1.0f, 1.0f, 1.0f};
    f32 metallic = 0.0f;
    f32 roughness = 0.85f;
    f32 normal_strength = 1.0f;
    f32 tile_scale = 10.0f;     // UV tiling for this layer
    bool visible = true;
};

// Splatmap layer weights for one terrain chunk.
// The alpha channel is unused here (last layer fills to 1).
// Format per pixel: R=layer0, G=layer1, B=layer2, A=layer3 (if 4+ layers).
struct SplatmapData {
    u32 width = 0;   // must match chunk splatmap resolution
    u32 height = 0;
    std::vector<u8> pixels; // RGBA8, row-major

    // Get the blending weights at normalized UV (0..1), bilinear between pixels.
    // Returns up to 4 weights (one per layer).
    // Sum may be < 1.0 if no layer is painted.
    std::array<f32, 4> GetWeights(f32 u, f32 v) const;
};

// Build a splatmap (up to 4 layers) at `resolution` x `resolution`:
// - `layer_weights` empty: the first layer everywhere;
// - one weight per layer: that mix everywhere;
// - one weight per layer per pixel (row-major, a pixel's layers together): painted.
// Weights are normalized to sum to 1 per pixel.
SplatmapData BuildSplatmap(u32 resolution, span<const SplatmapLayer> layers,
                           const std::vector<f32>& layer_weights);

// A brush for terrain painting.
struct TerrainBrush {
    f32 radius = 2.0f;       // world units
    f32 strength = 0.5f;     // how fast to apply per stroke (negative lowers, for raise/lower)
    f32 falloff = 0.5f;      // 0..1: the share of the radius that fades out (0: a hard edge)
    i32 layer_index = 0;     // which layer to paint
    bool flatten = false;   // flatten terrain to the brush center's height
    bool smooth = false;     // smooth terrain within the brush
};

// A circular brush's weight at an offset from its center: `strength` within
// the core, fading smoothly to 0 over the outer `falloff` share of the radius.
f32 BrushWeight(f32 dx, f32 dz, f32 radius, f32 strength, f32 falloff);

// Apply a paint stroke to the splatmap layer weights.
// Adjusts weights so only the target layer increases (others decrease proportionally).
void ApplyBrushStroke(SplatmapData& splatmap,
                       f32 center_u, f32 center_v,
                       const TerrainBrush& brush);

// Apply a stroke to the heightmap: smooth, flatten (toward the height under the
// center), or else raise/lower by up to `vertical_scale` world units at full weight.
void ApplyHeightBrush(Heightmap& heightmap,
                       f32 center_wx, f32 center_wz,
                       const TerrainBrush& brush,
                       f32 vertical_scale);

// Convert from layer indices to RGBA channel indices (0=R, 1=G, 2=B, 3=A).
i32 LayerToChannel(u32 layer_index);

// Convert from RGBA channel to layer index (inverse of LayerToChannel).
u32 ChannelToLayer(i32 channel);

// Merge layer weights so they sum to 1.0 (or less if no layer at a pixel).
void NormalizeWeights(std::array<f32, 4>& weights);

// Encode layer weights into an RGBA8 pixel.
u32 PackWeights(u32 layer_count, span<const f32> weights);

// Decode an RGBA8 pixel into up to 4 layer weights.
void UnpackWeights(u32 pixel, u32 layer_count, f32* out_weights);

} // namespace terrain
} // namespace aether
