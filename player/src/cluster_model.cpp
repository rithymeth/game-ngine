#include "cluster_model.h"

#include <algorithm>
#include <cmath>

namespace aether::player {

using namespace gfx::rhi;

ClusterModel::~ClusterModel() {
    passes_.clear(); // each pass releases its buffers
    if (device_ != nullptr && index_buffer_.IsValid()) device_->DestroyBuffer(index_buffer_);
}

bool ClusterModel::Build(IDevice& device, std::span<const Vec3> positions, std::span<const u32> indices, std::span<const u32> vertex_ids, std::string* error) {
    cook::ClusterParams params;
    if (!cook::BuildClusterHierarchy(positions, indices, params, hierarchy_, error)) return false;
    if (hierarchy_.clusters.empty() || hierarchy_.indices.empty()) {
        if (error) *error = "the mesh has no clusters";
        return false;
    }
    if (!vertex_ids.empty()) {
        if (vertex_ids.size() != positions.size()) {
            if (error) *error = "vertex_ids must have one entry for each position";
            return false;
        }
        for (u32& index : hierarchy_.indices) index = vertex_ids[index];
    }
    index_buffer_ = device.CreateIndexBuffer(hierarchy_.indices.data(), hierarchy_.indices.size() * sizeof(u32), IndexFormat::UInt32);
    if (!index_buffer_.IsValid()) {
        if (error) *error = "the device could not make the cluster index buffer";
        return false;
    }
    device_ = &device;
    full_triangles_ = indices.size() / 3;
    return true;
}

ClusterCullPass* ClusterModel::Pass(IDevice& device, u32 slot, u32 use) {
    if (use >= kMaxUsesPerFrame || slot > 16) return nullptr;
    if (passes_.size() <= slot) passes_.resize(slot + 1);
    auto& pool = passes_[slot];
    while (pool.size() <= use) pool.push_back(nullptr);
    if (!pool[use]) {
        auto pass = std::make_unique<ClusterCullPass>();
        std::string error;
        if (!pass->Initialize(device, hierarchy_, 0, &error)) return nullptr;
        pool[use] = std::move(pass);
    }
    return pool[use].get();
}

bool LocalClusterView(const Mat4& model, const Vec3& camera_world, const ViewFrustum& frustum, f32 viewport_height, bool cull_backfaces, cook::ClusterView& out) {
    if (!(frustum.pixels_per_unit_at_one > 0.0f) || !(viewport_height > 0.0f)) return false;
    const Vec3 axis[3] = {Vec3(model.cols[0].x, model.cols[0].y, model.cols[0].z), Vec3(model.cols[1].x, model.cols[1].y, model.cols[1].z),
                          Vec3(model.cols[2].x, model.cols[2].y, model.cols[2].z)};
    const f32 length[3] = {axis[0].Length(), axis[1].Length(), axis[2].Length()};
    const f32 lo = std::min({length[0], length[1], length[2]}), hi = std::max({length[0], length[1], length[2]});
    if (!(lo > 1e-6f) || hi > lo * 1.01f) return false;
    if (axis[0].Cross(axis[1]).Dot(axis[2]) <= 0.0f) return false; // mirrored: the faces' winding and the cones' sides would be the wrong way round
    Mat4 inverse;
    if (!model.TryInverse(inverse)) return false;
    const Vec4 at = inverse * Vec4(camera_world.x, camera_world.y, camera_world.z, 1.0f);
    if (!(std::abs(at.w) > 1e-9f)) return false;
    out = cook::ClusterView{};
    out.position = Vec3(at.x / at.w, at.y / at.w, at.z / at.w);
    // The projection's size of one unit one unit away is half the viewport height over tan(fov / 2).
    out.vertical_fov = 2.0f * std::atan((0.5f * viewport_height) / frustum.pixels_per_unit_at_one);
    out.viewport_height = viewport_height;
    out.near_distance = 0.05f;
    out.cull_backfaces = cull_backfaces;
    const Vec3 translation(model.cols[3].x, model.cols[3].y, model.cols[3].z);
    for (usize p = 0; p < 6; ++p) {
        // dot(n, A l + t) + d = dot(A^T n, l) + dot(n, t) + d: the plane in the primitive's space, brought to unit length.
        const Vec3 n(frustum.planes[p].x, frustum.planes[p].y, frustum.planes[p].z);
        const Vec3 local(axis[0].Dot(n), axis[1].Dot(n), axis[2].Dot(n));
        const f32 d = n.Dot(translation) + frustum.planes[p].w;
        const f32 len = local.Length();
        if (!(len > 1e-9f)) return false;
        out.frustum[p].normal = local * (1.0f / len);
        out.frustum[p].d = d / len;
    }
    out.has_frustum = true;
    return true;
}

} // namespace aether::player
