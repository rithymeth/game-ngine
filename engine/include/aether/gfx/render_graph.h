#pragma once

#include "aether/gfx/command_list.h"
#include "aether/gfx/d3d12_common.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aether::gfx {

// A minimal frame graph: passes declare which resources they read/write and
// in what D3D12 resource state, and the graph inserts the transition
// barriers between passes automatically instead of every pass hand-rolling
// ResourceBarrier calls. This is the barrier-automation core the original
// Task Graph design builds on — true dependency-driven pass reordering,
// transient resource allocation/aliasing, and multi-queue/async-compute
// sequencing are not implemented yet; passes run in registration order and
// every resource is caller-owned ("imported"), not allocated by the graph.
class RenderGraph {
public:
    using ResourceHandle = u32;
    static constexpr ResourceHandle kInvalidHandle = static_cast<ResourceHandle>(-1);

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

    void AddPass(const char* name, SetupFunc setup, ExecuteFunc execute);

    // Records every pass's barriers followed by its callback, in
    // registration order, into `cmd`.
    void Execute(ID3D12GraphicsCommandList* cmd);

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
    };
    struct TrackedResource {
        ID3D12Resource* resource;
        D3D12_RESOURCE_STATES current_state;
        std::string name;
    };

    std::vector<TrackedResource> resources_;
    std::unordered_map<ID3D12Resource*, ResourceHandle> resource_lookup_;
    std::vector<Pass> passes_;
};

} // namespace aether::gfx
