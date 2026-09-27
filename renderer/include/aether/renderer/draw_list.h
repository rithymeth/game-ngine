#pragma once

#include "aether/renderer/view.h"

#include <vector>

namespace aether {

// The objects a view can see (indices into RenderScene::objects, in order).
std::vector<u32> Cull(const RenderScene& scene, const View& view);

// Draws of one mesh: instances share the mesh and are drawn with one
// instanced draw call.
struct DrawBatch {
    u64 mesh_key = 0;
    std::vector<u32> instances; // object indices, nearest first
    f32 nearest = 0.0f;         // distance from the view to the nearest instance
};

// Opaque draws for a view: visible objects grouped by mesh, batches ordered
// front to back by their nearest instance (so the depth pre-pass rejects
// as much as possible), instances front to back too.
struct DrawList {
    std::vector<DrawBatch> batches;
    usize InstanceCount() const;
};
DrawList BuildDrawList(const RenderScene& scene, const View& view, const std::vector<u32>& visible);

} // namespace aether
