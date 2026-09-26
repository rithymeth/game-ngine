#include "aether/gfx/rhi/d3d12/d3d12_backend.h"

namespace aether::gfx::rhi::d3d12_backend {

D3D12_RESOURCE_STATES ToD3D12(ResourceState state) {
    switch (state) {
        case ResourceState::RenderTarget: return D3D12_RESOURCE_STATE_RENDER_TARGET;
        case ResourceState::Present: return D3D12_RESOURCE_STATE_PRESENT;
        case ResourceState::CopyDest: return D3D12_RESOURCE_STATE_COPY_DEST;
        case ResourceState::CopySource: return D3D12_RESOURCE_STATE_COPY_SOURCE;
        case ResourceState::Undefined: default: return D3D12_RESOURCE_STATE_COMMON;
    }
}

D3D12Device::D3D12Device(bool enable_debug_layer) : device_(enable_debug_layer) {}

std::unique_ptr<ISwapChain> D3D12Device::CreateSwapChain(void* native_window_handle, u32 width, u32 height,
                                                          u32 buffer_count) {
    return std::make_unique<D3D12SwapChain>(*this, native_window_handle, width, height, buffer_count);
}

std::unique_ptr<ICommandList> D3D12Device::CreateCommandList() {
    return std::make_unique<D3D12CommandList>(*this);
}

u64 D3D12Device::Submit(ICommandList& cmd, ISwapChain* wait_on_swap_chain) {
    (void)wait_on_swap_chain; // D3D12's flip-model swap chain needs no acquire/present semaphores
    auto* native = static_cast<ID3D12GraphicsCommandList*>(cmd.NativeHandle());
    ID3D12CommandList* lists[] = {native};
    return device_.Submit(lists, 1);
}

void D3D12Device::WaitForFence(u64 fence_value) {
    device_.WaitForFence(fence_value);
}

std::unique_ptr<ICommandList> D3D12Device::CreateComputeCommandList() {
    return std::make_unique<D3D12CommandList>(*this, D3D12_COMMAND_LIST_TYPE_COMPUTE);
}

u64 D3D12Device::SubmitCompute(ICommandList& cmd) {
    auto* native = static_cast<ID3D12GraphicsCommandList*>(cmd.NativeHandle());
    ID3D12CommandList* lists[] = {native};
    return device_.SubmitCompute(lists, 1);
}

TextureHandle D3D12Device::RegisterTexture(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE rtv) {
    TextureHandle handle{static_cast<u32>(textures_.size())};
    textures_.push_back({resource, rtv});
    return handle;
}

void D3D12Device::UpdateTexture(TextureHandle handle, ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE rtv) {
    textures_[handle.index] = {resource, rtv};
}

D3D12SwapChain::D3D12SwapChain(D3D12Device& device, void* hwnd, u32 width, u32 height, u32 buffer_count)
    : device_(device), swap_chain_(device.Native(), hwnd, width, height, buffer_count) {
    for (u32 i = 0; i < swap_chain_.BufferCount(); ++i) {
        handles_.push_back(device_.RegisterTexture(swap_chain_.BackBuffer(i), swap_chain_.BackBufferRTV(i)));
    }
}

void D3D12SwapChain::Resize(u32 width, u32 height) {
    swap_chain_.Resize(width, height);
    for (u32 i = 0; i < swap_chain_.BufferCount(); ++i) {
        device_.UpdateTexture(handles_[i], swap_chain_.BackBuffer(i), swap_chain_.BackBufferRTV(i));
    }
}

D3D12CommandList::D3D12CommandList(D3D12Device& device, D3D12_COMMAND_LIST_TYPE type)
    : device_(device), cmd_(device.Native(), type) {}

void D3D12CommandList::TransitionTexture(TextureHandle texture, ResourceState before, ResourceState after) {
    const TextureRecord& record = device_.GetTexture(texture);
    D3D12_RESOURCE_BARRIER barrier = TransitionBarrier(record.resource, ToD3D12(before), ToD3D12(after));
    cmd_.Get()->ResourceBarrier(1, &barrier);
}

void D3D12CommandList::ClearRenderTarget(TextureHandle texture, const ClearColor& color) {
    const TextureRecord& record = device_.GetTexture(texture);
    const f32 c[4] = {color.r, color.g, color.b, color.a};
    cmd_.Get()->ClearRenderTargetView(record.rtv, c, 0, nullptr);
}

void D3D12CommandList::SetViewportAndScissor(const Viewport& viewport, const Rect& scissor) {
    D3D12_VIEWPORT vp{viewport.x,    viewport.y,        viewport.width,
                       viewport.height, viewport.min_depth, viewport.max_depth};
    D3D12_RECT rect{scissor.left, scissor.top, scissor.right, scissor.bottom};
    cmd_.Get()->RSSetViewports(1, &vp);
    cmd_.Get()->RSSetScissorRects(1, &rect);
}

} // namespace aether::gfx::rhi::d3d12_backend
