#include "aether/gfx/render_graph.h"

#include "aether/gfx/device.h"

namespace aether::gfx {

namespace {
constexpr u32 kRtvHeapCapacity = 16;
constexpr u32 kDsvHeapCapacity = 16;
} // namespace

RenderGraph::RenderGraph(Device& device) : device_(device) {}

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
    TrackedResource tracked;
    tracked.resource = resource;
    tracked.current_state = initial_state;
    tracked.name = name;
    resources_.push_back(std::move(tracked));
    resource_lookup_.emplace(resource, handle);
    return handle;
}

RenderGraph::ResourceHandle RenderGraph::CreateTransientTexture(const D3D12_RESOURCE_DESC& desc,
                                                                 D3D12_RESOURCE_STATES initial_state,
                                                                 const D3D12_CLEAR_VALUE* clear_value,
                                                                 const char* name) {
    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

    ComPtr<ID3D12Resource> resource;
    AETHER_D3D_CHECK(device_.Handle()->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc,
                                                                initial_state, clear_value, IID_PPV_ARGS(&resource)));

    auto it = transient_lookup_.find(name);
    if (it != transient_lookup_.end()) {
        // Re-created (e.g. after a resize): drop the old allocation and any
        // views into it, keep the same handle so existing pass registrations
        // referencing it stay valid.
        TrackedResource& tracked = resources_[it->second];
        if (tracked.rtv_index != DescriptorHeap::kInvalidIndex) {
            rtv_heap_->Free(tracked.rtv_index);
            tracked.rtv_index = DescriptorHeap::kInvalidIndex;
        }
        if (tracked.dsv_index != DescriptorHeap::kInvalidIndex) {
            dsv_heap_->Free(tracked.dsv_index);
            tracked.dsv_index = DescriptorHeap::kInvalidIndex;
        }
        tracked.owned_resource = resource;
        tracked.resource = resource.Get();
        tracked.current_state = initial_state;
        resource_lookup_.erase(tracked.resource); // stale entries from the old pointer, if any
        resource_lookup_.emplace(tracked.resource, it->second);
        return it->second;
    }

    ResourceHandle handle = static_cast<ResourceHandle>(resources_.size());
    TrackedResource tracked;
    tracked.owned_resource = resource;
    tracked.resource = resource.Get();
    tracked.current_state = initial_state;
    tracked.name = name;
    resources_.push_back(std::move(tracked));
    resource_lookup_.emplace(resources_.back().resource, handle);
    transient_lookup_.emplace(name, handle);
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE RenderGraph::GetOrCreateRTV(ResourceHandle handle) {
    TrackedResource& tracked = resources_[handle];
    if (tracked.rtv_index == DescriptorHeap::kInvalidIndex) {
        if (!rtv_heap_) {
            rtv_heap_ = std::make_unique<DescriptorHeap>(device_, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvHeapCapacity,
                                                          /*shader_visible=*/false);
        }
        tracked.rtv_index = rtv_heap_->Allocate();
        device_.Handle()->CreateRenderTargetView(tracked.resource, nullptr, rtv_heap_->CPUHandle(tracked.rtv_index));
    }
    return rtv_heap_->CPUHandle(tracked.rtv_index);
}

D3D12_CPU_DESCRIPTOR_HANDLE RenderGraph::GetOrCreateDSV(ResourceHandle handle) {
    TrackedResource& tracked = resources_[handle];
    if (tracked.dsv_index == DescriptorHeap::kInvalidIndex) {
        if (!dsv_heap_) {
            dsv_heap_ = std::make_unique<DescriptorHeap>(device_, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvHeapCapacity,
                                                          /*shader_visible=*/false);
        }
        tracked.dsv_index = dsv_heap_->Allocate();
        device_.Handle()->CreateDepthStencilView(tracked.resource, nullptr, dsv_heap_->CPUHandle(tracked.dsv_index));
    }
    return dsv_heap_->CPUHandle(tracked.dsv_index);
}

void RenderGraph::AddPass(const char* name, SetupFunc setup, ExecuteFunc execute, QueueType queue) {
    usize index = passes_.size();
    passes_.push_back(Pass{name, std::move(execute), {}, queue});
    PassBuilder builder(*this, index);
    setup(builder);
}

bool RenderGraph::HasComputePasses() const {
    for (const Pass& pass : passes_) {
        if (pass.queue == QueueType::Compute) {
            return true;
        }
    }
    return false;
}

void RenderGraph::Execute(ID3D12GraphicsCommandList* graphics_cmd, ID3D12GraphicsCommandList* compute_cmd) {
    for (Pass& pass : passes_) {
        ID3D12GraphicsCommandList* cmd = (pass.queue == QueueType::Compute) ? compute_cmd : graphics_cmd;
        AETHER_ASSERT(cmd != nullptr);

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
