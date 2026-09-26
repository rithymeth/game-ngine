#pragma once

#include "aether/gfx/command_list.h"
#include "aether/gfx/d3d12_common.h"
#include "aether/gfx/descriptor_heap.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace aether::gfx {

class Device;

// Which hardware queue a pass's work is recorded for. Graphics and Compute
// passes are recorded into separate command lists so Compute work can be
// submitted to Device::ComputeQueue() and run concurrently with Graphics
// work on hardware that supports it — see RenderGraph::Execute.
enum class QueueType { Graphics, Compute };

// A minimal frame graph: passes declare which resources they read/write and
// in what D3D12 resource state, and the graph inserts the transition
// barriers between passes automatically instead of every pass hand-rolling
// ResourceBarrier calls. This is the barrier-automation core the original
// Task Graph design builds on.
//
// Resources are either imported (externally owned — a swap chain backbuffer,
// say) or transient (allocated and owned by the graph itself — a depth
// buffer). Transient resources are NOT sub-allocated/aliased against a
// shared heap even when their lifetimes don't overlap; each gets its own
// committed allocation. True memory aliasing is future work.
//
// Passes can be tagged Graphics or Compute; Execute() records each into the
// matching command list the caller provides. True dependency-driven pass
// *reordering* is still not implemented — passes run in the order they were
// registered, within their queue. Cross-queue GPU synchronization (so a
// Graphics pass reading a Compute pass's output actually waits for it) and
// submission are the caller's job, via Device::GraphicsQueueWaitOnCompute /
// SubmitCompute — RenderGraph only records.
class RenderGraph {
public:
    using ResourceHandle = u32;
    static constexpr ResourceHandle kInvalidHandle = static_cast<ResourceHandle>(-1);

    explicit RenderGraph(Device& device);

    class PassBuilder {
    public:
        // Read and Write both just declare "this resource must be in `state`
        // during this pass" for barrier purposes; the distinction is kept for
        // readability and for future hazard validation (e.g. flagging two
        // passes that both declare a Write to the same resource with no
        // ordering dependency between them), not used by Execute() today.
        void Read(ResourceHandle resource, D3D12_RESOURCE_STATES state) { AddUsage(resource, state); }
        void Write(ResourceHandle resource, D3D12_RESOURCE_STATES state) { AddUsage(resource, state); }

    private:
        friend class RenderGraph;
        PassBuilder(RenderGraph& graph, usize pass_index) : graph_(graph), pass_index_(pass_index) {}
        void AddUsage(ResourceHandle resource, D3D12_RESOURCE_STATES state);

        RenderGraph& graph_;
        usize pass_index_;
    };

    using SetupFunc = std::function<void(PassBuilder&)>;
    using ExecuteFunc = std::function<void(ID3D12GraphicsCommandList*)>;

    // Registers (or looks up, by pointer identity) an externally-owned
    // resource. `initial_state` is only consulted the first time this
    // resource is seen; afterward the graph remembers whatever state the
    // last pass left it in, and that memory persists across Reset() calls —
    // which is what makes barriers correct for something like a swap chain
    // backbuffer that must start next frame already known to be PRESENT.
    ResourceHandle ImportResource(ID3D12Resource* resource, D3D12_RESOURCE_STATES initial_state,
                                   const char* name = "");

    // Allocates (or, called again for the same `name`, re-allocates — e.g.
    // after a window resize) a GPU resource the graph itself owns. The
    // returned handle is stable across a resize: passes referencing it keep
    // working, they just see the new underlying resource next frame.
    ResourceHandle CreateTransientTexture(const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES initial_state,
                                           const D3D12_CLEAR_VALUE* clear_value, const char* name);

    ID3D12Resource* GetResource(ResourceHandle handle) const { return resources_[handle].resource; }

    // Lazily creates (first call) or returns the cached RTV/DSV for a
    // transient resource's current underlying allocation.
    D3D12_CPU_DESCRIPTOR_HANDLE GetOrCreateRTV(ResourceHandle handle);
    D3D12_CPU_DESCRIPTOR_HANDLE GetOrCreateDSV(ResourceHandle handle);

    void AddPass(const char* name, SetupFunc setup, ExecuteFunc execute, QueueType queue = QueueType::Graphics);

    bool HasComputePasses() const;

    // Records every registered pass's barriers followed by its callback, in
    // registration order within its queue, into the matching command list.
    // `compute_cmd` may be null only if HasComputePasses() is false.
    void Execute(ID3D12GraphicsCommandList* graphics_cmd, ID3D12GraphicsCommandList* compute_cmd = nullptr);

    // Clears the pass list for the next frame. Tracked resource states are
    // NOT cleared — see ImportResource.
    void Reset() { passes_.clear(); }

private:
    struct Usage {
        ResourceHandle resource;
        D3D12_RESOURCE_STATES state;
    };
    struct Pass {
        std::string name;
        ExecuteFunc execute;
        std::vector<Usage> usages;
        QueueType queue;
    };
    struct TrackedResource {
        ComPtr<ID3D12Resource> owned_resource; // non-null only for transient resources
        ID3D12Resource* resource = nullptr;    // always valid: owned_resource.Get() or the imported pointer
        D3D12_RESOURCE_STATES current_state = D3D12_RESOURCE_STATE_COMMON;
        std::string name;
        u32 rtv_index = DescriptorHeap::kInvalidIndex;
        u32 dsv_index = DescriptorHeap::kInvalidIndex;
    };

    Device& device_;
    std::vector<TrackedResource> resources_;
    std::unordered_map<ID3D12Resource*, ResourceHandle> resource_lookup_;
    std::unordered_map<std::string, ResourceHandle> transient_lookup_; // keyed by name: the resource pointer changes on resize
    std::vector<Pass> passes_;

    // Created lazily, only if a pass ever asks for an RTV/DSV of a transient
    // resource — most graphs (e.g. ones only importing the swap chain's own
    // RTV-managed backbuffers) never need these.
    std::unique_ptr<DescriptorHeap> rtv_heap_;
    std::unique_ptr<DescriptorHeap> dsv_heap_;
};

} // namespace aether::gfx
