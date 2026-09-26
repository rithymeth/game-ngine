#pragma once

#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/swap_chain.h"
#include "aether/gfx/rhi/types.h"

#include <memory>
#include <string>

namespace aether::gfx::rhi {

// Describes a pipeline for IDevice::CreatePipeline: one HLSL source compiled
// two ways internally (D3DCompile/DXIL for D3D12, DXC/SPIR-V for Vulkan —
// the same dual-compile CompileHLSL/CompileHLSLToSPIRV split rhi_demo's
// triangle mode already used, just moved behind the RHI instead of
// duplicated in demo code), one push-constant block, procedural vertices
// (no vertex buffer — see ICommandList::Draw). This is deliberately the
// minimal shape that made rhi_demo's triangle mode's two backend-specific
// pipeline-creation functions unifiable into one; a real material/mesh
// system needs considerably more (vertex layouts, descriptor/texture
// binding, blend states, ...) — see the RHI's README section for what's
// still open.
struct PipelineDesc {
    std::string hlsl_source;
    std::string vs_entry = "VSMain";
    std::string ps_entry = "PSMain";
    u32 push_constant_size_bytes = 0;
    bool cull_back_face = false;
};

// Backend-agnostic device/queue/submission layer, extended with a genuinely
// unified pipeline-creation + draw-call path (see PipelineDesc and
// ICommandList's BeginRenderPass/BindPipeline/SetPushConstants/Draw) for the
// narrow but real slice of "shaders/pipelines/draw calls" that a single
// procedural-vertex, single-push-constant-block pipeline covers. A full
// material/mesh system (vertex buffers, descriptor/texture binding, blend
// states, ...) is a distinctly larger, separate piece of work still done via
// ICommandList::NativeHandle() and backend-specific recording (see
// rhi_demo's cube mode) — GetBackend() is there for exactly that case.
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

    // Compiles `desc.hlsl_source` for this device's backend and builds
    // whatever backend-specific pipeline object that requires (D3D12 root
    // signature + PSO; Vulkan pipeline layout + a render-pass-compatible
    // graphics pipeline targeting `swap_chain`'s format). `swap_chain` is
    // only consulted for its color format/backend at creation time — the
    // returned handle isn't tied to that particular swap chain instance
    // beyond that.
    virtual PipelineHandle CreatePipeline(const PipelineDesc& desc, ISwapChain& swap_chain) = 0;

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
