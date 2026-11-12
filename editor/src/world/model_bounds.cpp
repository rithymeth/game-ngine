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

bool ModelBoundsCache::Get(const std::string& asset_path, LocalBounds& out) const {
    if (asset_path.empty() || content_root_.empty()) return false;
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
            for (const assets::GltfNodeInstance& instance : scene.node_instances) {
                if (instance.mesh_index >= scene.meshes.size()) continue;
                for (const assets::GltfPrimitive& primitive : scene.meshes[instance.mesh_index].primitives) {
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
            }
        }
        found = cache_.emplace(asset_path, entry).first;
    }
    if (!found->second.valid) return false;
    out = found->second.bounds;
    return true;
}

} // namespace aether::editor
