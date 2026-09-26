#include "aether/gfx/swap_chain.h"

#include "aether/gfx/device.h"

namespace aether::gfx {

SwapChain::SwapChain(Device& device, void* hwnd, u32 width, u32 height, u32 buffer_count)
    : device_(device), width_(width), height_(height), buffer_count_(buffer_count) {
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = format_;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = buffer_count;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> swap_chain1;
    AETHER_D3D_CHECK(device_.Factory()->CreateSwapChainForHwnd(device_.Queue(), static_cast<HWND>(hwnd), &desc,
                                                                nullptr, nullptr, &swap_chain1));
    AETHER_D3D_CHECK(device_.Factory()->MakeWindowAssociation(static_cast<HWND>(hwnd), DXGI_MWA_NO_ALT_ENTER));
    AETHER_D3D_CHECK(swap_chain1.As(&swap_chain_));

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
    rtv_heap_desc.NumDescriptors = buffer_count;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    AETHER_D3D_CHECK(device_.Handle()->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap_)));
    rtv_descriptor_size_ = device_.Handle()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    back_buffers_.resize(buffer_count);
    CreateRenderTargetViews();
}

SwapChain::~SwapChain() = default;

void SwapChain::CreateRenderTargetViews() {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    for (u32 i = 0; i < buffer_count_; ++i) {
        AETHER_D3D_CHECK(swap_chain_->GetBuffer(i, IID_PPV_ARGS(&back_buffers_[i])));
        device_.Handle()->CreateRenderTargetView(back_buffers_[i].Get(), nullptr, handle);
        handle.ptr += rtv_descriptor_size_;
    }
}

void SwapChain::ReleaseBackBuffers() {
    device_.WaitForFence(device_.Submit(nullptr, 0)); // drain the queue before releasing in-use backbuffers
    for (auto& buffer : back_buffers_) {
        buffer.Reset();
    }
}

void SwapChain::Resize(u32 width, u32 height) {
    if (width == 0 || height == 0 || (width == width_ && height == height_)) {
        return;
    }

    ReleaseBackBuffers();
    AETHER_D3D_CHECK(swap_chain_->ResizeBuffers(buffer_count_, width, height, format_, 0));
    width_ = width;
    height_ = height;
    CreateRenderTargetViews();
}

void SwapChain::Present(bool vsync) {
    AETHER_D3D_CHECK(swap_chain_->Present(vsync ? 1 : 0, 0));
}

u32 SwapChain::CurrentBackBufferIndex() const {
    return swap_chain_->GetCurrentBackBufferIndex();
}

D3D12_CPU_DESCRIPTOR_HANDLE SwapChain::CurrentBackBufferRTV() const {
    return BackBufferRTV(CurrentBackBufferIndex());
}

D3D12_CPU_DESCRIPTOR_HANDLE SwapChain::BackBufferRTV(u32 index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * rtv_descriptor_size_;
    return handle;
}

} // namespace aether::gfx
