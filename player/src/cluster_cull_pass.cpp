#include "cluster_cull_pass.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace aether::player {

namespace {

using namespace gfx::rhi;

// A cluster, as five float4s (so the layout is the same on every backend): bounds sphere and error, parent sphere and error, the triangle count, the tight
// sphere for culling, and the normal cone. The root's parent error is +FLT_MAX: the shader reads anything at or above 3e38 as "no parent".
constexpr u32 kClusterFloat4s = 5;
constexpr u32 kClusterBytes = kClusterFloat4s * 16;
constexpr u32 kOutputHeaderWords = 2; // count, triangles

struct Constants {
    f32 camera[4];                    // position, pixels per unit
    f32 params[4];                    // near distance, threshold (pixels), cluster count (as bits), capacity (as bits)
    f32 flags[4];                     // frustum on, cone culling on (as bits), 0, 0
    f32 planes[6][4];
};
static_assert(sizeof(Constants) == 144, "the shader's constant buffer is 9 float4s");

f32 Bits(u32 v) {
    f32 f;
    std::memcpy(&f, &v, sizeof(f));
    return f;
}

constexpr char kShader[] = R"(
RWStructuredBuffer<float4> Clusters : register(u0);
RWByteAddressBuffer Output : register(u1);
cbuffer Constants : register(b0, space1) {
    float4 Camera;   // xyz position, w pixels per unit
    float4 Params;   // x near, y threshold, z cluster count (bits), w capacity (bits)
    float4 Flags;    // x frustum on (bits), y cone culling on (bits)
    float4 Planes[6];
};

float Projected(float3 center, float radius, float error) {
    if (!(error > 0.0)) return 0.0;
    if (error >= 3.0e38) return asfloat(0x7f800000); // infinity: no parent
    float distance = max(length(center - Camera.xyz) - radius, max(Params.x, 1.0e-6));
    return error * Camera.w / distance;
}

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    uint index = id.x;
    uint count = asuint(Params.z);
    if (index >= count) return;
    float4 a = Clusters[index * 5 + 0]; // centre, radius
    float4 b = Clusters[index * 5 + 1]; // parent centre, parent radius
    float4 c = Clusters[index * 5 + 2]; // error, parent error, triangles (bits), -
    float4 d = Clusters[index * 5 + 3]; // tight sphere
    float4 e = Clusters[index * 5 + 4]; // cone axis, cutoff
    float threshold = Params.y;
    if (!(Projected(a.xyz, a.w, c.x) <= threshold)) return;
    if (c.y < 3.0e38 && !(Projected(b.xyz, b.w, c.y) > threshold)) return;
    if (asuint(Flags.x) != 0u) {
        for (int p = 0; p < 6; ++p) {
            if (dot(Planes[p].xyz, d.xyz) + Planes[p].w < -d.w) return;
        }
    }
    if (asuint(Flags.y) != 0u) {
        float3 to_cluster = d.xyz - Camera.xyz;
        float dist = length(to_cluster);
        if (dist > 1.0e-6 && dot(to_cluster, e.xyz) >= e.w * dist + d.w) return;
    }
    uint slot, unused;
    Output.InterlockedAdd(0, 1u, slot);           // how many were selected (may pass the capacity: Read reports it)
    Output.InterlockedAdd(4, asuint(c.z), unused); // their triangles
    if (slot < asuint(Params.w)) Output.Store((2 + slot) * 4, index);
}
)";

} // namespace

bool ClusterCullPass::Initialize(IDevice& device, const cook::ClusterHierarchy& hierarchy, u32 capacity, std::string* error) {
    Release();
    const auto fail = [&](const char* message) {
        if (error) *error = message;
        Release();
        return false;
    };
    if (hierarchy.clusters.empty()) return fail("the hierarchy has no clusters");
    if (hierarchy.clusters.size() > 0x7FFFFFFFull / kClusterBytes) return fail("the hierarchy has too many clusters for one buffer");
    cluster_count_ = static_cast<u32>(hierarchy.clusters.size());
    capacity_ = capacity == 0 ? cluster_count_ : capacity;
    device_ = &device;
    try {
        std::vector<f32> table(static_cast<usize>(cluster_count_) * kClusterFloat4s * 4);
        for (u32 i = 0; i < cluster_count_; ++i) {
            const cook::Cluster& c = hierarchy.clusters[i];
            f32* t = &table[static_cast<usize>(i) * kClusterFloat4s * 4];
            const f32 cluster[kClusterFloat4s * 4] = {
                c.center.x, c.center.y, c.center.z, c.radius,
                c.parent_center.x, c.parent_center.y, c.parent_center.z, c.parent_radius,
                c.error, c.parent_error, Bits(c.triangle_count), 0.0f,
                c.bounds.center.x, c.bounds.center.y, c.bounds.center.z, c.bounds.radius,
                c.bounds.cone_axis.x, c.bounds.cone_axis.y, c.bounds.cone_axis.z, c.bounds.cone_cutoff};
            std::memcpy(t, cluster, sizeof(cluster));
        }
        clusters_ = device.CreateStorageBuffer(table.size() * sizeof(f32), 16, table.data());
        const u64 output_bytes = (static_cast<u64>(kOutputHeaderWords) + capacity_) * sizeof(u32);
        output_ = device.CreateStorageBuffer(output_bytes);
        constants_ = device.CreateConstantBuffer(sizeof(Constants));
        ComputePipelineDesc desc;
        desc.hlsl_source = kShader;
        desc.storage_buffer_count = 2;
        desc.constant_buffer_count = 1;
        pipeline_ = device.CreateComputePipeline(desc);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        Release();
        return false;
    }
    if (!clusters_.IsValid() || !output_.IsValid() || !constants_.IsValid() || !pipeline_.IsValid()) return fail("the device could not make the pass's resources");
    valid_ = true;
    return true;
}

void ClusterCullPass::Release() {
    if (device_) {
        if (clusters_.IsValid()) device_->DestroyBuffer(clusters_);
        if (output_.IsValid()) device_->DestroyBuffer(output_);
        if (constants_.IsValid()) device_->DestroyBuffer(constants_);
        if (pipeline_.IsValid()) device_->DestroyComputePipeline(pipeline_);
    }
    clusters_ = output_ = constants_ = {};
    pipeline_ = {};
    device_ = nullptr;
    cluster_count_ = capacity_ = 0;
    valid_ = false;
}

bool ClusterCullPass::Record(ICommandList& cmd, const cook::ClusterView& view, f32 threshold_pixels) {
    if (!valid_ || std::isnan(threshold_pixels) || !std::isfinite(view.position.x) || !std::isfinite(view.position.y) || !std::isfinite(view.position.z) ||
        !std::isfinite(view.vertical_fov) || !(view.vertical_fov > 0.0f) || !(view.vertical_fov < 3.1f) || !std::isfinite(view.viewport_height) ||
        !(view.viewport_height > 0.0f)) {
        return false;
    }
    Constants k{};
    const f32 pixels_per_unit = view.viewport_height * 0.5f / std::tan(view.vertical_fov * 0.5f);
    k.camera[0] = view.position.x;
    k.camera[1] = view.position.y;
    k.camera[2] = view.position.z;
    k.camera[3] = pixels_per_unit;
    k.params[0] = std::max(view.near_distance, 1e-6f);
    k.params[1] = std::max(threshold_pixels, 0.0f);
    k.params[2] = Bits(cluster_count_);
    k.params[3] = Bits(capacity_);
    k.flags[0] = Bits(view.has_frustum ? 1u : 0u);
    k.flags[1] = Bits(view.cull_backfaces ? 1u : 0u);
    for (u32 p = 0; p < 6; ++p) {
        k.planes[p][0] = view.frustum[p].normal.x;
        k.planes[p][1] = view.frustum[p].normal.y;
        k.planes[p][2] = view.frustum[p].normal.z;
        k.planes[p][3] = view.frustum[p].d;
    }
    device_->UpdateConstantBuffer(constants_, &k, sizeof(k));
    const std::vector<u32> zero(kOutputHeaderWords, 0u);
    device_->UpdateStorageBuffer(output_, zero.data(), zero.size() * sizeof(u32));
    cmd.BindComputePipeline(pipeline_);
    cmd.BindComputeStorageBuffer(0, clusters_);
    cmd.BindComputeStorageBuffer(1, output_);
    cmd.BindComputeConstantBuffer(0, constants_);
    cmd.Dispatch((cluster_count_ + kThreadGroup - 1) / kThreadGroup);
    cmd.HostReadBarrier(output_);
    return true;
}

bool ClusterCullPass::Read(GpuClusterSelection& out) const {
    out = GpuClusterSelection();
    if (!valid_ || !device_->SupportsStorageBufferRead()) return false;
    u32 header[kOutputHeaderWords] = {};
    if (!device_->ReadStorageBuffer(output_, header, sizeof(header))) return false;
    out.needed = header[0];
    out.triangles = header[1];
    out.overflow = header[0] > capacity_;
    const u32 kept = std::min(header[0], capacity_);
    out.clusters.resize(kept);
    if (kept > 0 && !device_->ReadStorageBuffer(output_, out.clusters.data(), static_cast<u64>(kept) * sizeof(u32), kOutputHeaderWords * sizeof(u32))) {
        out.clusters.clear();
        return false;
    }
    return true;
}

} // namespace aether::player
