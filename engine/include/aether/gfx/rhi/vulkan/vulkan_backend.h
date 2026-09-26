#pragma once

#include "aether/gfx/rhi/device.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <vector>

namespace aether::gfx::rhi::vulkan_backend {

VkImageLayout ToVkImageLayout(ResourceState state);

struct TextureRecord {
    VkImage image = VK_NULL_HANDLE;
};

struct PipelineRecord {
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

// Host-visible + host-coherent, matching the rest of this RHI's demo-scale
// "no staging buffer" trade-off for vertex/index data (see
// IDevice::CreateVertexBuffer's comment).
struct BufferRecord {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
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
    const PipelineRecord& GetPipeline(PipelineHandle handle) const { return pipelines_[handle.index]; }

    BufferHandle CreateVertexBuffer(const void* data, u64 size_bytes) override;
    BufferHandle CreateIndexBuffer(const void* data, u64 size_bytes, IndexFormat format) override;
    const BufferRecord& GetBuffer(BufferHandle handle) const { return buffers_[handle.index]; }

    SampledTextureHandle CreateTexture(u32 width, u32 height, const u8* rgba8_pixels) override;

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

    // The async-compute queue: a genuinely separate, dedicated compute-only
    // queue family when the hardware exposes one (common on discrete GPUs —
    // it's what makes the concurrency real rather than just API shape), a
    // second queue instance from the graphics family if that's all that's
    // available, or — logged loudly if so — the same queue/family as
    // graphics, serializing "async" compute with graphics on hardware that
    // truly only exposes one queue. See PickPhysicalDevice's comment.
    VkQueue ComputeQueue() const { return compute_queue_; }
    u32 ComputeQueueFamilyIndex() const { return compute_queue_family_index_; }

    TextureHandle RegisterTexture(VkImage image);
    void UpdateTexture(TextureHandle handle, VkImage image);
    VkImage GetTexture(TextureHandle handle) const { return textures_[handle.index].image; }

private:
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    u32 queue_family_index_ = 0;

    // A timeline semaphore stands in for a D3D12-style monotonically
    // increasing fence value: vkSignalSemaphore/wait-on-value instead of
    // vkResetFences per submission. Requires Vulkan 1.2 (VK_KHR_timeline_semaphore
    // core-promoted), which we require at instance/device creation.
    VkSemaphore timeline_semaphore_ = VK_NULL_HANDLE;
    u64 next_fence_value_ = 1;

    VkQueue compute_queue_ = VK_NULL_HANDLE;
    u32 compute_queue_family_index_ = 0;
    VkSemaphore compute_timeline_semaphore_ = VK_NULL_HANDLE;
    u64 next_compute_fence_value_ = 1;

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
    std::vector<BufferRecord> buffers_;
    std::vector<SampledTextureRecord> sampled_textures_;

    VkDescriptorSetLayout bindless_texture_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout bindless_sampler_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool bindless_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet bindless_texture_set_ = VK_NULL_HANDLE;
    VkDescriptorSet bindless_sampler_set_ = VK_NULL_HANDLE;
    VkSampler bindless_sampler_ = VK_NULL_HANDLE;

    // A 1x1 white dummy image, referenced by every bindless descriptor slot
    // no real CreateTexture() call has claimed yet — see
    // D3D12Device::dummy_texture_'s comment for why every slot needs a valid
    // descriptor from the start, not just the ones a particular draw
    // actually samples.
    SampledTextureRecord dummy_texture_;

    BufferHandle CreateBufferInternal(const void* data, u64 size_bytes);
    void CreateBindlessTextureInfrastructure();
    SampledTextureRecord UploadTextureRecord(u32 width, u32 height, const u8* rgba8_pixels);
};

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

    // Used by VulkanDevice::Submit to wire up the acquire/present semaphores
    // for whichever frame-in-flight slot is current.
    VkSemaphore CurrentImageAvailableSemaphore() const { return image_available_semaphores_[frame_index_]; }
    VkSemaphore CurrentRenderFinishedSemaphore() const { return render_finished_semaphores_[frame_index_]; }

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
    void DestroySwapchainAndImages();
    void CreateSyncObjects();
    void DestroySyncObjects();
    void CreateDefaultRenderPass();
    void CreateFramebuffers();
    void DestroyFramebuffers();
    void CreateDepthResources();
    void DestroyDepthResources();

    VulkanDevice& device_;
    void* hwnd_ = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D extent_{};
    u32 requested_buffer_count_ = 2;
    VkRenderPass default_render_pass_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;  // one per swapchain image

    // A single depth buffer, shared by every swapchain image (recreated
    // alongside them on resize) — the "Unified renderer: depth buffer +
    // blend states" follow-up. Always present in default_render_pass_'s
    // subpass and every framebuffer, regardless of whether any given
    // pipeline actually depth-tests (PipelineDesc::depth_test toggles a
    // pipeline's own VkPipelineDepthStencilStateCreateInfo, independent of
    // whether the render pass/framebuffer it runs in has a depth
    // attachment) — this mirrors the D3D12 backend always binding its own
    // depth buffer in BeginRenderPass regardless of the bound pipeline.
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
    VkImage depth_image_ = VK_NULL_HANDLE;
    VkDeviceMemory depth_memory_ = VK_NULL_HANDLE;
    VkImageView depth_image_view_ = VK_NULL_HANDLE;

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
    explicit VulkanCommandList(VulkanDevice& device, u32 queue_family_index);
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
    void BindBindlessTextures() override;

    void* NativeHandle() const override { return command_buffer_; }

private:
    VulkanDevice& device_;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    VkPipelineLayout bound_pipeline_layout_ = VK_NULL_HANDLE; // set by BindPipeline, read by SetPushConstants
};

} // namespace aether::gfx::rhi::vulkan_backend
