#pragma once

#include "aether/cook/cluster_hierarchy.h"
#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"

#include <string>
#include <vector>

namespace aether::player {

// What one cull gave: the clusters drawn (ids into the hierarchy, in no particular order: threads finish in any order), and their triangle count.
struct GpuClusterSelection {
    std::vector<u32> clusters;
    u64 triangles = 0;
    bool overflow = false; // more clusters were selected than the output has room for: `clusters` holds the first that fit, `needed` says how many
    u32 needed = 0;
};

// Stage 2 of virtual geometry (docs/design/VIRTUAL_GEOMETRY.md): the cluster cut and the culling on the GPU. One compute thread per cluster runs the same
// test as cook::SelectClusters (its own error projects to at most the pixel threshold, its parent's to more) and the frustum and normal-cone tests, and
// the survivors are appended to a list with an atomic counter. The CPU selector is its reference: a test runs both and compares. Occlusion (Hi-Z) culling
// and the indirect draw come later.
//
// The cluster table is uploaded once. Each cull writes its constants and zeroes its counters from the CPU (so the previous cull's submission must have
// finished), records a dispatch on a COMPUTE command list, and ends with a barrier that makes the list readable. Read() reads it back on the CPU, which
// needs IDevice::SupportsStorageBufferRead (Vulkan); the indirect draw of stage 3 will read the list on the GPU instead.
class ClusterCullPass {
public:
    static constexpr u32 kThreadGroup = 64;

    ClusterCullPass() = default;
    ~ClusterCullPass() { Release(); }
    ClusterCullPass(const ClusterCullPass&) = delete;
    ClusterCullPass& operator=(const ClusterCullPass&) = delete;

    // False (with a reason) for a device without compute, an empty hierarchy, or a capacity that is zero. `capacity` is how many selected clusters
    // the output holds; 0 means room for every cluster of the hierarchy.
    bool Initialize(gfx::rhi::IDevice& device, const cook::ClusterHierarchy& hierarchy, u32 capacity = 0, std::string* error = nullptr);
    void Release();
    bool Valid() const { return valid_; }
    u32 ClusterCount() const { return cluster_count_; }
    u32 Capacity() const { return capacity_; }

    // Writes the constants for `view` and `threshold_pixels`, zeroes the counters and records the dispatch and the host barrier on `compute_cmd`
    // (a list from CreateComputeCommandList, already Reset). Call it only when the last submission that used this pass has finished. False for a view
    // or threshold that is not a number, or a list that is not recording.
    bool Record(gfx::rhi::ICommandList& compute_cmd, const cook::ClusterView& view, f32 threshold_pixels);

    // After the submission that carried Record has finished: the selection. False when the device cannot read storage buffers.
    bool Read(GpuClusterSelection& out) const;

private:
    gfx::rhi::IDevice* device_ = nullptr;
    gfx::rhi::ComputePipelineHandle pipeline_;
    gfx::rhi::BufferHandle clusters_, output_, constants_;
    u32 cluster_count_ = 0;
    u32 capacity_ = 0;
    bool valid_ = false;
};

} // namespace aether::player
