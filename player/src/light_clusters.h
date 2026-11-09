#pragma once

#include "aether/math/math.h"

#include <span>
#include <vector>

// Clustered forward lighting: the view frustum cut into a grid of cells (kClusterTilesX x kClusterTilesY screen tiles, kClusterSlices
// depth slices spaced exponentially from the near plane to the far plane), and for each cell the point and spot lights that can reach
// it. Built on the CPU each frame from every light in view; the scene shader finds its pixel's cell and shades only those lights, so a
// scene may hold hundreds of lights where the frame constants held the nearest 8 point and 4 spot lights.
//
// The buffer the shader reads (`g_FrameLights`, a ByteAddressBuffer: PipelineDesc::frame_read_buffer), in bytes:
//   [0, lights * 48)            the lights, ClusterLight each
//   [cells_offset, + cells * 8) per cell, uint2: the first index into the index list, and how many follow
//   [indices_offset, + n * 2)   the index list: a light's position in the light array (16 bits), per cell
// FrameData's cluster block (frame_lights.h) holds the grid's shape, the offsets and how to find a pixel's cell.

namespace aether::player {

constexpr u32 kClusterTilesX = 16;
constexpr u32 kClusterTilesY = 9;
constexpr u32 kClusterSlices = 24;
constexpr u32 kClusterCells = kClusterTilesX * kClusterTilesY * kClusterSlices;
constexpr u32 kMaxClusterLights = 1024;     // the nearest stay when a frame has more in view
constexpr u32 kMaxClusterIndices = 1u << 18; // cells x lights, summed; lights that would overflow it are left out, farthest first

// One point or spot light, as the shader reads it. A point light is a spot whose cone takes in every direction (cosines below -1).
struct ClusterLight {
    f32 position_range[4]{};  // xyz world position, w range (the light fades to nothing there)
    f32 color_inner[4]{};     // rgb colour times intensity; w: cosine of the inner cone angle
    f32 direction_outer[4]{}; // xyz: the way it points; w: cosine of the outer cone angle
};
static_assert(sizeof(ClusterLight) == 48);

ClusterLight MakePointClusterLight(const Vec3& position, f32 range, const Vec3& color);
ClusterLight MakeSpotClusterLight(const Vec3& position, f32 range, const Vec3& color, const Vec3& direction, f32 outer_degrees,
                                  f32 inner_degrees);

// What the camera sees: world to view (right-handed, looking down -Z, as CameraView makes it), view to clip (CameraProjection), and the
// planes the depth slices span.
struct ClusterView {
    Mat4 view;
    Mat4 projection;
    f32 near_plane = 0.1f;
    f32 far_plane = 1000.0f;
};

struct LightClusters {
    std::vector<u8> bytes;  // the shader's buffer, laid out as above
    u32 light_count = 0;    // lights binned (in view, kept)
    u32 index_count = 0;
    u32 cells_offset = 0;   // bytes
    u32 indices_offset = 0; // bytes
    u32 dropped_lights = 0; // in view, but over kMaxClusterLights or the index budget
    f32 depth_scale = 0.0f; // a view depth d is in slice floor(log(d) * depth_scale + depth_bias)
    f32 depth_bias = 0.0f;
};

// The size of buffer BuildLightClusters can fill: allocate the frame read buffer this large.
constexpr u64 kLightClusterBufferBytes =
    u64(kMaxClusterLights) * sizeof(ClusterLight) + u64(kClusterCells) * 8 + u64(kMaxClusterIndices) * 2;
static_assert(kMaxClusterLights <= 65536, "light indices are 16 bits");

// The cell a view depth falls in along the slices (clamped to the grid), as the shader computes it.
u32 ClusterSlice(f32 view_depth, f32 depth_scale, f32 depth_bias);

// Bins `lights` into the grid for `view`. Lights wholly outside the frustum are left out; the rest are kept nearest first (by the
// distance from the camera to the light's reach) while they fit the limits above. Each light lands in every cell its bounding sphere's
// screen rectangle and depth range touch (a spot's sphere bounds its cone), which can include cells it does not light, never the reverse.
void BuildLightClusters(std::span<const ClusterLight> lights, const ClusterView& view, LightClusters& out);

} // namespace aether::player
