#pragma once

#include "aether/gfx/rhi/types.h"

#include <vector>

namespace aether::gfx::rhi {

class ISwapChain {
public:
    virtual ~ISwapChain() = default;

    virtual void Resize(u32 width, u32 height) = 0;

    // Vulkan requires an explicit acquire step (vkAcquireNextImageKHR,
    // signaling a semaphore) before you know which image index to render
    // into this frame; D3D12's flip-model swap chain doesn't need this
    // (GetCurrentBackBufferIndex() is a plain query) and implements it as a
    // no-op. Call once per frame before CurrentBackBuffer()/recording,
    // regardless of backend.
    virtual void AcquireNextImage() = 0;

    virtual void Present(bool vsync) = 0;

    // Stable across Resize(): the same TextureHandle values keep being
    // returned, just backed by a new underlying image after a resize —
    // mirroring how RenderGraph::CreateTransientTexture keeps its
    // ResourceHandle stable across a recreate.
    virtual TextureHandle CurrentBackBuffer() const = 0;

    virtual u32 Width() const = 0;
    virtual u32 Height() const = 0;
    virtual u32 BufferCount() const = 0;

    // Escape hatch, same shape as IDevice/ICommandList's: lets
    // IDevice::Submit's Vulkan implementation reach this swap chain's
    // acquire/present semaphores without a backend-specific parameter type
    // in the abstract Submit() signature.
    virtual void* NativeHandle() const = 0;

    // Copies the current back buffer into `rgba8` (Width() x Height(), 4
    // bytes a pixel, rows top to bottom), waiting for the GPU first. Call it
    // between Submit and Present. False where the backend or swap chain
    // can't read back (Phase 24, docs/design/PHASE_SPECS.md §24.3: the
    // Vulkan backend's offscreen swap chains, and its window swap chains
    // where the surface allows it).
    virtual bool ReadBack(std::vector<u8>& rgba8) {
        (void)rgba8;
        return false;
    }
};

} // namespace aether::gfx::rhi
