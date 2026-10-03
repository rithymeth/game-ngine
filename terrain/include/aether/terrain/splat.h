#pragma once

#include "aether/core/base.h"
#include "aether/math/math.h"
#include "aether/terrain/terrain.h"

#include <array>
#include <span>
#include <string>
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

    // Get the blending weights at normalized UV (0..1).
    // Returns up to 4 weights (one per layer).
    // Sum may be < 1.0 if no layer is painted.
    std::array<f32, 4> GetWeights(f32 u, f32 v) const;
};

// Build a splatmap for a given terrain chunk.
// layers must have at least 1 entry. Uses the chunk's UV space.
SplatmapData BuildSplatmap(u32 resolution, span<const SplatmapLayer> layers,
                           const std::vector<f32>& layer_weights);

// The height-sculpt mode for a terrain brush.
enum class SculptMode {
    Raise,    // raise terrain (strength > 0) or lower (strength < 0)
    Flatten,  // lerp toward the brush center's height
    Smooth,   // average with neighbors
    Noise,    // add Perlin-like noise displacement (strength scales amplitude)
    Erode     // simplify the surface (remove small high-frequency bumps)
};

// A brush for terrain painting.
struct TerrainBrush {
    f32 radius = 2.0f;       // world units
    f32 strength = 0.5f;     // 0..1: how fast to apply paint per stroke
    f32 falloff = 0.5f;      // 0..1: how sharply the weight drops toward the rim (0 = smoothstep)
    i32 layer_index = 0;     // which layer to paint (splatmap)
    SculptMode mode = SculptMode::Raise;
    bool flatten = false;   // back-compat: sets mode to Flatten
    bool smooth = false;     // back-compat: sets mode to Smooth

    // Noise sculpt parameters.
    f32 noise_scale = 8.0f;  // feature size in world units (smaller = finer detail)
    i32 noise_seed = 1337;   // deterministic seed for the noise displacement

    // Erosion parameters.
    f32 erosion_deposition = 0.35f;  // how much material is re-applied downstream
    i32 erosion_iterations = 1;      // passes over the brush footprint
};

// Compute a circular brush's weight at an offset from the center.
// weight = strength at the center, fading to 0 at the radius.
f32 BrushWeight(f32 dx, f32 dz, f32 radius, f32 strength, f32 falloff);

// Apply a paint stroke to the splatmap layer weights.
// Adjusts weights so only the target layer increases (others decrease proportionally).
void ApplyBrushStroke(SplatmapData& splatmap,
                       f32 center_u, f32 center_v,
                       const TerrainBrush& brush);

// Apply a paint stroke to the heightmap (flatten or smooth).
void ApplyHeightBrush(Heightmap& heightmap,
                       f32 center_wx, f32 center_wz,
                       const TerrainBrush& brush,
                       f32 vertical_scale);

// Displace the heightmap inside the brush footprint with Perlin-like noise.
// Amplitude is scaled by brush.strength; feature size by brush.noise_scale.
void ApplyNoiseBrush(Heightmap& heightmap,
                      f32 center_wx, f32 center_wz,
                      const TerrainBrush& brush,
                      f32 vertical_scale);

// A simple particle-free erosion pass over the brush footprint: for each
// sample, if a neighbor is much lower, deposit some of this sample's excess
// onto the neighbor (flattening sharp ridges). erosion_deposition controls
// how aggressive the smoothing is, erosion_iterations how many passes.
void ApplyErosionBrush(Heightmap& heightmap,
                        f32 center_wx, f32 center_wz,
                        const TerrainBrush& brush,
                        f32 vertical_scale);

// Dispatch the correct height-brush mode based on brush.mode.
// Raises/lowers, flattens, smooths, applies noise, or erodes.
void ApplySculptBrush(Heightmap& heightmap,
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
