#pragma once

#include "aether/gfx/buffer.h"
#include "aether/gfx/command_list.h"
#include "aether/gfx/descriptor_heap.h"
#include "aether/gfx/device.h"
#include "aether/gfx/rhi/device.h"
#include "aether/gfx/swap_chain.h"
#include "aether/gfx/texture.h"

#include <memory>
#include <mutex>
#include <optional>
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
    u32 frame_constants_root_index = ~0u; // the root CBV for PipelineDesc::frame_constants_size_bytes, if any
    u32 push_constant_size_bytes = 0;
    bool enable_bindless_textures = false;
    bool use_vertex_buffer = false;
    u32 vertex_stride_bytes = 20;
};
struct ComputePipelineRecord {
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12PipelineState> pso;
    u32 storage_buffer_count = 0;
    u32 sampled_texture_count = 0;
    u32 constant_buffer_count = 0;
};

class D3D12Device final : public IDevice {
public:
    explicit D3D12Device(bool enable_debug_layer);
    ~D3D12Device() override;

    std::unique_ptr<ISwapChain> CreateSwapChain(void* native_window_handle, u32 width, u32 height,
                                                 u32 buffer_count) override;
    std::unique_ptr<ICommandList> CreateCommandList() override;
    u64 Submit(ICommandList& cmd, ISwapChain* wait_on_swap_chain = nullptr) override;
    void WaitForFence(u64 fence_value) override;
    bool IsFenceComplete(u64 fence_value) const override { return device_.IsFenceComplete(fence_value); }

    PipelineHandle CreatePipeline(const PipelineDesc& desc, ISwapChain& swap_chain) override;
    ComputePipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc) override;
    const PipelineRecord& GetPipeline(PipelineHandle handle) const { return pipelines_[handle.index]; }
    usize PipelineCount() const { return pipelines_.size(); }
    const ComputePipelineRecord& GetComputePipeline(ComputePipelineHandle handle) const { return compute_pipelines_[handle.index]; }
    usize ComputePipelineCount() const { return compute_pipelines_.size(); }

    BufferHandle CreateVertexBuffer(const void* data, u64 size_bytes) override;
    BufferHandle CreateIndexBuffer(const void* data, u64 size_bytes, IndexFormat format) override;
    BufferHandle CreateStorageBuffer(u64 size_bytes, u32 element_stride_bytes, const void* initial_data) override;
    void UpdateStorageBuffer(BufferHandle buffer, const void* data, u64 size_bytes, u64 offset) override;
    BufferHandle CreateConstantBuffer(u64 size_bytes, const void* initial_data) override;
    void UpdateConstantBuffer(BufferHandle buffer, const void* data, u64 size_bytes, u64 offset) override;
    void UpdateVertexBuffer(BufferHandle buffer, const void* data, u64 size_bytes) override;
    bool SupportsFrameConstants() const override { return true; }
    BufferHandle CreateFrameConstantBuffer(u64 size_bytes) override;
    void UpdateFrameConstantBuffer(BufferHandle buffer, const void* data, u64 size_bytes) override { UpdateConstantBuffer(buffer, data, size_bytes, 0); }
    const gfx::Buffer& GetBuffer(BufferHandle handle) const { return *buffers_[handle.index]; }
    usize BufferCount() const { return buffers_.size(); }
    D3D12_GPU_DESCRIPTOR_HANDLE ComputeStorageDescriptor(BufferHandle handle) const;
    D3D12_GPU_DESCRIPTOR_HANDLE ComputeSampledDescriptor(SampledTextureHandle handle) const;
    ID3D12DescriptorHeap* ComputeStorageHeap() const { return compute_storage_heap_->Heap(); }
    bool IsConstantBuffer(BufferHandle handle) const {
        return handle.IsValid() && handle.index < constant_buffer_flags_.size() && constant_buffer_flags_[handle.index];
    }
    bool IsStorageBuffer(BufferHandle handle) const {
        return handle.IsValid() && handle.index < storage_descriptor_indices_.size() &&
               storage_descriptor_indices_[handle.index] != DescriptorHeap::kInvalidIndex;
    }
    bool IsVertexBuffer(BufferHandle handle) const {
        return handle.IsValid() && handle.index < buffer_vertex_flags_.size() && buffer_vertex_flags_[handle.index];
    }
    bool IsIndexBuffer(BufferHandle handle, IndexFormat format) const {
        return handle.IsValid() && handle.index < index_buffer_formats_.size() &&
               index_buffer_formats_[handle.index].has_value() && index_buffer_formats_[handle.index] == format;
    }

    SampledTextureHandle CreateTexture(u32 width, u32 height, const u8* rgba8_pixels) override;
    bool SupportsCompressedTexture(CompressedTextureFormat f) const override { return f == CompressedTextureFormat::BC1 || f == CompressedTextureFormat::BC3 || f == CompressedTextureFormat::BC7; } // core D3D12; it has no ASTC
    SampledTextureHandle CreateCompressedTexture(CompressedTextureFormat format, u32 width, u32 height, std::span<const CompressedMip> mips) override;
    bool DestroyTexture(SampledTextureHandle texture) override;
    SampledTextureHandle CreateR32FloatTexture(u32 width, u32 height, const f32* values) override;
    DescriptorHeap& BindlessTextureHeap() { return *bindless_texture_heap_; }

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
    void DeferCommandAllocator(ComPtr<ID3D12CommandAllocator> allocator) noexcept;

    Backend GetBackend() const override { return Backend::D3D12; }
    void* NativeHandle() const override { return device_.Handle(); }

    gfx::Device& Native() { return device_; }

    TextureHandle RegisterTexture(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE rtv);
    void UpdateTexture(TextureHandle handle, ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE rtv);
    const TextureRecord& GetTexture(TextureHandle handle) const { return textures_[handle.index]; }
    bool IsTextureValid(TextureHandle handle) const {
        return handle.IsValid() && handle.index < textures_.size() && textures_[handle.index].resource != nullptr;
    }

private:
    BufferHandle CreateBufferInternal(const void* data, u64 size_bytes, bool is_vertex,
                                      std::optional<IndexFormat> index_format = std::nullopt);
    void ReserveBufferRegistryEntry();

    // Declared before device_ so deferred allocators are released only after
    // gfx::Device has made its final queue-drain attempt during destruction.
    std::vector<ComPtr<ID3D12CommandAllocator>> deferred_command_allocators_;
    std::mutex deferred_allocator_mutex_;
    gfx::Device device_;
    std::vector<TextureRecord> textures_;
    std::vector<PipelineRecord> pipelines_;
    std::vector<ComputePipelineRecord> compute_pipelines_;
    std::vector<std::unique_ptr<gfx::Buffer>> buffers_;
    std::vector<u32> storage_descriptor_indices_;
    std::vector<u32> sampled_descriptor_indices_;
    std::vector<bool> constant_buffer_flags_;
    std::vector<bool> buffer_vertex_flags_;
    std::vector<std::optional<IndexFormat>> index_buffer_formats_;

    // The device-global bindless texture table (see kMaxBindlessTextures):
    // built once here so a pipeline created with enable_bindless_textures
    // can reference a stable descriptor-table shape from the start, with
    // every slot pre-filled with dummy_texture_'s SRV (index 0 permanently)
    // so the debug layer never sees an uninitialized descriptor in the
    // table's range, regardless of how many real CreateTexture() calls have
    // happened yet.
    std::unique_ptr<DescriptorHeap> bindless_texture_heap_;
    std::unique_ptr<DescriptorHeap> compute_storage_heap_;
    std::unique_ptr<gfx::Texture> dummy_texture_;
    std::vector<std::unique_ptr<gfx::Texture>> sampled_textures_;
    std::vector<ComPtr<ID3D12Resource>> sampled_float_textures_;
    u32 sampled_texture_count_ = 0;
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
    bool ReadBack(std::vector<u8>& rgba8) override;
    bool BelongsTo(const D3D12Device& device) const { return &device_ == &device; }

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

    // Each backbuffer has its own depth resource so frames in flight never
    // share a writable depth attachment.
    D3D12_CPU_DESCRIPTOR_HANDLE DepthDSV(u32 index) const { return depth_dsv_heap_->CPUHandle(index); }

private:
    void CreateDepthBuffer(u32 width, u32 height);

    D3D12Device& device_;
    gfx::SwapChain swap_chain_;
    std::vector<TextureHandle> handles_;
    std::vector<bool> used_before_;

    std::vector<ComPtr<ID3D12Resource>> depth_resources_;
    std::unique_ptr<DescriptorHeap> depth_dsv_heap_;
};

class D3D12CommandList final : public ICommandList {
public:
    explicit D3D12CommandList(D3D12Device& device, D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT);
    ~D3D12CommandList() override;

    void Reset() override;
    void Close() override;
    void TransitionTexture(TextureHandle texture, ResourceState before, ResourceState after) override;
    void ClearRenderTarget(TextureHandle texture, const ClearColor& color) override;
    void SetViewportAndScissor(const Viewport& viewport, const Rect& scissor) override;

    void BeginRenderPass(ISwapChain& swap_chain, const ClearColor& clear_color) override;
    void EndRenderPass() override;
    void BindPipeline(PipelineHandle pipeline) override;
    void SetPushConstants(const void* data, u32 size_bytes) override;
    void Draw(u32 vertex_count, u32 first_vertex = 0) override;

    void BindVertexBuffer(BufferHandle buffer, u32 stride_bytes) override;
    void BindIndexBuffer(BufferHandle buffer, IndexFormat format) override;
    void DrawIndexed(u32 index_count) override;
    void BindComputePipeline(ComputePipelineHandle pipeline) override;
    void BindComputeStorageBuffer(u32 slot, BufferHandle buffer) override;
    void BindComputeSampledTexture(u32 slot, SampledTextureHandle texture) override;
    void BindComputeConstantBuffer(u32 slot, BufferHandle buffer) override;
    void ComputeBufferBarrier(BufferHandle buffer) override;
    void Dispatch(u32 groups_x, u32 groups_y, u32 groups_z) override;
    void BindBindlessTextures() override;
    void BindFrameConstants(BufferHandle buffer) override;

    void* NativeHandle() const override { return cmd_.Get(); }
    bool BelongsTo(const D3D12Device& device) const { return &device_ == &device; }
    bool IsComputeList() const { return type_ == D3D12_COMMAND_LIST_TYPE_COMPUTE; }
    std::mutex& SubmissionMutex() { return submission_mutex_; }
    bool IsReadyForSubmit() const { return !recording_ && has_closed_recording_; }
    bool IsSubmissionInFlight() const {
        return last_submitted_fence_ != 0 &&
               !(IsComputeList() ? device_.IsComputeFenceComplete(last_submitted_fence_)
                                 : device_.IsFenceComplete(last_submitted_fence_));
    }
    void MarkSubmitted(u64 fence_value) { last_submitted_fence_ = fence_value; }

private:
    D3D12Device& device_;
    D3D12_COMMAND_LIST_TYPE type_;
    gfx::CommandList cmd_;
    std::mutex submission_mutex_;
    bool recording_ = true; // D3D12 creates the native list in the open state.
    bool has_closed_recording_ = false;
    u64 last_submitted_fence_ = 0;
    ID3D12Resource* active_backbuffer_ = nullptr; // set by BeginRenderPass, consumed by EndRenderPass
    u32 frame_constants_root_index_ = ~0u;        // of the pipeline BindPipeline last bound
    u32 compute_storage_buffer_count_ = 0;
    u32 compute_sampled_texture_count_ = 0;
    u32 compute_constant_buffer_count_ = 0;
    bool compute_pipeline_bound_ = false;
    bool graphics_pipeline_bound_ = false;
    bool graphics_bindless_enabled_ = false;
    bool graphics_bindless_bound_ = false;
    bool graphics_uses_vertex_buffer_ = false;
    u32 graphics_vertex_stride_bytes_ = 0;
    bool vertex_buffer_bound_ = false;
    u64 bound_vertex_count_ = 0;
    bool index_buffer_bound_ = false;
    u64 bound_index_count_ = 0;
    u32 graphics_push_constant_size_bytes_ = 0;
    bool render_pass_active_ = false;
    std::vector<bool> compute_storage_bound_;
    std::vector<bool> compute_sampled_texture_bound_;
    std::vector<bool> compute_constant_buffer_bound_;
};

} // namespace aether::gfx::rhi::d3d12_backend
