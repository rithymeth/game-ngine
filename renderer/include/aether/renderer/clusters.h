#pragma once

#include "aether/renderer/view.h"

#include <vector>

namespace aether {

// Clustered lighting (Phase 14 step 2): the view volume is cut into a grid of
// froxels (16 x 9 tiles on screen, 24 depth slices, exponentially spaced so
// near slices are thin) and each lists the lights that can reach it. The
// forward pass then shades a pixel with just its cluster's lights. This is
// the CPU reference; the GPU compute pass is checked against it.
struct ClusterGrid {
    u32 x = 16, y = 9, z = 24;
    u32 Count() const { return x * y * z; }
    u32 Index(u32 ix, u32 iy, u32 iz) const { return (iz * y + iy) * x + ix; }
};

struct ClusterLightRange {
    u32 offset = 0;      // into LightClusters::indices
    u16 point_count = 0; // point light indices first,
    u16 spot_count = 0;  // then spot light indices
};

struct LightClusters {
    ClusterGrid grid;
    std::vector<ClusterLightRange> clusters;
    std::vector<u32> indices;   // into RenderScene::point_lights / spot_lights
    u32 max_per_cluster = 256;  // lights past this are dropped (and counted)
    u64 dropped = 0;

    // The slice a view-space depth (distance in front, > 0) falls in.
    u32 SliceOf(f32 depth) const;
    // The cluster holding a world-space point, or false if it's outside the view.
    bool ClusterOf(const View& view, const Vec3& world_point, u32& index) const;
    // Slice boundaries (view-space depths), z + 1 of them.
    std::vector<f32> slice_depths;
};

LightClusters BuildLightClusters(const RenderScene& scene, const View& view, ClusterGrid grid = {}, u32 max_per_cluster = 256);

// The sphere around a spot light's cone (for culling).
void SpotBoundingSphere(const RenderSpotLight& spot, Vec3& center, f32& radius);

} // namespace aether
