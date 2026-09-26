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
// ResourceBarrier calls.
//
// Resources are either imported (externally owned — a swap chain backbuffer,
// say) or transient (allocated and owned by the graph itself — a depth
// buffer). Distinct transient resources with non-overlapping lifetimes
// (the span of passes between their first and last use) are automatically
// aliased into shared GPU heap memory via D3D12 placed resources and
// aliasing barriers, instead of each getting its own committed allocation —
// see Execute()'s internal Compile() step. A transient resource used from
// both queues is conservatively excluded from aliasing (given its own
// dedicated allocation) rather than risk an unsafe cross-queue alias.
//
// Within each queue, passes execute in registration order. A genuine
// dependency-driven reordering pass was attempted here and deliberately
// removed: with only a resource handle + state per usage (no notion of a
// Write producing a new "version" a Read can selectively bind to), any
// topological sort over "last pass to touch this resource" edges is
// mathematically guaranteed to reproduce registration order exactly, for
// any input — see ComputeExecutionOrder's comment for the reasoning. Real
// reordering needs resource versioning, which is a distinctly larger
// feature and still future work. Cross-queue ordering (so a Graphics pass
// reading a Compute pass's output actually waits for it) is still the
// caller's job, via Device::GraphicsQueueWaitOnCompute/SubmitCompute —
// RenderGraph only records within a queue.
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
        // ordering dependency between them). Either one creates a same-queue
        // ordering edge against whichever other pass most recently used the
        // same resource, which is what Execute()'s topological sort uses.
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

    // Declares a GPU resource the graph itself owns (as opposed to
    // ImportResource's externally-owned ones). The returned handle is stable
    // across a resize: passes referencing it keep working across a later
    // call with the same `name` and a new `desc`. Actual GPU allocation is
    // deferred to Execute()'s internal Compile() step, once the full set of
    // this frame's passes (and therefore this resource's lifetime) is known
    // — that's what lets non-overlapping transients get aliased together.
    ResourceHandle CreateTransientTexture(const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES initial_state,
                                           const D3D12_CLEAR_VALUE* clear_value, const char* name);

    // Valid only after at least one Execute() call following this handle's
    // CreateTransientTexture/ImportResource — i.e. once the resource has
    // actually been allocated.
    ID3D12Resource* GetResource(ResourceHandle handle) const { return resources_[handle].resource; }

    // Lazily creates (first call) or returns the cached RTV/DSV for a
    // transient resource's current underlying allocation.
    D3D12_CPU_DESCRIPTOR_HANDLE GetOrCreateRTV(ResourceHandle handle);
    D3D12_CPU_DESCRIPTOR_HANDLE GetOrCreateDSV(ResourceHandle handle);

    // Testing/diagnostic accessor: true once both handles have been
    // allocated into the same underlying GPU heap (i.e. Compile() decided
    // their lifetimes don't overlap and aliased them). There's no public
    // D3D12 API to confirm two placed resources share physical memory from
    // the resource objects themselves (ID3D12Resource::GetGPUVirtualAddress
    // is only meaningful for buffers, not textures) — this checks our own
    // bucket-assignment bookkeeping instead, trusting CreatePlacedResource
    // to honor the heap+offset contract it documents.
    bool ShareHeapAllocation(ResourceHandle a, ResourceHandle b) const;

    void AddPass(const char* name, SetupFunc setup, ExecuteFunc execute, QueueType queue = QueueType::Graphics);

    bool HasComputePasses() const;

    // Compiles this frame's graph (dependency-orders passes per queue,
    // allocates/aliases any not-yet-allocated or resized transient
    // resources, and works out where aliasing barriers are needed) and
    // records every pass's barriers followed by its callback, in the
    // computed order within its queue, into the matching command list.
    // `compute_cmd` may be null only if HasComputePasses() is false.
    void Execute(ID3D12GraphicsCommandList* graphics_cmd, ID3D12GraphicsCommandList* compute_cmd = nullptr);

    // Clears the pass list for the next frame. Tracked resource states and
    // transient resource allocations are NOT cleared — see ImportResource
    // and CreateTransientTexture.
    void Reset() { passes_.clear(); }

private:
    struct Usage {
        ResourceHandle resource;
        D3D12_RESOURCE_STATES state;
    };
    struct AliasingBarrier {
        ID3D12Resource* before; // nullptr for "activating the first resource in a fresh heap" (no barrier needed, but kept for symmetry)
        ID3D12Resource* after;
    };
    struct Pass {
        std::string name;
        ExecuteFunc execute;
        std::vector<Usage> usages;
        QueueType queue;
        std::vector<AliasingBarrier> aliasing_barriers; // computed fresh by Compile() every frame
    };
    struct TrackedResource {
        ComPtr<ID3D12Resource> owned_resource; // non-null only for transient resources
        ID3D12Resource* resource = nullptr;    // valid once allocated: owned_resource.Get() or the imported pointer
        D3D12_RESOURCE_STATES current_state = D3D12_RESOURCE_STATE_COMMON;
        std::string name;
        u32 rtv_index = DescriptorHeap::kInvalidIndex;
        u32 dsv_index = DescriptorHeap::kInvalidIndex;

        // Transient-only bookkeeping (imported resources leave these unused):
        bool is_transient = false;
        bool needs_allocation = false; // set by CreateTransientTexture; cleared once Compile() allocates it
        D3D12_RESOURCE_DESC pending_desc{};
        D3D12_CLEAR_VALUE pending_clear{};
        bool has_clear_value = false;
        D3D12_RESOURCE_STATES pending_initial_state = D3D12_RESOURCE_STATE_COMMON;
    };
    struct TransientBucket {
        ComPtr<ID3D12Heap> heap;
        u64 size = 0;
        // Resources sharing this heap, in activation order (the order
        // Compile() determined they first become live) — consecutive
        // entries need an aliasing barrier between them at the later one's
        // first use.
        std::vector<ResourceHandle> resources_in_order;
    };

    std::vector<usize> ComputeExecutionOrder(QueueType queue) const;
    void Compile();
    void AllocateTransientResources();
    void AssignAliasingBarriers();
    void ExecuteQueue(const std::vector<usize>& order, ID3D12GraphicsCommandList* cmd);

    Device& device_;
    std::vector<TrackedResource> resources_;
    std::unordered_map<ID3D12Resource*, ResourceHandle> resource_lookup_;
    std::unordered_map<std::string, ResourceHandle> transient_lookup_; // keyed by name: the resource pointer changes on resize
    std::vector<Pass> passes_;
    std::vector<TransientBucket> transient_buckets_; // persists across Reset(); rebuilt only when a transient is (re)declared
    bool transients_dirty_ = true;

    // Created lazily, only if a pass ever asks for an RTV/DSV of a transient
    // resource — most graphs (e.g. ones only importing the swap chain's own
    // RTV-managed backbuffers) never need these.
    std::unique_ptr<DescriptorHeap> rtv_heap_;
    std::unique_ptr<DescriptorHeap> dsv_heap_;
};

} // namespace aether::gfx
