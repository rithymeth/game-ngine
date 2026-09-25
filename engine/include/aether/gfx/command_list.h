#pragma once

#include "aether/gfx/d3d12_common.h"

namespace aether::gfx {

class Device;

// Owns one command allocator + one graphics command list. Deliberately not a
// wrapper over every D3D12 recording call — Get()/operator-> hands out the
// raw ID3D12GraphicsCommandList* so callers record with the native API
// directly. What this class does own is the lifecycle: Reset() before
// recording a frame (the caller must have already confirmed the GPU is done
// with this allocator's prior contents via Device::WaitForFence) and Close()
// before submission.
class CommandList {
public:
    explicit CommandList(Device& device, D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT);

    void Reset(ID3D12PipelineState* initial_state = nullptr);
    void Close();

    ID3D12GraphicsCommandList* Get() const { return list_.Get(); }
    ID3D12GraphicsCommandList* operator->() const { return list_.Get(); }

private:
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> list_;
};

// Common D3D12_RESOURCE_BARRIER boilerplate for a single resource transition.
inline D3D12_RESOURCE_BARRIER TransitionBarrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                                 D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

} // namespace aether::gfx
