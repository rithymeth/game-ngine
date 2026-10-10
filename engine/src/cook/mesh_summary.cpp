#include "aether/cook/mesh_summary.h"

#include <algorithm>

namespace aether::cook {

MeshSummary SummarizeMeshLods(const assets::GltfScene& scene, const MeshSummaryOptions& options) {
    MeshSummary out;
    u64 total_triangles = 0;
    for (const assets::GltfMesh& mesh : scene.meshes)
        for (const assets::GltfPrimitive& prim : mesh.primitives) total_triangles += prim.indices.size() / 3;
    if (total_triangles > options.max_triangles) {
        out.message = "Too large to summarise here (" + std::to_string(total_triangles) + " triangles)";
        return out;
    }

    const usize levels = options.lods.ratios.size() + 1;
    out.lods.resize(levels);
    for (usize i = 0; i < levels; ++i) out.lods[i].level = static_cast<u32>(i);

    std::vector<Vec3> positions;
    for (const assets::GltfMesh& mesh : scene.meshes) {
        for (const assets::GltfPrimitive& prim : mesh.primitives) {
            ++out.primitives;
            out.vertices += prim.vertices.size();
            const bool skinned = !prim.joint_indices.empty();
            if (skinned || prim.indices.empty()) {
                ++out.skipped_primitives;
                continue;
            }
            positions.clear();
            positions.reserve(prim.vertices.size());
            for (const assets::GltfVertex& v : prim.vertices) positions.emplace_back(v.position[0], v.position[1], v.position[2]);

            std::vector<LodLevel> lods;
            std::string reason;
            if (!GenerateLods(positions, prim.indices, options.lods, lods, &reason)) {
                out.message = "A mesh can't be simplified: " + reason;
                out.lods.clear();
                return out;
            }
            for (usize i = 0; i < lods.size() && i < levels; ++i) {
                MeshletMesh meshlets;
                if (!BuildMeshlets(positions, lods[i].indices, options.meshlets, meshlets, &reason)) {
                    out.message = "A mesh can't be split into meshlets: " + reason;
                    out.lods.clear();
                    return out;
                }
                out.lods[i].triangles += lods[i].triangles();
                out.lods[i].meshlets += meshlets.meshlets.size();
                out.lods[i].error = std::max(out.lods[i].error, lods[i].error);
            }
        }
    }
    out.ok = true;
    if (out.skipped_primitives > 0)
        out.message = std::to_string(out.skipped_primitives) + " skinned or empty primitive(s) are not included";
    return out;
}

} // namespace aether::cook
