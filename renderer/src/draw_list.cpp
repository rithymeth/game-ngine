#include "aether/renderer/draw_list.h"

#include <algorithm>
#include <map>

namespace aether {

std::vector<u32> Cull(const RenderScene& scene, const View& view) {
    std::vector<u32> visible;
    for (u32 i = 0; i < scene.objects.size(); ++i) {
        if (view.frustum.Intersects(scene.objects[i].bounds)) visible.push_back(i);
    }
    return visible;
}

usize DrawList::InstanceCount() const {
    usize n = 0;
    for (const DrawBatch& b : batches) n += b.instances.size();
    return n;
}

DrawList BuildDrawList(const RenderScene& scene, const View& view, const std::vector<u32>& visible) {
    // Distance along the view direction to each bounding box's nearest point.
    auto distance = [&](u32 index) {
        const Aabb& b = scene.objects[index].bounds;
        const Vec3 nearest(std::clamp(view.position.x, b.min.x, b.max.x), std::clamp(view.position.y, b.min.y, b.max.y),
                           std::clamp(view.position.z, b.min.z, b.max.z));
        return (nearest - view.position).Length();
    };
    DrawList list;
    std::map<std::pair<u64, u64>, usize> batch_of;
    for (u32 index : visible) {
        const RenderObject& o = scene.objects[index];
        auto [it, added] = batch_of.try_emplace({o.mesh_key, o.material_key}, list.batches.size());
        if (added) list.batches.push_back({o.mesh_key, o.material_key, {}, 0.0f});
        list.batches[it->second].instances.push_back(index);
    }
    for (DrawBatch& batch : list.batches) {
        std::vector<std::pair<f32, u32>> sorted;
        for (u32 index : batch.instances) sorted.push_back({distance(index), index});
        std::sort(sorted.begin(), sorted.end());
        for (usize i = 0; i < sorted.size(); ++i) batch.instances[i] = sorted[i].second;
        batch.nearest = sorted.empty() ? 0.0f : sorted.front().first;
    }
    std::stable_sort(list.batches.begin(), list.batches.end(), [](const DrawBatch& a, const DrawBatch& b) {
        if (a.nearest != b.nearest) return a.nearest < b.nearest;
        return a.mesh_key != b.mesh_key ? a.mesh_key < b.mesh_key : a.material_key < b.material_key;
    });
    return list;
}

} // namespace aether
