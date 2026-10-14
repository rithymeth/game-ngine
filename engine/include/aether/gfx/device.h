#pragma once

#include "aether/gfx/d3d12_common.h"

#include <functional>
#include <mutex>

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

    // Waits for both queues to drain, then runs an operation while new
    // submissions to either queue are blocked. Intended for CPU writes to
    // shader-visible descriptors that either queue may reference.
    void WithQueuesIdle(const std::function<void()>& operation);

    // Waits for compute work to drain and blocks new compute submissions
    // while the operation runs. The operation may submit ordered graphics
    // work, such as a copy that updates a compute-visible buffer.
    void WithComputeQueueIdle(const std::function<void()>& operation);

    // Waits for graphics work to drain and blocks new graphics submissions
    // while an operation runs. Use for CPU writes to graphics-only resources.
    void WithGraphicsQueueIdle(const std::function<void()>& operation);

    void WaitForFence(u64 fence_value);
    bool IsFenceComplete(u64 fence_value) const {
        if (fence_value == 0) return true;
        std::lock_guard<std::mutex> submit_lock(submit_mutex_);
        if (!fence_ || fence_value >= next_fence_value_) return false;
        const u64 completed = fence_->GetCompletedValue();
        // D3D12 reports UINT64_MAX when the device has been removed. It must
        // not be treated as an ordinary completed fence or callers may reset
        // allocators and reuse resources after the GPU has failed.
        return completed != ~u64{0} && completed >= fence_value;
    }

    // A second, independent queue (its own fence) for async compute: work
    // submitted here can execute on the GPU concurrently with the direct
    // queue's graphics work, on hardware that supports it. Cross-queue data
    // dependencies still need an explicit GPU-side wait (see
    // ComputeQueueWaitOnGraphics/GraphicsQueueWaitOnCompute below) — nothing
    // here is safe to assume ordered with direct-queue work by default.
    ID3D12CommandQueue* ComputeQueue() const { return compute_queue_.Get(); }

    u64 SubmitCompute(ID3D12CommandList* const* lists, u32 count);
    void WaitForComputeFence(u64 fence_value);
    bool IsComputeFenceComplete(u64 fence_value) const {
        if (fence_value == 0) return true;
        std::lock_guard<std::mutex> submit_lock(compute_submit_mutex_);
        if (!compute_fence_ || fence_value >= next_compute_fence_value_) return false;
        const u64 completed = compute_fence_->GetCompletedValue();
        return completed != ~u64{0} && completed >= fence_value;
    }

    // GPU-side (not CPU-blocking) cross-queue waits: makes one queue's
    // future work wait for the other queue to reach a given fence value
    // before it starts executing. This is the actual primitive async
    // compute overlap is built from — e.g. the graphics queue waiting on a
    // compute pass's output without the CPU ever blocking to synchronize
    // them.
    void ComputeQueueWaitOnGraphics(u64 graphics_fence_value);
    void GraphicsQueueWaitOnCompute(u64 compute_fence_value);

private:
    ComPtr<IDXGIFactory6> factory_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fence_event_ = nullptr;
    mutable std::mutex submit_mutex_;
    std::mutex fence_wait_mutex_;
    u64 next_fence_value_ = 1;

    ComPtr<ID3D12CommandQueue> compute_queue_;
    ComPtr<ID3D12Fence> compute_fence_;
    HANDLE compute_fence_event_ = nullptr;
    mutable std::mutex compute_submit_mutex_;
    std::mutex compute_fence_wait_mutex_;
    u64 next_compute_fence_value_ = 1;
};

} // namespace aether::gfx
