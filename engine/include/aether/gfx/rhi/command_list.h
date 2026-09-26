#pragma once

#include "aether/gfx/rhi/types.h"

namespace aether::gfx::rhi {

// Backend-agnostic command recording — deliberately minimal (see types.h):
// covers the swapchain present/clear lifecycle, not drawing. A backend's
// concrete command list (D3D12CommandList, VulkanCommandList) additionally
// exposes its native handle (ID3D12GraphicsCommandList*/VkCommandBuffer) for
// backend-specific recording (shaders, pipelines, draw calls) that hasn't
// been abstracted — same escape-hatch shape as IDevice::NativeHandle.
class ICommandList {
public:
    virtual ~ICommandList() = default;

    virtual void Reset() = 0;
    virtual void Close() = 0;

    virtual void TransitionTexture(TextureHandle texture, ResourceState before, ResourceState after) = 0;
    virtual void ClearRenderTarget(TextureHandle texture, const ClearColor& color) = 0;
    virtual void SetViewportAndScissor(const Viewport& viewport, const Rect& scissor) = 0;

    // Escape hatch: the backend-native command list/buffer pointer
    // (ID3D12GraphicsCommandList* for D3D12, VkCommandBuffer for Vulkan,
    // reinterpret_cast from a uintptr_t on the Vulkan side since VkCommandBuffer
    // is itself just a pointer typedef) for recording anything this
    // interface doesn't cover yet.
    virtual void* NativeHandle() const = 0;
};

} // namespace aether::gfx::rhi
