#include "world/model_bounds.h"

#include "aether/assets/gltf_loader.h"

#include <algorithm>
#include <limits>

namespace aether::editor {

void ModelBoundsCache::SetContentRoot(std::filesystem::path content_root) {
    if (content_root == content_root_) return;
    content_root_ = std::move(content_root);
    cache_.clear();
}

const ModelBoundsCache::Entry* ModelBoundsCache::Find(const std::string& asset_path) const {
    if (asset_path.empty() || content_root_.empty()) return nullptr;
    auto found = cache_.find(asset_path);
    if (found == cache_.end()) {
        Entry entry;
        assets::GltfScene scene;
        std::error_code ec;
        const std::filesystem::path file = content_root_ / asset_path;
        if (std::filesystem::is_regular_file(file, ec) && assets::LoadGltf(file.string(), scene)) {
            constexpr f32 kInf = std::numeric_limits<f32>::max();
            Vec3 low(kInf, kInf, kInf), high(-kInf, -kInf, -kInf);
            bool any = false;
            usize triangle_count = 0;
            for (const assets::GltfNodeInstance& instance : scene.node_instances) {
                if (instance.mesh_index >= scene.meshes.size()) continue;
                for (const assets::GltfPrimitive& primitive : scene.meshes[instance.mesh_index].primitives) {
                    triangle_count += primitive.indices.size() / 3;
                    for (const assets::GltfVertex& vertex : primitive.vertices) {
                        const Vec4 p = instance.world_transform * Vec4(vertex.position[0], vertex.position[1], vertex.position[2], 1.0f);
                        low = Vec3(std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z));
                        high = Vec3(std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z));
                        any = true;
                    }
                }
            }
            if (any && high.x >= low.x && high.y >= low.y && high.z >= low.z) {
                entry.valid = true;
                entry.bounds.min = low;
                entry.bounds.max = high;
                if (triangle_count > 0 && triangle_count <= kMaxPickTriangles) { // the shape too, unless it is too big to test a ray against on every mouse move
                    for (const assets::GltfNodeInstance& instance : scene.node_instances) {
                        if (instance.mesh_index >= scene.meshes.size()) continue;
                        for (const assets::GltfPrimitive& primitive : scene.meshes[instance.mesh_index].primitives) {
                            const u32 first = static_cast<u32>(entry.triangles.positions.size());
                            for (const assets::GltfVertex& vertex : primitive.vertices) {
                                const Vec4 p = instance.world_transform * Vec4(vertex.position[0], vertex.position[1], vertex.position[2], 1.0f);
                                entry.triangles.positions.push_back(Vec3(p.x, p.y, p.z));
                            }
                            for (usize k = 0; k + 2 < primitive.indices.size(); k += 3) {
                                const u32 a = primitive.indices[k], b = primitive.indices[k + 1], c = primitive.indices[k + 2];
                                if (a >= primitive.vertices.size() || b >= primitive.vertices.size() || c >= primitive.vertices.size()) continue; // a bad index is skipped, not read
                                entry.triangles.indices.push_back(first + a);
                                entry.triangles.indices.push_back(first + b);
                                entry.triangles.indices.push_back(first + c);
                            }
                        }
                    }
                }
            }
        }
        found = cache_.emplace(asset_path, std::move(entry)).first;
    }
    return &found->second;
}

bool ModelBoundsCache::Get(const std::string& asset_path, LocalBounds& out) const {
    const Entry* entry = Find(asset_path);
    if (entry == nullptr || !entry->valid) return false;
    out = entry->bounds;
    return true;
}

const ModelTriangles* ModelBoundsCache::Triangles(const std::string& asset_path) const {
    const Entry* entry = Find(asset_path);
    return entry != nullptr && entry->valid && !entry->triangles.indices.empty() ? &entry->triangles : nullptr;
}

} // namespace aether::editor
