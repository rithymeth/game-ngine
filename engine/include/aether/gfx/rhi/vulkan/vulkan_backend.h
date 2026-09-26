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

private:
    void CreateSwapchainAndImages(u32 width, u32 height);
    void DestroySwapchainAndImages();
    void CreateSyncObjects();
    void DestroySyncObjects();

    VulkanDevice& device_;
    void* hwnd_ = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D extent_{};
    u32 requested_buffer_count_ = 2;

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
    void* NativeHandle() const override { return command_buffer_; }

private:
    VulkanDevice& device_;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
};

} // namespace aether::gfx::rhi::vulkan_backend
