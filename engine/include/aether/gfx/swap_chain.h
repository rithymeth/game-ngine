#pragma once

#include "aether/gfx/d3d12_common.h"

#include <vector>

namespace aether::gfx {

class Device;

// Manual double/triple buffering: this class only owns the DXGI swap chain
// and per-backbuffer RTVs. It does NOT track which backbuffers are still in
// flight on the GPU — that's the frame loop's job (wait on the fence value
// recorded the last time this backbuffer index was used, before reusing its
// command allocator), consistent with the engine's explicit-sync principle.
class SwapChain {
public:
    SwapChain(Device& device, void* hwnd, u32 width, u32 height, u32 buffer_count = 2);
    ~SwapChain();

    SwapChain(const SwapChain&) = delete;
    SwapChain& operator=(const SwapChain&) = delete;

    void Resize(u32 width, u32 height);
    void Present(bool vsync);

    u32 CurrentBackBufferIndex() const;
    ID3D12Resource* CurrentBackBuffer() const { return back_buffers_[CurrentBackBufferIndex()].Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE CurrentBackBufferRTV() const;

    u32 Width() const { return width_; }
    u32 Height() const { return height_; }
    u32 BufferCount() const { return buffer_count_; }
    DXGI_FORMAT Format() const { return format_; }

private:
    void CreateRenderTargetViews();
    void ReleaseBackBuffers();

    Device& device_;
    ComPtr<IDXGISwapChain3> swap_chain_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    std::vector<ComPtr<ID3D12Resource>> back_buffers_;
    u32 rtv_descriptor_size_ = 0;
    u32 width_ = 0;
    u32 height_ = 0;
    u32 buffer_count_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_R8G8B8A8_UNORM;
};

} // namespace aether::gfx
