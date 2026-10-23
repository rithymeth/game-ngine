#pragma once

#include "aether/gfx/rhi/device.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#define VK_USE_PLATFORM_WIN32_KHR
#elif defined(__ANDROID__)
#define VK_USE_PLATFORM_ANDROID_KHR // vkCreateAndroidSurfaceKHR, for an ANativeWindow
#endif
#include <vulkan/vulkan.h>

#include <vector>
#include <array>
#include <atomic>
#include <functional>
#include <mutex>

namespace aether::gfx::rhi::vulkan_backend {

VkImageLayout ToVkImageLayout(ResourceState state);

struct TextureRecord {
    VkImage image = VK_NULL_HANDLE;
};

struct PipelineRecord {
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    u32 push_constant_size_bytes = 0;
    bool enable_bindless_textures = false;
    bool use_vertex_buffer = false;
    u32 vertex_stride_bytes = 20;
};
struct ComputePipelineRecord {
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout storage_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout constant_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout sampled_layout = VK_NULL_HANDLE;
    u32 storage_buffer_count = 0;
    u32 sampled_texture_count = 0;
    u32 constant_buffer_count = 0;
};

// Host-visible + host-coherent, matching the rest of this RHI's demo-scale
// "no staging buffer" trade-off for vertex/index data (see
// IDevice::CreateVertexBuffer's comment).
struct BufferRecord {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size_bytes = 0;
    void* mapped = nullptr;
    VkDescriptorSet constants_set = VK_NULL_HANDLE; // frame constants: the set that points at this buffer (see CreateConstantBuffer)
    bool is_storage = false;
    bool is_constant = false;
    bool is_vertex = false;
    bool is_index = false;
    IndexFormat index_format = IndexFormat::UInt16;
};

// A real (device-local, staged) sampled texture — unlike BufferRecord above,
// texture sampling performance is the entire point of CreateTexture, so this
// one goes through a proper staging-buffer upload into DEVICE_LOCAL memory.
struct SampledTextureRecord {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

// Vulkan counterpart to d3d12_backend::D3D12Device. Notably: this
// environment has the Vulkan loader (a GPU driver ships it) but not the
// LunarG validation layers, which only come with the full SDK — so unlike
// the D3D12 backend, there is no debug/validation layer to fall back to at
// all; `enable_debug_layer` is accepted for API symmetry but currently
// always resolves to "off" (see the .cpp).
class VulkanDevice final : public IDevice {
public:
    explicit VulkanDevice(bool enable_debug_layer);
    ~VulkanDevice() override;

    std::unique_ptr<ISwapChain> CreateSwapChain(void* native_window_handle, u32 width, u32 height,
                                                 u32 buffer_count) override;
    std::unique_ptr<ICommandList> CreateCommandList() override;
    u64 Submit(ICommandList& cmd, ISwapChain* wait_on_swap_chain) override;
    void WaitForFence(u64 fence_value) override;
    bool IsFenceComplete(u64 fence_value) const override;

    std::unique_ptr<ICommandList> CreateComputeCommandList() override;
    u64 SubmitCompute(ICommandList& cmd) override;
    void WaitForComputeFence(u64 fence_value) override;
    bool IsComputeFenceComplete(u64 fence_value) const override;
    void ComputeQueueWaitOnGraphics(u64 graphics_fence_value) override;
    void GraphicsQueueWaitOnCompute(u64 compute_fence_value) override;

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
    const BufferRecord& GetBuffer(BufferHandle handle) const { return buffers_[handle.index]; }
    usize BufferCount() const { return buffers_.size(); }
    bool IsVertexBuffer(BufferHandle handle) const {
        return handle.IsValid() && handle.index < buffers_.size() && buffers_[handle.index].is_vertex;
    }
    bool IsIndexBuffer(BufferHandle handle, IndexFormat format) const {
        return handle.IsValid() && handle.index < buffers_.size() && buffers_[handle.index].is_index &&
               buffers_[handle.index].index_format == format;
    }

    SampledTextureHandle CreateTexture(u32 width, u32 height, const u8* rgba8_pixels) override;
    bool SupportsCompressedTexture(CompressedTextureFormat format) const override;
    SampledTextureHandle CreateCompressedTexture(CompressedTextureFormat format, u32 width, u32 height, std::span<const CompressedMip> mips) override;
    bool DestroyTexture(SampledTextureHandle texture) override;

    // Frame constants (PipelineDesc::frame_constants_size_bytes): descriptor set 2, one uniform buffer at binding 0, which is where the HLSL's
    // `register(b0, space2)` lands under DXC's default mapping (space N is set N, register bN is binding N).
    bool SupportsFrameConstants() const override { return true; }
    BufferHandle CreateFrameConstantBuffer(u64 size_bytes) override;
    void UpdateFrameConstantBuffer(BufferHandle buffer, const void* data, u64 size_bytes) override;
    VkDescriptorSetLayout FrameConstantsSetLayout() const { return frame_constants_set_layout_; }
    SampledTextureHandle CreateR32FloatTexture(u32 width, u32 height, const f32* values) override;
    const SampledTextureRecord& GetSampledTexture(SampledTextureHandle handle) const {
        return sampled_textures_[handle.index - kFirstUserBindlessTextureIndex];
    }
    usize SampledTextureCount() const { return sampled_textures_.size(); }

    // The device-global bindless texture table — TWO descriptor sets, not
    // one: set 0 is a SAMPLED_IMAGE array (kMaxBindlessTextures slots,
    // matching the shared HLSL's `Texture2D g_Textures[32] : register(t0,
    // space0)`), set 1 is a single SAMPLER (matching `SamplerState
    // g_Sampler : register(s0, space1)`). See the "Unified mode's
    // textured-quad pipeline" comment in rhi_demo/main.cpp for why the
    // shared HLSL puts them in different register spaces (hence different
    // Vulkan descriptor sets) rather than combining them — both built once
    // in the constructor so CreatePipeline can reference stable layouts from
    // the very first enable_bindless_textures pipeline.
    VkDescriptorSetLayout BindlessTextureSetLayout() const { return bindless_texture_set_layout_; }
    VkDescriptorSetLayout BindlessSamplerSetLayout() const { return bindless_sampler_set_layout_; }
    VkDescriptorSet BindlessTextureSet() const { return bindless_texture_set_; }
    VkDescriptorSet BindlessSamplerSet() const { return bindless_sampler_set_; }

    Backend GetBackend() const override { return Backend::Vulkan; }
    void* NativeHandle() const override { return device_; }

    VkInstance Instance() const { return instance_; }
    VkPhysicalDevice PhysicalDevice() const { return physical_device_; }
    VkDevice Handle() const { return device_; }
    VkQueue Queue() const { return queue_; }
    u32 QueueFamilyIndex() const { return queue_family_index_; }
    std::mutex& GraphicsQueueMutex() { return graphics_queue_mutex_; }

    // The async-compute queue: a genuinely separate, dedicated compute-only
    // queue family when the hardware exposes one (common on discrete GPUs —
    // it's what makes the concurrency real rather than just API shape), a
    // second queue instance from the graphics family if that's all that's
    // available, or — logged loudly if so — the same queue/family as
    // graphics, serializing "async" compute with graphics on hardware that
    // truly only exposes one queue. See PickPhysicalDevice's comment.
    VkQueue ComputeQueue() const { return compute_queue_; }
    u32 ComputeQueueFamilyIndex() const { return compute_queue_family_index_; }
    std::mutex& ComputeQueueMutex() {
        return compute_queue_ == queue_ ? graphics_queue_mutex_ : compute_queue_mutex_;
    }
    VkResult WaitForDeviceIdle();
    void WithQueuesIdle(const std::function<void()>& operation);
    void WithGraphicsQueueIdle(const std::function<void()>& operation);
    bool IsComputeDispatchWithinLimits(u32 groups_x, u32 groups_y, u32 groups_z) const {
        return groups_x <= max_compute_work_group_count_[0] && groups_y <= max_compute_work_group_count_[1] &&
               groups_z <= max_compute_work_group_count_[2];
    }

    TextureHandle RegisterTexture(VkImage image);
    void UpdateTexture(TextureHandle handle, VkImage image);
    void UnregisterTexture(TextureHandle handle);
    VkImage GetTexture(TextureHandle handle) const;
    void DeferCommandListResources(VkCommandPool command_pool, std::vector<VkDescriptorPool> descriptor_pools) noexcept;

private:
    struct DeferredCommandListResources {
        VkCommandPool command_pool = VK_NULL_HANDLE;
        std::vector<VkDescriptorPool> descriptor_pools;
    };
    std::mutex deferred_command_list_resources_mutex_;
    std::vector<DeferredCommandListResources> deferred_command_list_resources_;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    u32 queue_family_index_ = 0;
    std::mutex graphics_queue_mutex_;

    // A timeline semaphore stands in for a D3D12-style monotonically
    // increasing fence value: vkSignalSemaphore/wait-on-value instead of
    // vkResetFences per submission. Requires Vulkan 1.2 (VK_KHR_timeline_semaphore
    // core-promoted), which we require at instance/device creation.
    VkSemaphore timeline_semaphore_ = VK_NULL_HANDLE;
    std::atomic<u64> next_fence_value_{1};

    VkQueue compute_queue_ = VK_NULL_HANDLE;
    u32 compute_queue_family_index_ = 0;
    std::mutex compute_queue_mutex_;
    std::array<u32, 3> max_compute_work_group_count_{1, 1, 1};
    VkDeviceSize max_uniform_buffer_range_ = 0;
    VkDeviceSize max_storage_buffer_range_ = 0;
    u32 max_compute_storage_buffers_ = 0;
    u32 max_compute_uniform_buffers_ = 0;
    u32 max_compute_sampled_images_ = 0;
    u32 max_compute_resources_ = 0;
    u32 max_descriptor_set_storage_buffers_ = 0;
    u32 max_descriptor_set_uniform_buffers_ = 0;
    u32 max_descriptor_set_sampled_images_ = 0;
    VkSemaphore compute_timeline_semaphore_ = VK_NULL_HANDLE;
    std::atomic<u64> next_compute_fence_value_{1};

    // Vulkan has no queue-level wait primitive independent of a submission
    // (unlike ID3D12CommandQueue::Wait) — a cross-queue wait can only attach
    // to an actual vkQueueSubmit's pWaitSemaphores. ComputeQueueWaitOnGraphics/
    // GraphicsQueueWaitOnCompute stash the request here; the next
    // SubmitCompute/Submit call on that queue consumes (and clears) it.
    struct PendingWait {
        bool valid = false;
        VkSemaphore semaphore = VK_NULL_HANDLE;
        u64 value = 0;
    };
    PendingWait pending_compute_wait_;
    PendingWait pending_graphics_wait_;

    std::vector<TextureRecord> textures_;
    std::vector<PipelineRecord> pipelines_;
    std::vector<ComputePipelineRecord> compute_pipelines_;
    std::vector<BufferRecord> buffers_;
    std::vector<SampledTextureRecord> sampled_textures_; // position i is bindless slot i + kFirstUserBindlessTextureIndex; a freed one is empty
    std::vector<u32> free_texture_slots_;                // positions in sampled_textures_ DestroyTexture emptied, reused first
    // Stores `record` in the first free position (or a new one) and returns its bindless index.
    u32 StoreTextureRecord(const SampledTextureRecord& record);
    bool TextureTableFull() const { return sampled_textures_.size() - free_texture_slots_.size() >= kMaxUserBindlessTextures; }

    VkDescriptorSetLayout bindless_texture_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout bindless_sampler_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool bindless_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet bindless_texture_set_ = VK_NULL_HANDLE;
    VkDescriptorSet bindless_sampler_set_ = VK_NULL_HANDLE;
    VkSampler bindless_sampler_ = VK_NULL_HANDLE;
    // Frame constants: one uniform buffer per set, one set per constant buffer, allocated from this pool.
    VkDescriptorSetLayout frame_constants_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool frame_constants_pool_ = VK_NULL_HANDLE;
    static constexpr u32 kMaxConstantBuffers = 64;
    u32 constant_buffer_count_ = 0;

    // A 1x1 white dummy image, permanently retained for slot zero and also
    // referenced by every other bindless descriptor slot before real
    // CreateTexture() calls claim them — see
    // D3D12Device::dummy_texture_'s comment for why every slot needs a valid
    // descriptor from the start, not just the ones a particular draw
    // actually samples.
    SampledTextureRecord dummy_texture_;

    BufferHandle CreateBufferInternal(const void* data, u64 size_bytes);
    void CreateBindlessTextureInfrastructure();
    SampledTextureRecord UploadTextureRecord(u32 width, u32 height, const u8* rgba8_pixels);
    // One image of `format` with the given levels (largest first; each `size` bytes of tightly packed texels or blocks), staged and copied.
    struct ImageLevel {
        const u8* data = nullptr;
        u64 size = 0;
        u32 width = 0, height = 0;
    };
    SampledTextureRecord UploadImageRecord(VkFormat format, u32 width, u32 height, const std::vector<ImageLevel>& levels);
    // Writes `record` into slot `index` of the bindless table.
    void WriteBindlessTexture(u32 index, const SampledTextureRecord& record);
    void Destroy() noexcept;
};

// A window swap chain (VkSurfaceKHR + VkSwapchainKHR) or, when created
// with a null window handle, an offscreen one: the same render pass,
// framebuffers and depth buffer around plain device-local images, with no
// surface, no acquire/present semaphores and a no-op Present - what
// headless tools and the screenshot tests render into (lavapipe in CI).
// The window handle is an HWND on Windows and a GLFWwindow* elsewhere
// (aether::Window::PlatformWindow()).
class VulkanSwapChain final : public ISwapChain {
public:
    VulkanSwapChain(VulkanDevice& device, void* native_window_handle, u32 width, u32 height, u32 buffer_count);
    ~VulkanSwapChain() override;

    void Resize(u32 width, u32 height) override;
    void AcquireNextImage() override;
    void Present(bool vsync) override;
    TextureHandle CurrentBackBuffer() const override { return handles_[current_image_index_]; }
    u32 Width() const override { return extent_.width; }
    u32 Height() const override { return extent_.height; }
    u32 BufferCount() const override { return static_cast<u32>(handles_.size()); }
    void* NativeHandle() const override { return const_cast<VulkanSwapChain*>(this); }
    bool ReadBack(std::vector<u8>& rgba8) override;
    bool IsOffscreen() const { return offscreen_; }
    bool BelongsTo(const VulkanDevice& device) const { return &device_ == &device; }

    // Used by VulkanDevice::Submit to wire up the acquire/present semaphores
    // for whichever frame-in-flight slot is current.
    // VK_NULL_HANDLE for an offscreen swap chain, which has nothing to wait on.
    VkSemaphore CurrentImageAvailableSemaphore() const {
        return offscreen_ ? VK_NULL_HANDLE : image_available_semaphores_[frame_index_];
    }
    VkSemaphore CurrentRenderFinishedSemaphore() const {
        return offscreen_ ? VK_NULL_HANDLE : render_finished_semaphores_[frame_index_];
    }

    // Escape hatch for backend-specific rendering beyond clear-to-color
    // (e.g. a real render pass + framebuffer + pipeline): the abstract
    // ISwapChain interface only exposes an opaque TextureHandle, but a
    // render-pass-based draw needs the actual VkImageView and VkFormat, plus
    // the swapchain-local image index (distinct from TextureHandle::index,
    // which is an index into the *device's* global texture table) to select
    // the matching per-image framebuffer.
    VkFormat Format() const { return format_; }
    u32 CurrentImageIndex() const { return current_image_index_; }
    VkImageView ImageView(u32 index) const { return image_views_[index]; }

    // Backing for ICommandList::BeginRenderPass/IDevice::CreatePipeline (the
    // "Unified Cross-API Renderer" follow-up): one render pass for this
    // swapchain's format, created once and stable across resize, plus one
    // framebuffer per image, recreated alongside the image views whenever
    // the swapchain itself is recreated (resize or out-of-date).
    VkRenderPass DefaultRenderPass() const { return default_render_pass_; }
    VkFramebuffer CurrentFramebuffer() const { return framebuffers_[current_image_index_]; }

private:
    void CreateSwapchainAndImages(u32 width, u32 height);
    std::vector<VkImage> CreateOffscreenImages(u32 width, u32 height);
    void DestroySwapchainAndImages();
    void CreateSyncObjects();
    void DestroySyncObjects();
    void CreateDefaultRenderPass();
    void CreateFramebuffers();
    void DestroyFramebuffers();
    void CreateDepthResources();
    void DestroyDepthResources();

    VulkanDevice& device_;
    void* window_ = nullptr; // HWND or GLFWwindow*; null when offscreen
    bool offscreen_ = false;
    bool can_read_back_ = false;
    bool recreate_after_present_ = false;
    std::vector<VkImage> offscreen_images_;          // offscreen only: the images...
    std::vector<VkDeviceMemory> offscreen_memory_;   // ...and their memory
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkColorSpaceKHR color_space_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent_{};
    u32 requested_buffer_count_ = 2;
    VkRenderPass default_render_pass_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;  // one per swapchain image

    // One depth attachment per swapchain image prevents frames in flight
    // from sharing writable depth data. They are recreated on resize.
    // Always present in default_render_pass_'s
    // subpass and every framebuffer, regardless of whether any given
    // pipeline actually depth-tests (PipelineDesc::depth_test toggles a
    // pipeline's own VkPipelineDepthStencilStateCreateInfo, independent of
    // whether the render pass/framebuffer it runs in has a depth
    // attachment) — this mirrors the D3D12 backend always binding its own
    // depth buffer in BeginRenderPass regardless of the bound pipeline.
    VkFormat depth_format_ = VK_FORMAT_UNDEFINED;
    std::vector<VkImage> depth_images_;
    std::vector<VkDeviceMemory> depth_memories_;
    std::vector<VkImageView> depth_image_views_;

    std::vector<TextureHandle> handles_;                   // one per swapchain image
    std::vector<VkImageView> image_views_;                 // one per swapchain image
    std::vector<VkSemaphore> image_available_semaphores_;  // one per frame-in-flight slot
    std::vector<VkSemaphore> render_finished_semaphores_;
    u32 frame_index_ = 0;          // rotates through frame-in-flight slots each AcquireNextImage()
    u32 current_image_index_ = 0;  // set by the most recent AcquireNextImage()
};

class VulkanCommandList final : public ICommandList {
public:
    // queue_family_index selects which queue family's command pool this
    // list's buffer comes from — must match whichever queue it's later
    // submitted to (VulkanDevice::Queue() vs ComputeQueue()), matching
    // D3D12CommandList's `type` parameter serving the same purpose.
    explicit VulkanCommandList(VulkanDevice& device, u32 queue_family_index, bool compute_queue = false);
    ~VulkanCommandList() override;

    void Reset() override;
    void Close() override;
    void TransitionTexture(TextureHandle texture, ResourceState before, ResourceState after) override;
    void ClearRenderTarget(TextureHandle texture, const ClearColor& color) override;
    void SetViewportAndScissor(const Viewport& viewport, const Rect& scissor) override;

    void BeginRenderPass(ISwapChain& swap_chain, const ClearColor& clear_color) override;
    void EndRenderPass() override;
    void BindPipeline(PipelineHandle pipeline) override;
    void SetPushConstants(const void* data, u32 size_bytes) override;
    void Draw(u32 vertex_count) override;

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

    void* NativeHandle() const override { return command_buffer_; }
    bool BelongsTo(const VulkanDevice& device) const { return &device_ == &device; }
    bool IsComputeList() const { return compute_queue_; }
    bool IsReadyForSubmit() const { return !recording_ && has_closed_recording_; }
    bool IsSubmissionInFlight() const {
        return last_submitted_fence_ != 0 &&
               !(compute_queue_ ? device_.IsComputeFenceComplete(last_submitted_fence_)
                                : device_.IsFenceComplete(last_submitted_fence_));
    }
    void MarkSubmitted(u64 fence_value) { last_submitted_fence_ = fence_value; }

private:
    VkDescriptorPool CreateComputeDescriptorPool();
    VulkanDevice& device_;
    bool compute_queue_ = false;
    bool recording_ = false;
    bool has_closed_recording_ = false;
    u64 last_submitted_fence_ = 0;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> compute_descriptor_pools_;
    VkDescriptorSet bound_compute_descriptor_set_ = VK_NULL_HANDLE;
    VkDescriptorSet bound_compute_sampled_set_ = VK_NULL_HANDLE;
    ComputePipelineHandle bound_compute_pipeline_{};
    std::vector<bool> compute_storage_bound_;
    std::vector<bool> compute_sampled_texture_bound_;
    std::vector<bool> compute_constant_buffer_bound_;
    VkDescriptorSet bound_compute_constant_set_ = VK_NULL_HANDLE;
    VkPipelineLayout bound_pipeline_layout_ = VK_NULL_HANDLE; // set by BindPipeline, read by SetPushConstants
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
};

} // namespace aether::gfx::rhi::vulkan_backend
