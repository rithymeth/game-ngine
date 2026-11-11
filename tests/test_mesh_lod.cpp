#include "../player/src/mesh_lod.h"
#include "aether/assets/model_importer.h"
#include "aether/assets/procedural_mesh.h"
#include "test_framework.h"

#include <algorithm>

using namespace aether;
using namespace aether::player;
using namespace aether::assets::procedural;

// The far level of detail for plants: a cheaper index list over the same vertices.

namespace {

struct Flat {
    std::vector<Vec3> positions;
    std::vector<u32> indices;
};

// A mesh's primitives as one position list and one index list.
Flat Merge(const ProceduralMesh& m) {
    Flat out;
    for (const assets::MeshPrimitiveData& p : m.mesh.primitives) {
        const u32 base = static_cast<u32>(out.positions.size());
        for (const assets::MeshVertex& v : p.vertices) out.positions.push_back(Vec3(v.position[0], v.position[1], v.position[2]));
        for (const u32 i : p.indices) out.indices.push_back(base + i);
    }
    return out;
}

void Bounds(const std::vector<Vec3>& positions, const std::vector<u32>& indices, Vec3& lo, Vec3& hi) {
    lo = Vec3(1e30f, 1e30f, 1e30f);
    hi = Vec3(-1e30f, -1e30f, -1e30f);
    for (const u32 i : indices) {
        const Vec3& p = positions[i];
        lo = Vec3(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
        hi = Vec3(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
    }
}

} // namespace

AETHER_TEST(MeshLod_PlantsLoseMostTrianglesAndKeepTheirShape) {
    for (const ProceduralKind kind : {ProceduralKind::Shrub, ProceduralKind::PineTree, ProceduralKind::BroadleafTree, ProceduralKind::OakTree}) {
        u32 with_lod = 0;
        for (u32 variant = 0; variant < 6; ++variant) {
            const Flat mesh = Merge(GenerateVariant(kind, variant, 7));
            const std::vector<u32> lod = SimplifyIndices(mesh.positions, mesh.indices, LodTargetRatio(mesh.indices.size() / 3));
            if (lod.empty()) continue; // a mesh already coarse enough has none
            ++with_lod;
            AETHER_CHECK(lod.size() % 3 == 0);
            AETHER_CHECK(lod.size() <= mesh.indices.size() * 7 / 10);
            for (usize t = 0; t < lod.size(); t += 3) {
                AETHER_CHECK(lod[t] < mesh.positions.size() && lod[t + 1] < mesh.positions.size() && lod[t + 2] < mesh.positions.size());
                AETHER_CHECK(lod[t] != lod[t + 1] && lod[t + 1] != lod[t + 2] && lod[t] != lod[t + 2]); // no degenerate triangle
            }
            // The silhouette: the box that holds it shrinks by little.
            Vec3 lo, hi, lod_lo, lod_hi;
            Bounds(mesh.positions, mesh.indices, lo, hi);
            Bounds(mesh.positions, lod, lod_lo, lod_hi);
            const Vec3 e = hi - lo, le = lod_hi - lod_lo;
            const f32 height = le.y / e.y, width = std::max(le.x, le.z) / std::max(e.x, e.z);
            if (variant == 0) std::printf("    %s variant %u: %zu -> %zu triangles, height %.2f, width %.2f of the full mesh's\n", ProceduralKindName(kind), variant, mesh.indices.size() / 3, lod.size() / 3, height, width);
            AETHER_CHECK(height > 0.9f && height <= 1.001f);
            AETHER_CHECK(width > 0.85f && width <= 1.001f);
        }
        std::printf("    %s: %u of 6 variants have a far level\n", ProceduralKindName(kind), with_lod);
        AETHER_CHECK(with_lod >= 4);
    }
}

AETHER_TEST(MeshLod_AimsForTheTargetRatio) {
    // A 33 x 33 grid (2,048 triangles): a quarter is about 512, within a factor of two either way.
    Flat grid;
    constexpr u32 n = 33;
    for (u32 z = 0; z < n; ++z)
        for (u32 x = 0; x < n; ++x) grid.positions.push_back(Vec3(static_cast<f32>(x), 0.0f, static_cast<f32>(z)));
    for (u32 z = 0; z + 1 < n; ++z)
        for (u32 x = 0; x + 1 < n; ++x) {
            const u32 a = z * n + x, b = a + 1, c = a + n, d = c + 1;
            grid.indices.insert(grid.indices.end(), {a, c, b, b, c, d});
        }
    const std::vector<u32> lod = SimplifyIndices(grid.positions, grid.indices, 0.25f);
    const f32 ratio = static_cast<f32>(lod.size()) / static_cast<f32>(grid.indices.size());
    std::printf("    a 2,048 triangle grid at target 0.25 keeps %.2f\n", ratio);
    AETHER_CHECK(!lod.empty() && ratio > 0.1f && ratio <= 0.25f);
    Vec3 lo, hi;
    Bounds(grid.positions, lod, lo, hi);
    AETHER_CHECK(hi.x - lo.x > 28.0f && hi.z - lo.z > 28.0f); // still covers the grid
}

AETHER_TEST(MeshLod_ATinyOrFlatMeshHasNone) {
    Flat quad;
    quad.positions = {Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(0, 0, 1), Vec3(1, 0, 1)};
    quad.indices = {0, 2, 1, 1, 2, 3};
    AETHER_CHECK(SimplifyIndices(quad.positions, quad.indices).empty());
    AETHER_CHECK(SimplifyIndices({}, {}).empty());
    // Indices out of range are skipped, not read.
    Flat bad = Merge(GenerateVariant(ProceduralKind::Shrub, 0, 3));
    bad.indices.push_back(99999);
    bad.indices.push_back(0);
    bad.indices.push_back(1);
    const std::vector<u32> lod = SimplifyIndices(bad.positions, bad.indices);
    for (const u32 i : lod) AETHER_CHECK(i < bad.positions.size());
}

AETHER_TEST(MeshLod_IsTheSameEveryTime) {
    const Flat mesh = Merge(GenerateVariant(ProceduralKind::PineTree, 2, 11));
    AETHER_CHECK(SimplifyIndices(mesh.positions, mesh.indices) == SimplifyIndices(mesh.positions, mesh.indices));
}
