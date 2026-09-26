#pragma once

#include "aether/gfx/command_list.h"
#include "aether/gfx/device.h"
#include "aether/gfx/rhi/device.h"
#include "aether/gfx/swap_chain.h"

#include <vector>

namespace aether::gfx::rhi::d3d12_backend {

D3D12_RESOURCE_STATES ToD3D12(ResourceState state);

struct TextureRecord {
    ID3D12Resource* resource = nullptr; // not owned here — owned by whatever gfx:: object created it
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
};

// Adapts the existing, independently-tested gfx::Device (Phase 3) to
// IDevice by composition rather than inheritance — gfx::Device stays exactly
// as it was, this just wraps it and adds the texture-handle table
// ICommandList's abstract TransitionTexture/ClearRenderTarget need.
struct PipelineRecord {
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12PipelineState> pso;
};

class D3D12Device final : public IDevice {
public:
    explicit D3D12Device(bool enable_debug_layer);

    std::unique_ptr<ISwapChain> CreateSwapChain(void* native_window_handle, u32 width, u32 height,
                                                 u32 buffer_count) override;
    std::unique_ptr<ICommandList> CreateCommandList() override;
    u64 Submit(ICommandList& cmd, ISwapChain* wait_on_swap_chain = nullptr) override;
    void WaitForFence(u64 fence_value) override;
    bool IsFenceComplete(u64 fence_value) const override { return device_.IsFenceComplete(fence_value); }

    PipelineHandle CreatePipeline(const PipelineDesc& desc, ISwapChain& swap_chain) override;
    const PipelineRecord& GetPipeline(PipelineHandle handle) const { return pipelines_[handle.index]; }

    std::unique_ptr<ICommandList> CreateComputeCommandList() override;
    u64 SubmitCompute(ICommandList& cmd) override;
    void WaitForComputeFence(u64 fence_value) override { device_.WaitForComputeFence(fence_value); }
    bool IsComputeFenceComplete(u64 fence_value) const override { return device_.IsComputeFenceComplete(fence_value); }
    void ComputeQueueWaitOnGraphics(u64 graphics_fence_value) override {
        device_.ComputeQueueWaitOnGraphics(graphics_fence_value);
    }
    void GraphicsQueueWaitOnCompute(u64 compute_fence_value) override {
        device_.GraphicsQueueWaitOnCompute(compute_fence_value);
    }

    Backend GetBackend() const override { return Backend::D3D12; }
    void* NativeHandle() const override { return device_.Handle(); }

    gfx::Device& Native() { return device_; }

    TextureHandle RegisterTexture(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE rtv);
    void UpdateTexture(TextureHandle handle, ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE rtv);
    const TextureRecord& GetTexture(TextureHandle handle) const { return textures_[handle.index]; }

private:
    gfx::Device device_;
    std::vector<TextureRecord> textures_;
    std::vector<PipelineRecord> pipelines_;
};

class D3D12SwapChain final : public ISwapChain {
public:
    D3D12SwapChain(D3D12Device& device, void* hwnd, u32 width, u32 height, u32 buffer_count);

    void Resize(u32 width, u32 height) override;
    void AcquireNextImage() override {} // not needed: DXGI's flip model exposes the current index directly
    void Present(bool vsync) override { swap_chain_.Present(vsync); }
    TextureHandle CurrentBackBuffer() const override { return handles_[swap_chain_.CurrentBackBufferIndex()]; }
    u32 Width() const override { return swap_chain_.Width(); }
    u32 Height() const override { return swap_chain_.Height(); }
    u32 BufferCount() const override { return swap_chain_.BufferCount(); }
    void* NativeHandle() const override { return const_cast<gfx::SwapChain*>(&swap_chain_); }

    gfx::SwapChain& Native() { return swap_chain_; }
    u32 CurrentIndex() const { return swap_chain_.CurrentBackBufferIndex(); }

    // Tracks, per backbuffer index, whether BeginRenderPass has transitioned
    // it before — a fresh backbuffer starts life in COMMON, everything after
    // its first use starts from PRESENT (matching the RHI's existing
    // TransitionTexture convention, now applied automatically instead of by
    // the caller). Returns the PREVIOUS value and marks it used.
    bool ConsumeUsedBefore(u32 index) {
        bool previous = used_before_[index];
        used_before_[index] = true;
        return previous;
    }

private:
    D3D12Device& device_;
    gfx::SwapChain swap_chain_;
    std::vector<TextureHandle> handles_;
    std::vector<bool> used_before_;
};

class D3D12CommandList final : public ICommandList {
public:
    explicit D3D12CommandList(D3D12Device& device, D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT);

    void Reset() override { cmd_.Reset(); }
    void Close() override { cmd_.Close(); }
    void TransitionTexture(TextureHandle texture, ResourceState before, ResourceState after) override;
    void ClearRenderTarget(TextureHandle texture, const ClearColor& color) override;
    void SetViewportAndScissor(const Viewport& viewport, const Rect& scissor) override;

    void BeginRenderPass(ISwapChain& swap_chain, const ClearColor& clear_color) override;
    void EndRenderPass() override;
    void BindPipeline(PipelineHandle pipeline) override;
    void SetPushConstants(const void* data, u32 size_bytes) override;
    void Draw(u32 vertex_count) override;

    void* NativeHandle() const override { return cmd_.Get(); }

private:
    D3D12Device& device_;
    gfx::CommandList cmd_;
    ID3D12Resource* active_backbuffer_ = nullptr; // set by BeginRenderPass, consumed by EndRenderPass
};

} // namespace aether::gfx::rhi::d3d12_backend
