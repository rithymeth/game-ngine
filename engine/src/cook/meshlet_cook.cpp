#include "aether/cook/meshlet_cook.h"

#include <meshoptimizer.h>

#include <algorithm>

namespace aether::cook {

namespace {

static_assert(sizeof(Vec3) == 3 * sizeof(f32), "meshoptimizer reads positions as tightly packed floats");

bool Fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

bool CheckMesh(std::span<const Vec3> positions, std::span<const u32> indices, std::string* error) {
    if (indices.size() % 3 != 0) return Fail(error, "the index count isn't a multiple of 3");
    for (const u32 i : indices) {
        if (i >= positions.size()) return Fail(error, "an index is outside the vertex array");
    }
    return true;
}

} // namespace

bool BuildMeshlets(std::span<const Vec3> positions, std::span<const u32> indices, const MeshletParams& params,
                   MeshletMesh& out, std::string* error) {
    out = MeshletMesh();
    if (params.max_vertices < 3 || params.max_vertices > 255) return Fail(error, "max_vertices must be 3 to 255");
    if (params.max_triangles < 4 || params.max_triangles > 512 || params.max_triangles % 4 != 0) {
        return Fail(error, "max_triangles must be a multiple of 4, 4 to 512");
    }
    if (!(params.cone_weight >= 0.0f && params.cone_weight <= 1.0f)) return Fail(error, "cone_weight must be 0 to 1");
    if (!CheckMesh(positions, indices, error)) return false;
    if (indices.empty()) return true;

    const usize max_meshlets = meshopt_buildMeshletsBound(indices.size(), params.max_vertices, params.max_triangles);
    std::vector<meshopt_Meshlet> built(max_meshlets);
    std::vector<u32> vertices(max_meshlets * params.max_vertices);
    std::vector<u8> triangles(max_meshlets * params.max_triangles * 3);
    const usize count = meshopt_buildMeshlets(built.data(), vertices.data(), triangles.data(), indices.data(), indices.size(),
                                              &positions[0].x, positions.size(), sizeof(Vec3), params.max_vertices,
                                              params.max_triangles, params.cone_weight);
    built.resize(count);
    const meshopt_Meshlet& last = built.back();
    vertices.resize(last.vertex_offset + last.vertex_count);
    triangles.resize(last.triangle_offset + ((last.triangle_count * 3 + 3) & ~3u));

    out.vertices = std::move(vertices);
    out.triangles = std::move(triangles);
    out.meshlets.reserve(count);
    out.bounds.reserve(count);
    for (const meshopt_Meshlet& m : built) {
        out.meshlets.push_back({m.vertex_offset, m.triangle_offset, m.vertex_count, m.triangle_count});
        const meshopt_Bounds b = meshopt_computeMeshletBounds(&out.vertices[m.vertex_offset], &out.triangles[m.triangle_offset],
                                                              m.triangle_count, &positions[0].x, positions.size(), sizeof(Vec3));
        MeshletBounds bounds;
        bounds.center = Vec3(b.center[0], b.center[1], b.center[2]);
        bounds.radius = b.radius;
        bounds.cone_axis = Vec3(b.cone_axis[0], b.cone_axis[1], b.cone_axis[2]);
        bounds.cone_cutoff = b.cone_cutoff;
        out.bounds.push_back(bounds);
    }
    return true;
}

bool GenerateLods(std::span<const Vec3> positions, std::span<const u32> indices, const LodParams& params,
                  std::vector<LodLevel>& out, std::string* error) {
    out.clear();
    if (!CheckMesh(positions, indices, error)) return false;
    f32 previous_ratio = 1.0f;
    for (const f32 r : params.ratios) {
        if (!(r > 0.0f && r < previous_ratio)) return Fail(error, "ratios must decrease, each between 0 and 1");
        previous_ratio = r;
    }
    if (!(params.target_error >= 0.0f && params.target_error <= 1.0f)) return Fail(error, "target_error must be 0 to 1");

    LodLevel base;
    base.indices.assign(indices.begin(), indices.end());
    out.push_back(std::move(base));
    if (indices.empty()) return true;

    const unsigned options = params.lock_border ? meshopt_SimplifyLockBorder : 0u;
    for (const f32 ratio : params.ratios) {
        const usize target = std::max<usize>(3, (static_cast<usize>(static_cast<f64>(indices.size()) * ratio) / 3) * 3);
        LodLevel level;
        level.indices.resize(indices.size());
        f32 result_error = 0.0f;
        const usize produced = meshopt_simplify(level.indices.data(), indices.data(), indices.size(), &positions[0].x,
                                                positions.size(), sizeof(Vec3), target, params.target_error, options, &result_error);
        level.indices.resize(produced);
        level.error = result_error; // meshoptimizer reports it relative to the mesh's extent
        // Never larger than the level before it.
        if (level.indices.size() > out.back().indices.size()) level.indices = out.back().indices;
        out.push_back(std::move(level));
    }
    return true;
}

} // namespace aether::cook
