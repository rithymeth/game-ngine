#include "test_framework.h"

#include "aether/cook/mesh_summary.h"

#include <cmath>

// The mesh summary an editor shows (Phase 39 step 6): LOD and meshlet counts that match what the cooker produces.

using namespace aether;
using namespace aether::cook;

namespace {

assets::GltfPrimitive MakeGrid(u32 n) {
    assets::GltfPrimitive prim;
    for (u32 z = 0; z <= n; ++z) {
        for (u32 x = 0; x <= n; ++x) {
            assets::GltfVertex v{};
            v.position[0] = static_cast<f32>(x);
            v.position[1] = 0.3f * std::sin(static_cast<f32>(x) * 0.5f) * std::cos(static_cast<f32>(z) * 0.5f);
            v.position[2] = static_cast<f32>(z);
            prim.vertices.push_back(v);
        }
    }
    for (u32 z = 0; z < n; ++z) {
        for (u32 x = 0; x < n; ++x) {
            const u32 a = z * (n + 1) + x, b = a + 1, c = a + (n + 1), d = c + 1;
            for (u32 i : {a, c, b, b, c, d}) prim.indices.push_back(i);
        }
    }
    return prim;
}

} // namespace

AETHER_TEST(MeshSummary_CountsLodsAndMeshletsLikeTheCooker) {
    assets::GltfScene scene;
    scene.meshes.emplace_back();
    scene.meshes[0].primitives.push_back(MakeGrid(24));
    const MeshSummary s = SummarizeMeshLods(scene);
    AETHER_CHECK(s.ok);
    AETHER_CHECK(s.primitives == 1 && s.skipped_primitives == 0);
    AETHER_CHECK(s.vertices == 25 * 25);
    AETHER_CHECK(s.lods.size() == 4);
    AETHER_CHECK(s.lods[0].triangles == 24 * 24 * 2);
    for (usize i = 1; i < s.lods.size(); ++i) AETHER_CHECK(s.lods[i].triangles <= s.lods[i - 1].triangles);
    AETHER_CHECK(s.lods[0].meshlets > 0);

    // The same numbers as the cooker's own functions.
    std::vector<Vec3> positions;
    for (const assets::GltfVertex& v : scene.meshes[0].primitives[0].vertices) positions.emplace_back(v.position[0], v.position[1], v.position[2]);
    MeshletMesh meshlets;
    AETHER_CHECK(BuildMeshlets(positions, scene.meshes[0].primitives[0].indices, MeshletParams{}, meshlets));
    AETHER_CHECK(s.lods[0].meshlets == meshlets.meshlets.size());
}

AETHER_TEST(MeshSummary_SkipsSkinnedRefusesHugeAndHandlesEmpty) {
    assets::GltfScene scene;
    scene.meshes.emplace_back();
    scene.meshes[0].primitives.push_back(MakeGrid(4));
    assets::GltfPrimitive skinned = MakeGrid(4);
    skinned.joint_indices.assign(skinned.vertices.size(), std::array<u16, 4>{0, 0, 0, 0});
    skinned.joint_weights.assign(skinned.vertices.size(), std::array<f32, 4>{1, 0, 0, 0});
    scene.meshes[0].primitives.push_back(skinned);
    scene.meshes[0].primitives.emplace_back(); // empty
    const MeshSummary s = SummarizeMeshLods(scene);
    AETHER_CHECK(s.ok && s.primitives == 3 && s.skipped_primitives == 2);
    AETHER_CHECK(s.lods[0].triangles == 4 * 4 * 2);
    AETHER_CHECK(!s.message.empty());

    MeshSummaryOptions tiny;
    tiny.max_triangles = 10;
    const MeshSummary huge = SummarizeMeshLods(scene, tiny);
    AETHER_CHECK(!huge.ok && !huge.message.empty() && huge.lods.empty());

    const MeshSummary none = SummarizeMeshLods(assets::GltfScene{});
    AETHER_CHECK(none.ok && none.primitives == 0 && none.lods[0].triangles == 0);
}
