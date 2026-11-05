#pragma once

// @stability: experimental
//
// Meshlets and LODs for the cooker (Phase 39 step 5 and Phase 42 step 4, docs/design/UPGRADE_PLAN.md).
// Offline, CPU-only steps on an indexed triangle mesh, built on meshoptimizer (MIT, hidden behind the .cpp):
//
//   BuildMeshlets  splits a mesh into small clusters (at most 64 vertices and 124 triangles by default) with
//                  a bounding sphere and normal cone each, the unit a virtual-geometry or mesh-shader
//                  renderer culls by.
//   GenerateLods   simplifies a mesh to a chain of coarser versions for distance LODs.
//
// The results are in memory only; mesh_cook.h writes them into the cooked mesh (39.6a).

#include "aether/core/base.h"
#include "aether/math/math.h"

#include <span>
#include <string>
#include <vector>

namespace aether::cook {

struct MeshletParams {
    u32 max_vertices = 64;   // 3 to 255
    u32 max_triangles = 124; // 1 to 512, a multiple of 4
    f32 cone_weight = 0.0f;  // 0 packs for fewest meshlets; towards 1 favours tight normal cones (better culling)
};

struct Meshlet {
    u32 vertex_offset = 0;   // into MeshletMesh::vertices
    u32 triangle_offset = 0; // into MeshletMesh::triangles (3 bytes per triangle, indices into this meshlet's vertices)
    u32 vertex_count = 0;
    u32 triangle_count = 0;
};

struct MeshletBounds {
    Vec3 center;
    f32 radius = 0.0f;
    Vec3 cone_axis;
    f32 cone_cutoff = 1.0f; // cull the meshlet when dot(normalize(center - camera), cone_axis) >= cone_cutoff + radius / distance(center, camera)
};

struct MeshletMesh {
    std::vector<Meshlet> meshlets;
    std::vector<u32> vertices;   // global vertex indices, per meshlet
    std::vector<u8> triangles;   // local indices, 3 per triangle
    std::vector<MeshletBounds> bounds;
};

// False (with a reason) for bad parameters, an index count that isn't a multiple of 3, or an index outside the
// positions. An empty mesh succeeds with no meshlets. The output is deterministic.
bool BuildMeshlets(std::span<const Vec3> positions, std::span<const u32> indices, const MeshletParams& params,
                   MeshletMesh& out, std::string* error = nullptr);

struct LodParams {
    // Each level keeps about this fraction of the original triangles, in decreasing order.
    std::vector<f32> ratios = {0.5f, 0.25f, 0.125f};
    // The largest error a level may add, as a fraction of the mesh's size; a level stops short of its ratio to stay under it.
    f32 target_error = 0.05f;
    bool lock_border = true; // keep open edges in place (so neighbouring pieces still meet)
};

struct LodLevel {
    std::vector<u32> indices; // into the same vertex array as the source mesh
    f32 error = 0.0f;         // relative to the mesh's extent
    usize triangles() const { return indices.size() / 3; }
};

// Level 0 is the source mesh; the rest follow `params.ratios`. Triangle counts never increase from one level to the next.
bool GenerateLods(std::span<const Vec3> positions, std::span<const u32> indices, const LodParams& params,
                  std::vector<LodLevel>& out, std::string* error = nullptr);

// HLOD (Phase 42.4): concatenates several indexed meshes into one proxy's geometry,
// offsetting each source's indices into the merged vertex array. Feed the result to
// GenerateLods for the proxy's distance LOD chain. False for mismatched
// position/index lists, a partial triangle or an index outside its own source.
// Deterministic (sources in order).
bool MergeMeshes(const std::vector<std::span<const Vec3>>& source_positions,
                 const std::vector<std::span<const u32>>& source_indices, std::vector<Vec3>& out_positions,
                 std::vector<u32>& out_indices, std::string* error = nullptr);

// One HLOD proxy: the sources it stands in for and the centre used to place it.
struct HlodCluster {
    Vec3 center;
    std::vector<usize> sources; // indices into the source list
};

// Groups sources into HLOD proxies by proximity: a source joins the nearest
// cluster whose centre is within `cluster_radius`, else starts a new one; the
// cluster centre follows the mean of its sources. Deterministic (sources in
// order). `cluster_radius <= 0` makes every source its own cluster.
std::vector<HlodCluster> ClusterForHlod(const std::vector<Vec3>& source_centers, f32 cluster_radius);

// One baked HLOD proxy: the merged geometry of a cluster and its distance LOD chain.
struct HlodProxy {
    Vec3 center;
    std::vector<usize> sources; // indices into the source lists, as in the cluster
    std::vector<Vec3> positions;
    std::vector<u32> indices;
    std::vector<LodLevel> lods; // into `positions`
};
// Builds one proxy for `cluster`: merges the sources it references and generates
// the merged mesh's LOD chain. False for an out-of-range source or a merge/LOD failure.
bool BuildHlodProxy(const std::vector<std::span<const Vec3>>& source_positions,
                    const std::vector<std::span<const u32>>& source_indices, const HlodCluster& cluster,
                    const LodParams& lod_params, HlodProxy& out, std::string* error = nullptr);

} // namespace aether::cook
