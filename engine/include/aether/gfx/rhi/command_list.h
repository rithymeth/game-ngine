#pragma once

#include "aether/gfx/rhi/types.h"

#include <atomic>

namespace aether::gfx::rhi {

class ISwapChain;

// Backend-agnostic command recording. Three tiers:
//
//  - Reset/Close/TransitionTexture/ClearRenderTarget/SetViewportAndScissor:
//    the original swapchain present/clear lifecycle — clear-to-color, no
//    shaders.
//  - BeginRenderPass/EndRenderPass/BindPipeline/SetPushConstants/Draw: a
//    genuinely unified draw-call path (the RHI's "Unified Cross-API
//    Renderer" follow-up) — the SAME calls, in the SAME order, render on
//    both D3D12 and Vulkan with zero backend branching in the caller. This
//    covers exactly what rhi_demo's triangle/cube modes previously needed
//    two separate backend-specific code paths for (D3D12 root
//    signature/PSO/OMSetRenderTargets vs. Vulkan pipeline layout/render
//    pass/framebuffer) — see IDevice::CreatePipeline for what's hidden
//    behind PipelineHandle. Still deliberately narrow: procedural vertices
//    only (no vertex/index buffer binding), one push-constant block, no
//    textures/descriptors — see the RHI's README section for what's still
//    backend-specific (rhi_demo's cube mode, with real vertex buffers and a
//    depth buffer, still uses each backend's NativeHandle() escape hatch
//    directly).
//  - BindComputePipeline/BindComputeStorageBuffer/BindComputeSampledTexture/
//    BindComputeConstantBuffer/Dispatch: structured/raw-buffer compute
//    kernels with sampled-texture reads shared by D3D12 and Vulkan.
class ICommandList {
public:
    virtual ~ICommandList() = default;

    // Draws recorded through Draw, DrawIndexed and the indirect forms on any list since ResetDrawCallCount, for a performance HUD
    // (the editor's viewport stats). Recording is single-threaded where a caller resets and reads around its own frame; the counter
    // is atomic only so a reset cannot race a stray recording elsewhere.
    static u64 DrawCallCount() { return s_draw_calls_.load(std::memory_order_relaxed); }
    static void ResetDrawCallCount() { s_draw_calls_.store(0, std::memory_order_relaxed); }
    // Triangles in the draws recorded since ResetTriangleCount: vertex_count / 3 for Draw and index_count / 3 for DrawIndexed
    // (both record triangle lists). Indirect draws count as zero -- their counts live on the GPU -- so a scene drawn through
    // an indirect path shows fewer triangles than it drew, and a triangle list is assumed for the others.
    static u64 TriangleCount() { return s_triangles_.load(std::memory_order_relaxed); }
    static void ResetTriangleCount() { s_triangles_.store(0, std::memory_order_relaxed); }

    virtual void Reset() = 0;
    virtual void Close() = 0;

    virtual void TransitionTexture(TextureHandle texture, ResourceState before, ResourceState after) = 0;
    virtual void ClearRenderTarget(TextureHandle texture, const ClearColor& color) = 0;
    virtual void SetViewportAndScissor(const Viewport& viewport, const Rect& scissor) = 0;

    // Begins drawing into `swap_chain`'s current backbuffer, clearing it to
    // `clear_color` first (loadOp=CLEAR on Vulkan, ClearRenderTargetView on
    // D3D12) — this hides the render-pass/framebuffer vs.
    // OMSetRenderTargets difference entirely, including the backbuffer's
    // PRESENT<->RENDER_TARGET/COLOR_ATTACHMENT_OPTIMAL transitions (tracked
    // internally per swapchain image, not by the caller). Must be paired
    // with EndRenderPass() before Close().
    virtual void BeginRenderPass(ISwapChain& swap_chain, const ClearColor& clear_color) = 0;
    // As BeginRenderPass, but into `swap_chain`'s SceneColorTexture (which must already exist) with the swap chain's depth, both cleared.
    // Only pipelines made with PipelineDesc::scene_color_target draw in it; CopySceneDepth works as in a back-buffer pass. EndRenderPass
    // leaves the target shader-readable for the pass that follows. False (nothing recorded) without a SceneColorTexture, inside another
    // pass, and on devices without support (the default).
    virtual bool BeginSceneColorPass(ISwapChain& /*swap_chain*/, const ClearColor& /*clear_color*/) { return false; }
    // Begins drawing into a render texture (IDevice::CreateRenderTexture), with no depth attachment, the viewport and scissor its full
    // size. `clear_color` null keeps its contents (for additive passes over what an earlier pass drew), else clears to it. Only
    // pipelines made with PipelineDesc::render_texture_target draw in it; CopySceneDepth is refused. EndRenderPass leaves it
    // shader-readable. False (nothing recorded) for a handle that isn't a live render texture of this device, inside another pass, and
    // on devices without support (the default).
    virtual bool BeginTexturePass(SampledTextureHandle /*target*/, const ClearColor* /*clear_color*/) { return false; }
    // Begins drawing depth only into a depth texture (IDevice::CreateDepthTexture), cleared to 1 (far) when `clear`, the viewport and
    // scissor its full size (SetViewportAndScissor narrows them, e.g. to one cascade of a shadow atlas). Only pipelines made with
    // PipelineDesc::depth_target draw in it. EndRenderPass leaves it shader-readable. False (nothing recorded) for a handle that isn't a
    // live depth texture of this device, inside another pass, and on devices without support (the default).
    virtual bool BeginDepthPass(SampledTextureHandle /*target*/, bool /*clear*/) { return false; }
    virtual void EndRenderPass() = 0;

    // Binds a pipeline created via IDevice::CreatePipeline. Must be called
    // inside a BeginRenderPass/EndRenderPass pair.
    virtual void BindPipeline(PipelineHandle pipeline) = 0;

    // Uploads `size_bytes` of push-constant data (D3D12 root 32-bit
    // constants / Vulkan push constants) — must not exceed the
    // `push_constant_size` the currently-bound pipeline was created with.
    virtual void SetPushConstants(const void* data, u32 size_bytes) = 0;

    // Draws `vertex_count` procedural vertices (SV_VertexID/gl_VertexIndex
    // driven, no vertex buffer) as a triangle list. With a bound vertex
    // buffer, `first_vertex` starts the draw that many vertices in, so one
    // buffer can feed several draws (e.g. one per texture, keeping a bindless
    // index uniform across each draw); first_vertex + vertex_count must fit.
    virtual void Draw(u32 vertex_count, u32 first_vertex = 0) = 0;

    // Real vertex/index buffers + bindless textures (the "Unified Cross-API
    // Renderer" follow-up) — must be called after BindPipeline, with a
    // pipeline created with use_vertex_buffer/enable_bindless_textures set
    // accordingly. `stride_bytes` must match the selected
    // PipelineDesc::vertex_layout baked into the pipeline (both backends
    // reject a mismatch rather than silently misreading vertex data).
    virtual void BindVertexBuffer(BufferHandle buffer, u32 stride_bytes) = 0;
    virtual void BindIndexBuffer(BufferHandle buffer, IndexFormat format) = 0;
    virtual void DrawIndexed(u32 index_count) = 0;
    // Like DrawIndexed, but the draw's arguments are read by the GPU from a storage buffer (IDevice::CreateStorageBuffer) at submission time: five
    // u32s, {index_count, instance_count, first_index, vertex_offset (as i32), first_instance}, at byte `offset` (4-byte aligned, the five words
    // must fit). This is how a compute pass (cluster culling) drives a draw without the CPU reading its result. Needs a bound pipeline, vertex buffer
    // and index buffer as DrawIndexed does, and IDevice::SupportsIndirectDraw. A compute list that wrote the buffer must be ordered before this
    // graphics list by a semaphore or a fence (the buffer is shared between both queues). False, nothing recorded, otherwise (the default).
    virtual bool DrawIndexedIndirect(BufferHandle /*args*/, u64 /*offset*/ = 0) { return false; }
    // Like DrawIndexedIndirect for up to `max_draws` draws: the arguments are consecutive five-word records from `args` at `args_offset`, and the number
    // actually drawn is the u32 at `count_offset` of `count` (a storage buffer, usually written by the same compute pass), clamped to `max_draws`. A shader
    // that selects objects appends their records and bumps the count; nothing is read back. Needs IDevice::SupportsIndirectCount; false otherwise.
    virtual bool DrawIndexedIndirectCount(BufferHandle /*args*/, BufferHandle /*count*/, u32 /*max_draws*/, u64 /*args_offset*/ = 0, u64 /*count_offset*/ = 0) {
        return false;
    }

    // Compute dispatch. BindComputePipeline must precede Dispatch; dimensions
    // are workgroup counts and must be non-zero.
    virtual void BindComputePipeline(ComputePipelineHandle pipeline) = 0;
    virtual void BindComputeStorageBuffer(u32 slot, BufferHandle buffer) = 0;
    virtual void BindComputeSampledTexture(u32 slot, SampledTextureHandle texture) = 0;
    virtual void BindComputeConstantBuffer(u32 slot, BufferHandle buffer) = 0;
    virtual void ComputeBufferBarrier(BufferHandle buffer) = 0;
    // Makes what the compute shaders wrote to `buffer` (a storage buffer) visible to the CPU once the submission carrying this has finished, so
    // IDevice::ReadStorageBuffer can read it. Record it after the last dispatch that writes the buffer, on a compute list. Does nothing on devices
    // without IDevice::SupportsStorageBufferRead (the default).
    virtual void HostReadBarrier(BufferHandle /*buffer*/) {}
    virtual void Dispatch(u32 groups_x, u32 groups_y = 1, u32 groups_z = 1) = 0;

    // Binds the device-global bindless texture table (IDevice::CreateTexture)
    // for the currently-bound pipeline (must have been created with
    // enable_bindless_textures = true). One call per draw sequence is enough
    // — the whole table is bound at once, same as gfx::DescriptorHeap's
    // bindless convention elsewhere in this engine; which texture a given
    // draw actually samples is selected shader-side by an index carried in
    // SetPushConstants' data, not by anything this call takes.
    virtual void BindBindlessTextures() = 0;

    // Binds a constant buffer (IDevice::CreateConstantBuffer) as the bound
    // pipeline's frame constants (PipelineDesc::frame_constants_size_bytes > 0).
    // Call after BindPipeline; the binding holds for the draws that follow,
    // until the next BindPipeline. A no-op on devices without support.
    virtual void BindFrameConstants(BufferHandle /*buffer*/) {}
    // Binds a buffer made by IDevice::CreateFrameConstantBuffer as the bound pipeline's frame read buffer (PipelineDesc::
    // frame_read_buffer). Call after BindPipeline; the binding holds until the next BindPipeline. A no-op on devices without support.
    virtual void BindFrameReadBuffer(BufferHandle /*buffer*/) {}
    // Binds a top-level structure (IDevice::CreateTopLevelAS) as the bound pipeline's RaytracingAccelerationStructure (PipelineDesc::
    // acceleration_structure). Call after BindPipeline; the binding holds until the next BindPipeline. A no-op on devices without support.
    virtual void BindAccelerationStructure(AccelerationStructureHandle /*structure*/) {}

    // GPU timing (IDevice::CreateGpuTimer): writes slot `index` of the timer with the time the GPU reaches this point (when everything before
    // has finished), inside or outside a pass. ResolveGpuTimer, once at the end of the recording and outside any pass, makes the slots readable
    // after the submission's fence. Graphics lists only; false (nothing recorded) for a dead timer, a slot out of range, a compute list or a
    // device without timing.
    virtual bool WriteGpuTimestamp(GpuTimerHandle /*timer*/, u32 /*index*/) { return false; }
    virtual bool ResolveGpuTimer(GpuTimerHandle /*timer*/) { return false; }

    // Inside a BeginRenderPass pass: copies the depth drawn so far into the swap chain's SceneDepthTexture (which must already exist) and
    // continues the same pass, colour and depth kept (Vulkan ends the render pass, copies and begins a loading one; D3D12 copies between
    // barriers). The bound pipeline and its bindings are gone afterwards and the viewport and scissor are the full swap chain again, as after
    // BeginRenderPass: rebind before drawing. Draws after it may read the texture; they must not write depth they then expect to read. False
    // (nothing recorded, the pass untouched) without an active pass or a SceneDepthTexture, and on devices without support (the default).
    virtual bool CopySceneDepth() { return false; }
    // Outside any pass: copies a render texture (IDevice::CreateRenderTexture, RGBA16F: 8 bytes a texel) into a buffer made by
    // IDevice::CreateReadbackBuffer, rows ReadbackRowPitch(width, 8) apart, which the buffer must have room for (height rows). The texture
    // stays shader-readable. False, nothing recorded, for any other texture or buffer, inside a pass, or on devices without readback.
    virtual bool CopyRenderTextureToReadback(SampledTextureHandle /*texture*/, BufferHandle /*readback*/) { return false; }

    // Escape hatch: the backend-native command list/buffer pointer
    // (ID3D12GraphicsCommandList* for D3D12, VkCommandBuffer for Vulkan,
    // reinterpret_cast from a uintptr_t on the Vulkan side since VkCommandBuffer
    // is itself just a pointer typedef) for recording anything this
    // interface doesn't cover yet.
    virtual void* NativeHandle() const = 0;

protected:
    // What the backends call when they record a draw, so DrawCallCount and TriangleCount count it.
    static void NoteDraw(u64 triangles = 0) {
        s_draw_calls_.fetch_add(1, std::memory_order_relaxed);
        if (triangles != 0) s_triangles_.fetch_add(triangles, std::memory_order_relaxed);
    }

private:
    inline static std::atomic<u64> s_draw_calls_{0};
    inline static std::atomic<u64> s_triangles_{0};
};

} // namespace aether::gfx::rhi
