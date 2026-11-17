#pragma once

#include "cluster_cull_pass.h"
#include "view_cull.h"

#include "aether/assets/model_importer.h"
#include "aether/cook/cluster_hierarchy.h"
#include "aether/gfx/rhi/device.h"
#include "aether/math/math.h"

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace aether::player {

// Stage 4 of virtual geometry (docs/design/VIRTUAL_GEOMETRY.md): one mesh primitive the scene renderer draws through the cluster path. It holds the cluster
// hierarchy built from the primitive's own vertices and indices (the leaves are the mesh exactly, so a cut of 0 pixels draws what the plain draw draws), the index
// buffer the draw reads (the hierarchy's `indices`, in the clusters' order) and the GPU passes that choose the clusters. The vertex buffer stays the primitive's.
//
// A pass writes its constants and counters from the CPU and its draw list on the GPU, so each frame in flight needs its own, and so does each draw of the primitive
// in one frame (entities sharing the model): Pass(device, slot, use) makes them when first asked for.
class ClusterModel {
public:
    // Builds the hierarchy of a primitive (cook::BuildPrimitiveClusters, which welds alike vertices first) and its index buffer. False (with a reason) for a mesh the
    // hierarchy cannot be built on, or a device that makes no index buffer.
    bool Build(gfx::rhi::IDevice& device, std::span<const assets::MeshVertex> vertices, std::span<const u32> indices, std::string* error = nullptr);
    ~ClusterModel();
    ClusterModel() = default;
    ClusterModel(const ClusterModel&) = delete;
    ClusterModel& operator=(const ClusterModel&) = delete;

    // The same from a hierarchy that is already built (one cooked into the pak): its indices must be in the numbering of the vertex buffer the draw will use, which
    // has `vertex_count` vertices and `index_count` indices. False (with a reason) when it does not fit that mesh: no clusters, an index past the vertices, or
    // leaves that hold other than the mesh's triangles.
    bool Adopt(gfx::rhi::IDevice& device, cook::ClusterHierarchy hierarchy, usize vertex_count, usize index_count, std::string* error = nullptr);
    const cook::ClusterHierarchy& Hierarchy() const { return hierarchy_; }
    gfx::rhi::BufferHandle IndexBuffer() const { return index_buffer_; }
    u64 FullTriangles() const { return full_triangles_; }
    // The pass for frame slot `slot` and the `use`th draw of this primitive in that frame (null if the device could not make it).
    ClusterCullPass* Pass(gfx::rhi::IDevice& device, u32 slot, u32 use);
    static constexpr u32 kMaxUsesPerFrame = 8;

private:
    gfx::rhi::IDevice* device_ = nullptr;
    cook::ClusterHierarchy hierarchy_;
    gfx::rhi::BufferHandle index_buffer_;
    u64 full_triangles_ = 0;
    std::vector<std::vector<std::unique_ptr<ClusterCullPass>>> passes_; // [slot][use]
};

// What the cull needs of a view, in the primitive's own space: the camera's position there, and the world frustum's planes brought through the model matrix.
// False when the model matrix cannot be inverted, scales its axes unequally (more than 1% apart: the pixel error would not be one number), mirrors, or the frustum
// has no size for the projection (`pixels_per_unit_at_one` is 0). `cull_backfaces` asks for the normal-cone test too.
bool LocalClusterView(const Mat4& model, const Vec3& camera_world, const ViewFrustum& frustum, f32 viewport_height, bool cull_backfaces, cook::ClusterView& out);

} // namespace aether::player
