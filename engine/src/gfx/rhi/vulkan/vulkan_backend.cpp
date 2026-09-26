#include "aether/gfx/rhi/vulkan/vulkan_backend.h"

#include "aether/core/log.h"

#include <stdexcept>
#include <vector>

namespace aether::gfx::rhi::vulkan_backend {

namespace {

void Check(VkResult result, const char* expr) {
    if (result != VK_SUCCESS) {
        AETHER_LOG_FATAL("Vulkan", "%s failed (VkResult=%d)", expr, static_cast<int>(result));
        throw std::runtime_error(expr);
    }
}

} // namespace

#define AETHER_VK_CHECK(expr) ::aether::gfx::rhi::vulkan_backend::Check((expr), #expr)

VkImageLayout ToVkImageLayout(ResourceState state) {
    switch (state) {
        // Maps to TRANSFER_DST, not COLOR_ATTACHMENT_OPTIMAL: this RHI slice
        // clears the backbuffer directly with vkCmdClearColorImage (no
        // render pass/framebuffer/pipeline exists yet), which requires a
        // transfer-capable layout, not an attachment one. D3D12's
        // ClearRenderTargetView has no such requirement (it needs
        // D3D12_RESOURCE_STATE_RENDER_TARGET, unconditionally) — this
        // mapping is what keeps ICommandList::ClearRenderTarget's calling
        // convention (Transition to RenderTarget, then clear) identical
        // across both backends despite the underlying mechanisms differing.
        // A future render-pass/pipeline-based path would need a real
        // COLOR_ATTACHMENT_OPTIMAL mapping alongside this one.
        case ResourceState::RenderTarget: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        case ResourceState::Present: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        case ResourceState::CopyDest: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        case ResourceState::CopySource: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case ResourceState::Undefined:
        default:
            return VK_IMAGE_LAYOUT_UNDEFINED;
    }
}

namespace {

// This environment has the Vulkan loader (the GPU driver ships it) but not
// the LunarG validation layers, which are only distributed with the full
// SDK — so there is no "VK_LAYER_KHRONOS_validation" to request here at all
// (requesting an absent layer is a hard vkCreateInstance failure, not a
// graceful fallback). enable_debug_layer is accepted for symmetry with the
// D3D12 backend but is currently always a no-op; a real validation path
// would check for the layer's availability first via
// vkEnumerateInstanceLayerProperties, matching how the D3D12 backend probes
// for its debug interface before using it.
VkInstance CreateInstance(bool /*enable_debug_layer*/) {
    VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app_info.pApplicationName = "Aether";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "Aether";
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = VK_API_VERSION_1_2; // timeline semaphores are core here

    const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};

    VkInstanceCreateInfo create_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = static_cast<u32>(std::size(extensions));
    create_info.ppEnabledExtensionNames = extensions;

    VkInstance instance = VK_NULL_HANDLE;
    AETHER_VK_CHECK(vkCreateInstance(&create_info, nullptr, &instance));
    return instance;
}

struct PhysicalDeviceChoice {
    VkPhysicalDevice device = VK_NULL_HANDLE;
    u32 queue_family_index = 0;
};

PhysicalDeviceChoice PickPhysicalDevice(VkInstance instance) {
    u32 count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (count == 0) {
        AETHER_LOG_FATAL("Vulkan", "No Vulkan-capable physical devices found");
        throw std::runtime_error("no Vulkan physical devices");
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    PhysicalDeviceChoice best{};
    bool found = false;
    bool found_discrete = false;

    for (VkPhysicalDevice device : devices) {
        u32 queue_family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(queue_family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, families.data());

        for (u32 i = 0; i < queue_family_count; ++i) {
            bool graphics = (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
            bool present = vkGetPhysicalDeviceWin32PresentationSupportKHR(device, i) == VK_TRUE;
            if (!graphics || !present) {
                continue;
            }

            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(device, &props);
            bool discrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;

            if (!found || (discrete && !found_discrete)) {
                best = {device, i};
                found = true;
                found_discrete = discrete;
                AETHER_LOG_INFO("Vulkan", "Considering adapter: %s (discrete=%d)", props.deviceName, discrete);
            }
            break;
        }
    }

    if (!found) {
        AETHER_LOG_FATAL("Vulkan", "No physical device with a graphics+present-capable queue family found");
        throw std::runtime_error("no suitable Vulkan physical device");
    }
    return best;
}

VkDevice CreateLogicalDevice(VkPhysicalDevice physical_device, u32 queue_family_index) {
    f32 priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = queue_family_index;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    VkPhysicalDeviceVulkan12Features features_12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    features_12.timelineSemaphore = VK_TRUE;

    const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pNext = &features_12;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = static_cast<u32>(std::size(extensions));
    device_info.ppEnabledExtensionNames = extensions;

    VkDevice device = VK_NULL_HANDLE;
    AETHER_VK_CHECK(vkCreateDevice(physical_device, &device_info, nullptr, &device));
    return device;
}

} // namespace

VulkanDevice::VulkanDevice(bool enable_debug_layer) {
    instance_ = CreateInstance(enable_debug_layer);

    PhysicalDeviceChoice choice = PickPhysicalDevice(instance_);
    physical_device_ = choice.device;
    queue_family_index_ = choice.queue_family_index;

    device_ = CreateLogicalDevice(physical_device_, queue_family_index_);
    vkGetDeviceQueue(device_, queue_family_index_, 0, &queue_);

    VkSemaphoreTypeCreateInfo timeline_type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    timeline_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timeline_type.initialValue = 0;

    VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    semaphore_info.pNext = &timeline_type;
    AETHER_VK_CHECK(vkCreateSemaphore(device_, &semaphore_info, nullptr, &timeline_semaphore_));

    AETHER_LOG_INFO("Vulkan", "Device initialized (queue family %u)", queue_family_index_);
}

VulkanDevice::~VulkanDevice() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        if (timeline_semaphore_ != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, timeline_semaphore_, nullptr);
        }
        vkDestroyDevice(device_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
    }
}

std::unique_ptr<ISwapChain> VulkanDevice::CreateSwapChain(void* native_window_handle, u32 width, u32 height,
                                                           u32 buffer_count) {
    return std::make_unique<VulkanSwapChain>(*this, native_window_handle, width, height, buffer_count);
}

std::unique_ptr<ICommandList> VulkanDevice::CreateCommandList() {
    return std::make_unique<VulkanCommandList>(*this);
}

u64 VulkanDevice::Submit(ICommandList& cmd, ISwapChain* wait_on_swap_chain) {
    std::vector<VkSemaphore> wait_semaphores;
    std::vector<VkPipelineStageFlags> wait_stages;
    std::vector<u64> wait_values;
    std::vector<VkSemaphore> signal_semaphores;
    std::vector<u64> signal_values;

    if (wait_on_swap_chain) {
        auto* vk_swap_chain = static_cast<VulkanSwapChain*>(wait_on_swap_chain->NativeHandle());
        wait_semaphores.push_back(vk_swap_chain->CurrentImageAvailableSemaphore());
        wait_stages.push_back(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        wait_values.push_back(0); // ignored by the driver for a binary semaphore entry

        signal_semaphores.push_back(vk_swap_chain->CurrentRenderFinishedSemaphore());
        signal_values.push_back(0); // ignored for a binary semaphore entry
    }

    u64 fence_value = next_fence_value_++;
    signal_semaphores.push_back(timeline_semaphore_);
    signal_values.push_back(fence_value);

    VkTimelineSemaphoreSubmitInfo timeline_info{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timeline_info.waitSemaphoreValueCount = static_cast<u32>(wait_values.size());
    timeline_info.pWaitSemaphoreValues = wait_values.data();
    timeline_info.signalSemaphoreValueCount = static_cast<u32>(signal_values.size());
    timeline_info.pSignalSemaphoreValues = signal_values.data();

    auto native_cmd = static_cast<VkCommandBuffer>(cmd.NativeHandle());

    VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit_info.pNext = &timeline_info;
    submit_info.waitSemaphoreCount = static_cast<u32>(wait_semaphores.size());
    submit_info.pWaitSemaphores = wait_semaphores.data();
    submit_info.pWaitDstStageMask = wait_stages.data();
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &native_cmd;
    submit_info.signalSemaphoreCount = static_cast<u32>(signal_semaphores.size());
    submit_info.pSignalSemaphores = signal_semaphores.data();

    AETHER_VK_CHECK(vkQueueSubmit(queue_, 1, &submit_info, VK_NULL_HANDLE));
    return fence_value;
}

void VulkanDevice::WaitForFence(u64 fence_value) {
    if (fence_value == 0 || IsFenceComplete(fence_value)) {
        return;
    }
    VkSemaphoreWaitInfo wait_info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wait_info.semaphoreCount = 1;
    wait_info.pSemaphores = &timeline_semaphore_;
    wait_info.pValues = &fence_value;
    AETHER_VK_CHECK(vkWaitSemaphores(device_, &wait_info, UINT64_MAX));
}

bool VulkanDevice::IsFenceComplete(u64 fence_value) const {
    u64 current_value = 0;
    vkGetSemaphoreCounterValue(device_, timeline_semaphore_, &current_value);
    return current_value >= fence_value;
}

TextureHandle VulkanDevice::RegisterTexture(VkImage image) {
    TextureHandle handle{static_cast<u32>(textures_.size())};
    textures_.push_back({image});
    return handle;
}

void VulkanDevice::UpdateTexture(TextureHandle handle, VkImage image) {
    textures_[handle.index] = {image};
}

// --------------------------------------------------------------------------

VulkanSwapChain::VulkanSwapChain(VulkanDevice& device, void* native_window_handle, u32 width, u32 height,
                                  u32 buffer_count)
    : device_(device), hwnd_(native_window_handle), requested_buffer_count_(buffer_count) {
    VkWin32SurfaceCreateInfoKHR surface_info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surface_info.hinstance = GetModuleHandleW(nullptr);
    surface_info.hwnd = static_cast<HWND>(hwnd_);
    AETHER_VK_CHECK(vkCreateWin32SurfaceKHR(device_.Instance(), &surface_info, nullptr, &surface_));

    VkBool32 present_supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(device_.PhysicalDevice(), device_.QueueFamilyIndex(), surface_,
                                          &present_supported);
    AETHER_ASSERT(present_supported == VK_TRUE);

    CreateSwapchainAndImages(width, height);
    CreateSyncObjects();
}

VulkanSwapChain::~VulkanSwapChain() {
    vkDeviceWaitIdle(device_.Handle());
    DestroySyncObjects();
    DestroySwapchainAndImages();
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(device_.Instance(), surface_, nullptr);
    }
}

void VulkanSwapChain::CreateSwapchainAndImages(u32 width, u32 height) {
    VkSurfaceCapabilitiesKHR capabilities{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device_.PhysicalDevice(), surface_, &capabilities);

    extent_.width = std::max(capabilities.minImageExtent.width, std::min(capabilities.maxImageExtent.width, width));
    extent_.height =
        std::max(capabilities.minImageExtent.height, std::min(capabilities.maxImageExtent.height, height));

    u32 image_count = std::max(requested_buffer_count_, capabilities.minImageCount);
    if (capabilities.maxImageCount > 0) {
        image_count = std::min(image_count, capabilities.maxImageCount);
    }

    VkSwapchainCreateInfoKHR swapchain_info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swapchain_info.surface = surface_;
    swapchain_info.minImageCount = image_count;
    swapchain_info.imageFormat = format_;
    swapchain_info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swapchain_info.imageExtent = extent_;
    swapchain_info.imageArrayLayers = 1;
    // TRANSFER_DST covers the abstract ICommandList::ClearRenderTarget path
    // (no render pass involved); COLOR_ATTACHMENT covers a real render-pass-
    // based draw (rhi_demo's triangle mode) — both are near-universally
    // supported for swapchain images, so requesting both up front avoids
    // needing two different swapchains depending on how the demo is run.
    swapchain_info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchain_info.preTransform = capabilities.currentTransform;
    swapchain_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchain_info.presentMode = VK_PRESENT_MODE_FIFO_KHR; // always supported; vsync-equivalent
    swapchain_info.clipped = VK_TRUE;

    AETHER_VK_CHECK(vkCreateSwapchainKHR(device_.Handle(), &swapchain_info, nullptr, &swapchain_));

    u32 actual_image_count = 0;
    vkGetSwapchainImagesKHR(device_.Handle(), swapchain_, &actual_image_count, nullptr);
    std::vector<VkImage> images(actual_image_count);
    vkGetSwapchainImagesKHR(device_.Handle(), swapchain_, &actual_image_count, images.data());

    handles_.clear();
    image_views_.clear();
    for (VkImage image : images) {
        handles_.push_back(device_.RegisterTexture(image));

        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = format_;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        AETHER_VK_CHECK(vkCreateImageView(device_.Handle(), &view_info, nullptr, &view));
        image_views_.push_back(view);
    }
}

void VulkanSwapChain::DestroySwapchainAndImages() {
    for (VkImageView view : image_views_) {
        vkDestroyImageView(device_.Handle(), view, nullptr);
    }
    image_views_.clear();
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_.Handle(), swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

void VulkanSwapChain::CreateSyncObjects() {
    // Indexed by a rotating frame-in-flight slot (frame_index_), not by
    // acquired image index: vkAcquireNextImageKHR needs a semaphore to
    // signal *before* it tells us which image we got, so the image index
    // can't select it. Sized to the image count for simplicity; correctness
    // here also leans on the caller fence-waiting (Device::WaitForFence)
    // before reusing a slot's resources, same pattern the D3D12 sandbox
    // already uses per-backbuffer-index — so a mismatched image/semaphore
    // pairing under contention isn't a hazard the way it would be in a more
    // aggressively pipelined implementation.
    usize slot_count = handles_.size();
    image_available_semaphores_.resize(slot_count);
    render_finished_semaphores_.resize(slot_count);

    VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (usize i = 0; i < slot_count; ++i) {
        AETHER_VK_CHECK(vkCreateSemaphore(device_.Handle(), &info, nullptr, &image_available_semaphores_[i]));
        AETHER_VK_CHECK(vkCreateSemaphore(device_.Handle(), &info, nullptr, &render_finished_semaphores_[i]));
    }
}

void VulkanSwapChain::DestroySyncObjects() {
    for (VkSemaphore s : image_available_semaphores_) {
        vkDestroySemaphore(device_.Handle(), s, nullptr);
    }
    for (VkSemaphore s : render_finished_semaphores_) {
        vkDestroySemaphore(device_.Handle(), s, nullptr);
    }
    image_available_semaphores_.clear();
    render_finished_semaphores_.clear();
}

void VulkanSwapChain::Resize(u32 width, u32 height) {
    vkDeviceWaitIdle(device_.Handle());
    DestroySyncObjects();
    DestroySwapchainAndImages();
    CreateSwapchainAndImages(width, height);
    CreateSyncObjects();
}

void VulkanSwapChain::AcquireNextImage() {
    frame_index_ = (frame_index_ + 1) % static_cast<u32>(image_available_semaphores_.size());
    VkResult result = vkAcquireNextImageKHR(device_.Handle(), swapchain_, UINT64_MAX,
                                             image_available_semaphores_[frame_index_], VK_NULL_HANDLE,
                                             &current_image_index_);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        Resize(extent_.width, extent_.height);
        AETHER_VK_CHECK(vkAcquireNextImageKHR(device_.Handle(), swapchain_, UINT64_MAX,
                                               image_available_semaphores_[frame_index_], VK_NULL_HANDLE,
                                               &current_image_index_));
    } else {
        AETHER_VK_CHECK(result);
    }
}

void VulkanSwapChain::Present(bool vsync) {
    (void)vsync; // FIFO present mode (always used here) already is the vsync-equivalent

    VkSemaphore wait_semaphore = render_finished_semaphores_[frame_index_];
    VkPresentInfoKHR present_info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &wait_semaphore;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &swapchain_;
    present_info.pImageIndices = &current_image_index_;

    VkResult result = vkQueuePresentKHR(device_.Queue(), &present_info);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        Resize(extent_.width, extent_.height);
    } else {
        AETHER_VK_CHECK(result);
    }
}

// --------------------------------------------------------------------------

VulkanCommandList::VulkanCommandList(VulkanDevice& device) : device_(device) {
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = device_.QueueFamilyIndex();
    AETHER_VK_CHECK(vkCreateCommandPool(device_.Handle(), &pool_info, nullptr, &command_pool_));

    VkCommandBufferAllocateInfo alloc_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc_info.commandPool = command_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;
    AETHER_VK_CHECK(vkAllocateCommandBuffers(device_.Handle(), &alloc_info, &command_buffer_));
}

VulkanCommandList::~VulkanCommandList() {
    if (command_pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_.Handle(), command_pool_, nullptr);
    }
}

void VulkanCommandList::Reset() {
    AETHER_VK_CHECK(vkResetCommandBuffer(command_buffer_, 0));
    VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    AETHER_VK_CHECK(vkBeginCommandBuffer(command_buffer_, &begin_info));
}

void VulkanCommandList::Close() {
    AETHER_VK_CHECK(vkEndCommandBuffer(command_buffer_));
}

void VulkanCommandList::TransitionTexture(TextureHandle texture, ResourceState before, ResourceState after) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = ToVkImageLayout(before);
    barrier.newLayout = ToVkImageLayout(after);
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = device_.GetTexture(texture);
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    // Conservative (TOP_OF_PIPE -> BOTTOM_OF_PIPE, ALL access) rather than
    // precisely scoped src/dst stage+access masks: correct but not
    // performance-tuned, matching the "correctness over optimization" scope
    // of this slice of the RHI. A precise barrier would derive stage/access
    // masks from `before`/`after` instead.
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;

    vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                          0, nullptr, 0, nullptr, 1, &barrier);
}

void VulkanCommandList::ClearRenderTarget(TextureHandle texture, const ClearColor& color) {
    VkClearColorValue clear_value{};
    clear_value.float32[0] = color.r;
    clear_value.float32[1] = color.g;
    clear_value.float32[2] = color.b;
    clear_value.float32[3] = color.a;

    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    // Requires the image to already be in ResourceState::RenderTarget (see
    // ToVkImageLayout's comment for why that maps to TRANSFER_DST here).
    vkCmdClearColorImage(command_buffer_, device_.GetTexture(texture), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          &clear_value, 1, &range);
}

void VulkanCommandList::SetViewportAndScissor(const Viewport& viewport, const Rect& scissor) {
    VkViewport vp{viewport.x, viewport.y, viewport.width, viewport.height, viewport.min_depth, viewport.max_depth};
    VkRect2D rect{{scissor.left, scissor.top},
                  {static_cast<u32>(scissor.right - scissor.left), static_cast<u32>(scissor.bottom - scissor.top)}};
    vkCmdSetViewport(command_buffer_, 0, 1, &vp);
    vkCmdSetScissor(command_buffer_, 0, 1, &rect);
}

} // namespace aether::gfx::rhi::vulkan_backend
