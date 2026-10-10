#include "test_framework.h"

#include "aether/cook/meshlet_cook.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

// Meshlets and LODs (Phase 39 step 5, Phase 42 step 4): every triangle lands in exactly one meshlet, the
// limits hold, the bounds contain their vertices, the build is deterministic, and LODs shrink.

using namespace aether;
using namespace aether::cook;

namespace {

// An n x n grid of quads in the XZ plane with a gentle bump, as 2 triangles each.
void MakeGrid(u32 n, std::vector<Vec3>& positions, std::vector<u32>& indices) {
    positions.clear();
    indices.clear();
    for (u32 z = 0; z <= n; ++z) {
        for (u32 x = 0; x <= n; ++x) {
            const f32 fx = static_cast<f32>(x), fz = static_cast<f32>(z);
            positions.push_back(Vec3(fx, 0.3f * std::sin(fx * 0.5f) * std::cos(fz * 0.5f), fz));
        }
    }
    for (u32 z = 0; z < n; ++z) {
        for (u32 x = 0; x < n; ++x) {
            const u32 a = z * (n + 1) + x, b = a + 1, c = a + (n + 1), d = c + 1;
            for (u32 i : {a, c, b, b, c, d}) indices.push_back(i);
        }
    }
}

// A triangle as a sorted vertex triple, so winding and rotation don't matter for the coverage check.
std::array<u32, 3> Key(u32 a, u32 b, u32 c) {
    std::array<u32, 3> k = {a, b, c};
    std::sort(k.begin(), k.end());
    return k;
}

} // namespace

AETHER_TEST(Meshlet_EveryTriangleIsInExactlyOneMeshletWithinTheLimits) {
    std::vector<Vec3> positions;
    std::vector<u32> indices;
    MakeGrid(48, positions, indices); // 4608 triangles
    MeshletMesh mesh;
    std::string error;
    AETHER_CHECK(BuildMeshlets(positions, indices, MeshletParams{}, mesh, &error));
    AETHER_CHECK(!mesh.meshlets.empty() && mesh.bounds.size() == mesh.meshlets.size());

    std::map<std::array<u32, 3>, int> seen;
    for (const Meshlet& m : mesh.meshlets) {
        AETHER_CHECK(m.vertex_count >= 3 && m.vertex_count <= 64 && m.triangle_count >= 1 && m.triangle_count <= 124);
        for (u32 t = 0; t < m.triangle_count; ++t) {
            u32 g[3];
            for (u32 k = 0; k < 3; ++k) {
                const u8 local = mesh.triangles[m.triangle_offset + t * 3 + k];
                AETHER_CHECK(local < m.vertex_count);
                g[k] = mesh.vertices[m.vertex_offset + local];
                AETHER_CHECK(g[k] < positions.size());
            }
            ++seen[Key(g[0], g[1], g[2])];
        }
    }
    std::map<std::array<u32, 3>, int> expected;
    for (usize i = 0; i < indices.size(); i += 3) ++expected[Key(indices[i], indices[i + 1], indices[i + 2])];
    AETHER_CHECK(seen == expected);
}

AETHER_TEST(Meshlet_BoundsContainTheirVerticesAndBuildsAreDeterministic) {
    std::vector<Vec3> positions;
    std::vector<u32> indices;
    MakeGrid(32, positions, indices);
    MeshletParams params;
    params.max_vertices = 32;
    params.max_triangles = 48;
    params.cone_weight = 0.5f;
    MeshletMesh a, b;
    AETHER_CHECK(BuildMeshlets(positions, indices, params, a) && BuildMeshlets(positions, indices, params, b));
    AETHER_CHECK(a.vertices == b.vertices && a.triangles == b.triangles && a.meshlets.size() == b.meshlets.size());
    for (usize i = 0; i < a.meshlets.size(); ++i) {
        AETHER_CHECK(a.meshlets[i].vertex_count <= 32 && a.meshlets[i].triangle_count <= 48);
        const MeshletBounds& bounds = a.bounds[i];
        for (u32 v = 0; v < a.meshlets[i].vertex_count; ++v) {
            const Vec3& p = positions[a.vertices[a.meshlets[i].vertex_offset + v]];
            const f32 dx = p.x - bounds.center.x, dy = p.y - bounds.center.y, dz = p.z - bounds.center.z;
            AETHER_CHECK(std::sqrt(dx * dx + dy * dy + dz * dz) <= bounds.radius + 1e-3f);
        }
    }
}

AETHER_TEST(Meshlet_RejectsBadInputAndAcceptsAnEmptyMesh) {
    std::vector<Vec3> positions = {Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(0, 1, 0)};
    MeshletMesh mesh;
    std::string error;
    AETHER_CHECK(!BuildMeshlets(positions, std::vector<u32>{0, 1}, MeshletParams{}, mesh, &error) && !error.empty()); // not a triangle list
    AETHER_CHECK(!BuildMeshlets(positions, std::vector<u32>{0, 1, 9}, MeshletParams{}, mesh, &error));             // index out of range
    MeshletParams bad;
    bad.max_triangles = 125;
    AETHER_CHECK(!BuildMeshlets(positions, std::vector<u32>{0, 1, 2}, bad, mesh, &error));
    bad = MeshletParams{};
    bad.max_vertices = 2;
    AETHER_CHECK(!BuildMeshlets(positions, std::vector<u32>{0, 1, 2}, bad, mesh, &error));
    AETHER_CHECK(BuildMeshlets(positions, std::vector<u32>{}, MeshletParams{}, mesh) && mesh.meshlets.empty());
    AETHER_CHECK(BuildMeshlets({}, {}, MeshletParams{}, mesh) && mesh.meshlets.empty());
    AETHER_CHECK(BuildMeshlets(positions, std::vector<u32>{0, 1, 2}, MeshletParams{}, mesh) && mesh.meshlets.size() == 1 && mesh.meshlets[0].triangle_count == 1);
}

AETHER_TEST(Lod_LevelsShrinkAndStayInRange) {
    std::vector<Vec3> positions;
    std::vector<u32> indices;
    MakeGrid(40, positions, indices); // 3200 triangles
    std::vector<LodLevel> lods;
    LodParams params;
    params.target_error = 0.2f;
    std::string error;
    AETHER_CHECK(GenerateLods(positions, indices, params, lods, &error));
    AETHER_CHECK(lods.size() == 4 && lods[0].indices == indices && lods[0].error == 0.0f);
    for (usize i = 1; i < lods.size(); ++i) {
        AETHER_CHECK(lods[i].triangles() <= lods[i - 1].triangles());
        AETHER_CHECK(lods[i].indices.size() % 3 == 0 && lods[i].triangles() > 0);
        AETHER_CHECK(lods[i].error >= 0.0f && lods[i].error <= params.target_error + 1e-4f);
        for (const u32 index : lods[i].indices) AETHER_CHECK(index < positions.size());
    }
    AETHER_CHECK(lods[1].triangles() < lods[0].triangles() * 3 / 4);
    AETHER_CHECK(lods[3].triangles() < lods[1].triangles());

    std::vector<LodLevel> again;
    AETHER_CHECK(GenerateLods(positions, indices, params, again) && again.size() == lods.size() && again[2].indices == lods[2].indices);
}

AETHER_TEST(Lod_RejectsBadInputAndHandlesEmptyAndTinyMeshes) {
    std::vector<Vec3> positions = {Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(0, 1, 0)};
    std::vector<LodLevel> lods;
    std::string error;
    LodParams increasing;
    increasing.ratios = {0.25f, 0.5f};
    AETHER_CHECK(!GenerateLods(positions, std::vector<u32>{0, 1, 2}, increasing, lods, &error) && !error.empty());
    LodParams out_of_range;
    out_of_range.ratios = {1.5f};
    AETHER_CHECK(!GenerateLods(positions, std::vector<u32>{0, 1, 2}, out_of_range, lods));
    AETHER_CHECK(!GenerateLods(positions, std::vector<u32>{0, 1, 7}, LodParams{}, lods));
    AETHER_CHECK(GenerateLods(positions, std::vector<u32>{}, LodParams{}, lods) && lods.size() == 1 && lods[0].indices.empty());
    // One triangle cannot shrink: every level keeps it.
    AETHER_CHECK(GenerateLods(positions, std::vector<u32>{0, 1, 2}, LodParams{}, lods) && lods.size() == 4);
    for (const LodLevel& level : lods) AETHER_CHECK(level.triangles() == 1);
}
