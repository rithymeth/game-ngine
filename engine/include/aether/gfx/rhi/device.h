#pragma once

#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/swap_chain.h"
#include "aether/gfx/rhi/types.h"

#include <memory>

namespace aether::gfx::rhi {

// Backend-agnostic device/queue/submission layer — the part of an RHI that
// genuinely can be made identical across D3D12 and Vulkan (create a device,
// create a swap chain, record a command list, submit it, wait on a fence
// value, present). Shader compilation, pipeline state, descriptor/resource
// binding and draw calls are NOT abstracted here — those differ enough
// between the two APIs (HLSL root signatures vs SPIR-V descriptor sets, PSOs
// vs pipeline objects) that unifying them is a distinctly larger, separate
// piece of work; code needing them uses ICommandList::NativeHandle() and
// writes backend-specific recording, selected by IDevice::GetBackend().
class IDevice {
public:
    virtual ~IDevice() = default;

    virtual std::unique_ptr<ISwapChain> CreateSwapChain(void* native_window_handle, u32 width, u32 height,
                                                         u32 buffer_count = 2) = 0;
    virtual std::unique_ptr<ICommandList> CreateCommandList() = 0;

    // Submits and returns the fence value that will be reached once the GPU
    // finishes executing `cmd`. Pass `wait_on_swap_chain` whenever `cmd`
    // renders into that swap chain's current backbuffer (i.e. whenever you
    // called its AcquireNextImage() this frame): on Vulkan this makes the
    // submission wait for the acquire to actually complete and arranges for
    // the swap chain's next Present() to wait for this submission — real
    // semaphore bookkeeping D3D12's flip-model swap chain doesn't need
    // (there it's simply ignored). Omit it for work that doesn't touch a
    // swap chain image (e.g. an off-screen compute pass).
    virtual u64 Submit(ICommandList& cmd, ISwapChain* wait_on_swap_chain = nullptr) = 0;
    virtual void WaitForFence(u64 fence_value) = 0;
    virtual bool IsFenceComplete(u64 fence_value) const = 0;

    // A second, independent queue for async compute: work submitted here can
    // execute on the GPU concurrently with the direct/graphics queue, on
    // hardware that supports it — the same capability gfx::Device already
    // has for the D3D12-only sandbox, now exposed at the swappable RHI
    // layer. Command lists submitted here must come from
    // CreateComputeCommandList(), not CreateCommandList() (D3D12 requires a
    // command list's recorded type to match the queue it's executed on;
    // Vulkan requires the command pool's queue family to match).
    virtual std::unique_ptr<ICommandList> CreateComputeCommandList() = 0;
    virtual u64 SubmitCompute(ICommandList& cmd) = 0;
    virtual void WaitForComputeFence(u64 fence_value) = 0;
    virtual bool IsComputeFenceComplete(u64 fence_value) const = 0;

    // GPU-side (not CPU-blocking) cross-queue waits, matching gfx::Device's
    // primitive of the same name: makes the NEXT submission on the waiting
    // queue wait for the other queue to reach a given fence value before it
    // starts executing. D3D12 has a true queue-level wait
    // (ID3D12CommandQueue::Wait) independent of any particular submission;
    // Vulkan has no equivalent primitive in core 1.2 — a wait semaphore can
    // only attach to an actual vkQueueSubmit — so the Vulkan backend queues
    // the wait and attaches it to whichever SubmitCompute/Submit call comes
    // next on that queue. Call this immediately before the submission it's
    // meant to gate.
    virtual void ComputeQueueWaitOnGraphics(u64 graphics_fence_value) = 0;
    virtual void GraphicsQueueWaitOnCompute(u64 compute_fence_value) = 0;

    virtual Backend GetBackend() const = 0;

    // Escape hatch matching ICommandList::NativeHandle(): ID3D12Device* or
    // VkDevice (as void*, via reinterpret_cast — VkDevice is a pointer
    // typedef), for backend-specific object creation (shaders, pipelines).
    virtual void* NativeHandle() const = 0;
};

// Factory. Returns nullptr (with a logged error) if `backend` isn't
// available — e.g. Vulkan requested on a build/machine where the Vulkan SDK
// wasn't found at configure time. Check IsBackendAvailable() first if you
// need to choose a fallback.
std::unique_ptr<IDevice> CreateDevice(Backend backend, bool enable_debug_layer = true);

bool IsBackendAvailable(Backend backend);

} // namespace aether::gfx::rhi
