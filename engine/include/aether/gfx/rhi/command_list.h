#pragma once

#include "aether/gfx/rhi/types.h"

namespace aether::gfx::rhi {

class ISwapChain;

// Backend-agnostic command recording. Two tiers:
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
class ICommandList {
public:
    virtual ~ICommandList() = default;

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
    virtual void EndRenderPass() = 0;

    // Binds a pipeline created via IDevice::CreatePipeline. Must be called
    // inside a BeginRenderPass/EndRenderPass pair.
    virtual void BindPipeline(PipelineHandle pipeline) = 0;

    // Uploads `size_bytes` of push-constant data (D3D12 root 32-bit
    // constants / Vulkan push constants) — must not exceed the
    // `push_constant_size` the currently-bound pipeline was created with.
    virtual void SetPushConstants(const void* data, u32 size_bytes) = 0;

    // Draws `vertex_count` procedural vertices (SV_VertexID/gl_VertexIndex
    // driven, no vertex buffer) as a triangle list.
    virtual void Draw(u32 vertex_count) = 0;

    // Real vertex/index buffers + bindless textures (the "Unified Cross-API
    // Renderer" follow-up) — must be called after BindPipeline, with a
    // pipeline created with use_vertex_buffer/enable_bindless_textures set
    // accordingly. `stride_bytes` must match the fixed { float3 position;
    // float2 uv; } layout PipelineDesc::use_vertex_buffer bakes into the
    // pipeline (both backends assert this rather than silently misreading
    // vertex data at the wrong stride).
    virtual void BindVertexBuffer(BufferHandle buffer, u32 stride_bytes) = 0;
    virtual void BindIndexBuffer(BufferHandle buffer, IndexFormat format) = 0;
    virtual void DrawIndexed(u32 index_count) = 0;

    // Binds the device-global bindless texture table (IDevice::CreateTexture)
    // for the currently-bound pipeline (must have been created with
    // enable_bindless_textures = true). One call per draw sequence is enough
    // — the whole table is bound at once, same as gfx::DescriptorHeap's
    // bindless convention elsewhere in this engine; which texture a given
    // draw actually samples is selected shader-side by an index carried in
    // SetPushConstants' data, not by anything this call takes.
    virtual void BindBindlessTextures() = 0;

    // Escape hatch: the backend-native command list/buffer pointer
    // (ID3D12GraphicsCommandList* for D3D12, VkCommandBuffer for Vulkan,
    // reinterpret_cast from a uintptr_t on the Vulkan side since VkCommandBuffer
    // is itself just a pointer typedef) for recording anything this
    // interface doesn't cover yet.
    virtual void* NativeHandle() const = 0;
};

} // namespace aether::gfx::rhi
