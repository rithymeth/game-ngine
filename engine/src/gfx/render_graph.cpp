#include "aether/gfx/render_graph.h"

namespace aether::gfx {

void RenderGraph::PassBuilder::AddUsage(ResourceHandle resource, D3D12_RESOURCE_STATES state) {
    graph_.passes_[pass_index_].usages.push_back({resource, state});
}

RenderGraph::ResourceHandle RenderGraph::ImportResource(ID3D12Resource* resource, D3D12_RESOURCE_STATES initial_state,
                                                         const char* name) {
    auto it = resource_lookup_.find(resource);
    if (it != resource_lookup_.end()) {
        return it->second;
    }
    ResourceHandle handle = static_cast<ResourceHandle>(resources_.size());
    resources_.push_back(TrackedResource{resource, initial_state, name});
    resource_lookup_.emplace(resource, handle);
    return handle;
}

void RenderGraph::AddPass(const char* name, SetupFunc setup, ExecuteFunc execute) {
    usize index = passes_.size();
    passes_.push_back(Pass{name, std::move(execute), {}});
    PassBuilder builder(*this, index);
    setup(builder);
}

void RenderGraph::Execute(ID3D12GraphicsCommandList* cmd) {
    for (Pass& pass : passes_) {
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        for (const Usage& usage : pass.usages) {
            TrackedResource& tracked = resources_[usage.resource];
            if (tracked.current_state != usage.state) {
                barriers.push_back(TransitionBarrier(tracked.resource, tracked.current_state, usage.state));
                tracked.current_state = usage.state;
            }
        }
        if (!barriers.empty()) {
            cmd->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        }
        pass.execute(cmd);
    }
}

} // namespace aether::gfx
