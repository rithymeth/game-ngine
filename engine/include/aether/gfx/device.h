#pragma once

#include "aether/gfx/d3d12_common.h"

namespace aether::gfx {

// Owns the D3D12 device, a single direct command queue, and a manually
// advanced fence. There's no per-draw hidden state and no automatic
// synchronization: callers record command lists and Submit() them
// explicitly, and GPU/CPU sync is a u64 fence value the caller compares
// against IsFenceComplete()/waits on with WaitForFence() — matching the
// "explicit GPU control" pillar rather than papering over it with a
// convenience API.
class Device {
public:
    explicit Device(bool enable_debug_layer = true);
    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    ID3D12Device* Handle() const { return device_.Get(); }
    ID3D12CommandQueue* Queue() const { return queue_.Get(); }
    IDXGIFactory6* Factory() const { return factory_.Get(); }

    // Submits command lists to the direct queue and returns the fence value
    // that will be reached once the GPU finishes executing them.
    u64 Submit(ID3D12CommandList* const* lists, u32 count);

    void WaitForFence(u64 fence_value);
    bool IsFenceComplete(u64 fence_value) const { return fence_->GetCompletedValue() >= fence_value; }

private:
    ComPtr<IDXGIFactory6> factory_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fence_event_ = nullptr;
    u64 next_fence_value_ = 1;
};

} // namespace aether::gfx
