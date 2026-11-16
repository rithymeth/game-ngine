#pragma once

// @stability: experimental
//
// A cluster hierarchy for virtual geometry (the first stage of a Nanite-style renderer), built offline on an indexed triangle mesh, CPU-only, on
// meshoptimizer (hidden behind the .cpp) and the meshlet builder in meshlet_cook.h.
//
// The mesh is cut into clusters of at most 124 triangles (level 0, the leaves: the full-detail mesh, exactly). Each level then groups neighbouring
// clusters (about four), merges their triangles, simplifies the merge to about half with its border locked (so a group still meets its neighbours with no
// crack), and cuts the result into new clusters: the next level, about half the triangles. This goes on until one cluster is left, or until nothing can be
// simplified further. The result is a DAG (a cluster's parent is the group that merged it, and a group has several outputs), not a tree.
//
// Every cluster carries how wrong it is (`error`, object-space distance, 0 for a leaf) and how wrong its parent group would be (`parent_error`), each with a
// bounding sphere shared by every cluster of the group that made it. Errors only grow towards the root and spheres only grow with them. That is what makes
// the cut cheap and crack-free: a cluster is drawn when its own error looks small enough on screen but its parent's does not, which depends on that cluster
// alone, and the clusters picked this way always cover the mesh exactly once. SelectClusters is that test, on the CPU, as the reference for a GPU pass.
//
// The results are in memory only: no cooked format changes.

#include "aether/cook/meshlet_cook.h"
#include "aether/core/base.h"
#include "aether/math/math.h"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace aether::cook {

struct ClusterParams {
    MeshletParams meshlets;     // the cluster size (leaves and every level)
    u32 group_size = 4;         // clusters merged per group, 2 to 8
    f32 reduction = 0.5f;       // each group is simplified to about this fraction of its triangles, 0.1 to 0.9
    f32 min_progress = 0.85f;   // a group that cannot get below this fraction of its triangles is left as it is (its clusters wait for another grouping)
    u32 max_levels = 32;
};

struct Cluster {
    u32 index_offset = 0;    // into ClusterHierarchy::indices
    u32 triangle_count = 0;
    u32 level = 0;           // 0 for a leaf
    Vec3 center;             // this cluster's LOD bounding sphere, the same for every cluster its source group made (its own centre for a leaf)
    f32 radius = 0.0f;
    f32 error = 0.0f;        // object-space error against the full-detail mesh (0 for a leaf), the same for every cluster of the source group
    Vec3 parent_center;      // the same for the group that merges this cluster
    f32 parent_radius = 0.0f;
    f32 parent_error = 0.0f; // +FLT_MAX for a root: nothing coarser stands in for it
    MeshletBounds bounds;    // a tight sphere and normal cone around the triangles themselves, for frustum and back-face culling
    i32 group = -1;          // the group that merged this cluster, -1 for a root
    i32 source_group = -1;   // the group that made it, -1 for a leaf
};

struct ClusterGroup {
    std::vector<u32> inputs;  // clusters merged (one level finer)
    std::vector<u32> outputs; // clusters made from them
    Vec3 center;
    f32 radius = 0.0f;
    f32 error = 0.0f;
};

struct ClusterHierarchy {
    std::vector<u32> indices;          // every cluster's triangles, 3 vertex indices each, into the source vertex array
    std::vector<Cluster> clusters;     // leaves first (level 0), then each level in turn
    std::vector<ClusterGroup> groups;
    u32 level_count = 0;
    u32 leaf_count = 0;                // clusters[0 .. leaf_count) are the leaves
    f32 mesh_extent = 0.0f;            // the source mesh's size, for scale
    usize leaf_triangles() const;
    usize root_count() const;
};

// False (with a reason) for bad parameters, an index count that is not a multiple of 3, or an index outside the positions. An empty mesh succeeds with
// nothing. Deterministic.
bool BuildClusterHierarchy(std::span<const Vec3> positions, std::span<const u32> indices, const ClusterParams& params, ClusterHierarchy& out,
                           std::string* error = nullptr);

// What the viewer is: where it is, how tall the picture is and how wide it sees, and optionally its frustum as six inward-facing planes (a point is inside
// when dot(normal, p) + d >= 0 for all of them).
struct ClusterPlane {
    Vec3 normal{0, 0, 0}; // pointing into the frustum
    f32 d = 0.0f;
};

struct ClusterView {
    Vec3 position{0, 0, 0};
    f32 vertical_fov = 1.0f;       // radians
    f32 viewport_height = 1080.0f; // pixels
    f32 near_distance = 0.1f;      // distances are held to at least this, so nothing is infinitely large on screen
    bool cull_backfaces = false;   // drop clusters whose triangles all face away: dot(normalize(centre - position), cone axis) >= cone cutoff + radius / distance
    bool has_frustum = false;
    std::array<ClusterPlane, 6> frustum{};
};

// How many pixels an object-space error of `error` at a sphere of `radius` around `center` covers, for the nearest point of that sphere. A bigger
// error or a closer sphere is a bigger number. Infinite for a +FLT_MAX error, 0 for an error of 0.
f32 ProjectedError(const Vec3& center, f32 radius, f32 error, const ClusterView& view);

struct ClusterSelection {
    std::vector<u32> clusters; // indices into the hierarchy, in cluster order
    usize triangles = 0;       // what they add up to
};

// The clusters to draw: the cut through the hierarchy where each cluster's own error projects to at most `threshold_pixels` and its parent's to more than
// that, then (with a frustum) only the ones in view, then (with cull_backfaces) only the ones that can face the viewer. Without a frustum the result covers the whole mesh exactly once. A threshold of 0 or less gives the
// finest cut (the leaves, or the coarsest clusters that lose nothing). Deterministic. False for a threshold or view that is not a number.
bool SelectClusters(const ClusterHierarchy& hierarchy, const ClusterView& view, f32 threshold_pixels, ClusterSelection& out);

} // namespace aether::cook
