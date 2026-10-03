#include "aether/gfx/rhi/vulkan/vulkan_backend.h"

#include "aether/core/log.h"
#include "aether/gfx/shader_compiler.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#if !defined(_WIN32) && defined(AETHER_HAS_GLFW)
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#endif

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

#if defined(_WIN32)
    std::vector<const char*> extensions = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
#else
    // Elsewhere, every window-system surface extension the loader offers
    // (GLFW picks between X11 through xcb or Xlib, and Wayland, at run
    // time), and none at all on a machine with no window system - offscreen
    // swap chains don't need them.
    u32 available_count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &available_count, nullptr);
    std::vector<VkExtensionProperties> available(available_count);
    vkEnumerateInstanceExtensionProperties(nullptr, &available_count, available.data());
    static const char* const kWanted[] = {"VK_KHR_surface", "VK_KHR_xcb_surface", "VK_KHR_xlib_surface",
                                          "VK_KHR_wayland_surface", "VK_EXT_metal_surface"};
    std::vector<const char*> extensions;
    for (const char* wanted : kWanted) {
        for (const VkExtensionProperties& ext : available) {
            if (std::strcmp(ext.extensionName, wanted) == 0) {
                extensions.push_back(wanted);
                break;
            }
        }
    }
    if (extensions.size() == 1) {
        extensions.clear(); // VK_KHR_surface alone is of no use
    }
#endif

    VkInstanceCreateInfo create_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = static_cast<u32>(extensions.size());
    create_info.ppEnabledExtensionNames = extensions.data();

    VkInstance instance = VK_NULL_HANDLE;
    AETHER_VK_CHECK(vkCreateInstance(&create_info, nullptr, &instance));
    return instance;
}

struct PhysicalDeviceChoice {
    VkPhysicalDevice device = VK_NULL_HANDLE;
    u32 graphics_family = 0;
    u32 compute_family = 0;
    u32 compute_queue_index = 0; // index within compute_family's queues
    bool compute_is_dedicated_family = false;
    bool compute_is_separate_queue = false; // false only when truly the same queue as graphics
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
#if defined(_WIN32)
            bool present = vkGetPhysicalDeviceWin32PresentationSupportKHR(device, i) == VK_TRUE;
#else
            // Checked against the real surface when a window swap chain is
            // created; graphics queues present on every desktop driver.
            bool present = true;
#endif
            if (!graphics || !present) {
                continue;
            }

            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(device, &props);
            bool discrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;

            if (!found || (discrete && !found_discrete)) {
                best = {};
                best.device = device;
                best.graphics_family = i;
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

    // Look for a genuinely dedicated async-compute queue family (COMPUTE
    // without GRAPHICS) — this is what makes "async" compute real hardware
    // concurrency rather than just an API shape. Fall back to a second queue
    // instance in the graphics family, then to the same queue as graphics.
    u32 queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(best.device, &queue_family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(best.device, &queue_family_count, families.data());

    for (u32 i = 0; i < queue_family_count; ++i) {
        bool compute = (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
        bool graphics = (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
        if (compute && !graphics) {
            best.compute_family = i;
            best.compute_queue_index = 0;
            best.compute_is_dedicated_family = true;
            best.compute_is_separate_queue = true;
            AETHER_LOG_INFO("Vulkan", "Found a dedicated async-compute queue family (index %u)", i);
            return best;
        }
    }

    if (families[best.graphics_family].queueCount >= 2) {
        best.compute_family = best.graphics_family;
        best.compute_queue_index = 1;
        best.compute_is_separate_queue = true;
        AETHER_LOG_INFO("Vulkan",
                         "No dedicated compute family; using a second queue instance in the graphics family");
        return best;
    }

    best.compute_family = best.graphics_family;
    best.compute_queue_index = 0;
    best.compute_is_separate_queue = false;
    AETHER_LOG_WARN("Vulkan", "No spare queue for async compute on this device; compute will serialize with "
                              "graphics on the same queue");
    return best;
}

VkDevice CreateLogicalDevice(VkPhysicalDevice physical_device, const PhysicalDeviceChoice& choice) {
    f32 priorities[2] = {1.0f, 1.0f};
    std::vector<VkDeviceQueueCreateInfo> queue_infos;

    VkDeviceQueueCreateInfo graphics_queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    graphics_queue_info.queueFamilyIndex = choice.graphics_family;
    // Same family, distinct queue instance for compute: request both queues
    // up front (queueCount=2); vkGetDeviceQueue then fetches index 1 for
    // compute separately.
    bool shares_family_with_second_queue =
        choice.compute_is_separate_queue && !choice.compute_is_dedicated_family;
    graphics_queue_info.queueCount = shares_family_with_second_queue ? 2 : 1;
    graphics_queue_info.pQueuePriorities = priorities;
    queue_infos.push_back(graphics_queue_info);

    if (choice.compute_is_dedicated_family) {
        VkDeviceQueueCreateInfo compute_queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        compute_queue_info.queueFamilyIndex = choice.compute_family;
        compute_queue_info.queueCount = 1;
        compute_queue_info.pQueuePriorities = priorities;
        queue_infos.push_back(compute_queue_info);
    }

    VkPhysicalDeviceVulkan12Features features_12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    features_12.timelineSemaphore = VK_TRUE;

    const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pNext = &features_12;
    device_info.queueCreateInfoCount = static_cast<u32>(queue_infos.size());
    device_info.pQueueCreateInfos = queue_infos.data();
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
    queue_family_index_ = choice.graphics_family;
    compute_queue_family_index_ = choice.compute_family;

    device_ = CreateLogicalDevice(physical_device_, choice);
    vkGetDeviceQueue(device_, queue_family_index_, 0, &queue_);
    if (choice.compute_is_separate_queue) {
        vkGetDeviceQueue(device_, compute_queue_family_index_, choice.compute_queue_index, &compute_queue_);
    } else {
        compute_queue_ = queue_; // serialized fallback — see PickPhysicalDevice's warning
    }

    VkSemaphoreTypeCreateInfo timeline_type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    timeline_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timeline_type.initialValue = 0;

    VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    semaphore_info.pNext = &timeline_type;
    AETHER_VK_CHECK(vkCreateSemaphore(device_, &semaphore_info, nullptr, &timeline_semaphore_));
    AETHER_VK_CHECK(vkCreateSemaphore(device_, &semaphore_info, nullptr, &compute_timeline_semaphore_));

    AETHER_LOG_INFO("Vulkan", "Device initialized (graphics queue family %u, compute queue family %u)",
                     queue_family_index_, compute_queue_family_index_);

    CreateBindlessTextureInfrastructure();
}

VulkanDevice::~VulkanDevice() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
        for (const PipelineRecord& record : pipelines_) {
            if (record.pipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, record.pipeline, nullptr);
            }
            if (record.layout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device_, record.layout, nullptr);
            }
        }
        if (timeline_semaphore_ != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, timeline_semaphore_, nullptr);
        }
        if (compute_timeline_semaphore_ != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, compute_timeline_semaphore_, nullptr);
        }
        for (const BufferRecord& buffer : buffers_) {
            vkDestroyBuffer(device_, buffer.buffer, nullptr);
            vkFreeMemory(device_, buffer.memory, nullptr);
        }
        auto destroy_texture_record = [this](const SampledTextureRecord& record) {
            vkDestroyImageView(device_, record.view, nullptr);
            vkDestroyImage(device_, record.image, nullptr);
            vkFreeMemory(device_, record.memory, nullptr);
        };
        for (const SampledTextureRecord& texture : sampled_textures_) {
            destroy_texture_record(texture);
        }
        destroy_texture_record(dummy_texture_);
        if (bindless_pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, bindless_pool_, nullptr);
        }
        if (bindless_texture_set_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_, bindless_texture_set_layout_, nullptr);
        }
        if (bindless_sampler_set_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_, bindless_sampler_set_layout_, nullptr);
        }
        if (bindless_sampler_ != VK_NULL_HANDLE) {
            vkDestroySampler(device_, bindless_sampler_, nullptr);
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
    return std::make_unique<VulkanCommandList>(*this, queue_family_index_);
}

std::unique_ptr<ICommandList> VulkanDevice::CreateComputeCommandList() {
    return std::make_unique<VulkanCommandList>(*this, compute_queue_family_index_);
}

u64 VulkanDevice::SubmitCompute(ICommandList& cmd) {
    auto native_cmd = static_cast<VkCommandBuffer>(cmd.NativeHandle());

    std::vector<VkSemaphore> wait_semaphores;
    std::vector<VkPipelineStageFlags> wait_stages;
    std::vector<u64> wait_values;

    if (pending_compute_wait_.valid) {
        wait_semaphores.push_back(pending_compute_wait_.semaphore);
        wait_stages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        wait_values.push_back(pending_compute_wait_.value);
        pending_compute_wait_ = {};
    }

    u64 fence_value = next_compute_fence_value_++;
    VkSemaphore signal_semaphores[] = {compute_timeline_semaphore_};
    u64 signal_values[] = {fence_value};

    VkTimelineSemaphoreSubmitInfo timeline_info{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timeline_info.waitSemaphoreValueCount = static_cast<u32>(wait_values.size());
    timeline_info.pWaitSemaphoreValues = wait_values.data();
    timeline_info.signalSemaphoreValueCount = 1;
    timeline_info.pSignalSemaphoreValues = signal_values;

    VkSubmitInfo submit_info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit_info.pNext = &timeline_info;
    submit_info.waitSemaphoreCount = static_cast<u32>(wait_semaphores.size());
    submit_info.pWaitSemaphores = wait_semaphores.data();
    submit_info.pWaitDstStageMask = wait_stages.data();
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &native_cmd;
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = signal_semaphores;

    AETHER_VK_CHECK(vkQueueSubmit(compute_queue_, 1, &submit_info, VK_NULL_HANDLE));
    return fence_value;
}

void VulkanDevice::WaitForComputeFence(u64 fence_value) {
    if (fence_value == 0 || IsComputeFenceComplete(fence_value)) {
        return;
    }
    VkSemaphoreWaitInfo wait_info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wait_info.semaphoreCount = 1;
    wait_info.pSemaphores = &compute_timeline_semaphore_;
    wait_info.pValues = &fence_value;
    AETHER_VK_CHECK(vkWaitSemaphores(device_, &wait_info, UINT64_MAX));
}

bool VulkanDevice::IsComputeFenceComplete(u64 fence_value) const {
    u64 current_value = 0;
    vkGetSemaphoreCounterValue(device_, compute_timeline_semaphore_, &current_value);
    return current_value >= fence_value;
}

void VulkanDevice::ComputeQueueWaitOnGraphics(u64 graphics_fence_value) {
    pending_compute_wait_ = {true, timeline_semaphore_, graphics_fence_value};
}

void VulkanDevice::GraphicsQueueWaitOnCompute(u64 compute_fence_value) {
    pending_graphics_wait_ = {true, compute_timeline_semaphore_, compute_fence_value};
}

namespace {
VkShaderModule CreateShaderModuleFromBytecode(VkDevice device, const gfx::ShaderBytecode& bytecode) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytecode.Size();
    info.pCode = reinterpret_cast<const u32*>(bytecode.Data());
    VkShaderModule module = VK_NULL_HANDLE;
    AETHER_VK_CHECK(vkCreateShaderModule(device, &info, nullptr, &module));
    return module;
}

u32 FindMemoryType(VkPhysicalDevice physical_device, u32 type_bits, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_props{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);
    for (u32 i = 0; i < mem_props.memoryTypeCount; ++i) {
        bool type_ok = (type_bits & (1u << i)) != 0;
        bool props_ok = (mem_props.memoryTypes[i].propertyFlags & properties) == properties;
        if (type_ok && props_ok) {
            return i;
        }
    }
    AETHER_LOG_FATAL("Vulkan", "No suitable Vulkan memory type found");
    throw std::runtime_error("no suitable Vulkan memory type");
}

// Host-visible + host-coherent — see BufferRecord's comment for why this is
// the right trade-off for vertex/index data at this RHI's demo scale (and
// also reused for CreateTexture's staging buffer, which is genuinely
// short-lived regardless).
BufferRecord CreateHostVisibleBuffer(VkDevice device, VkPhysicalDevice physical_device, VkDeviceSize size,
                                      VkBufferUsageFlags usage, const void* data) {
    BufferRecord result;
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    AETHER_VK_CHECK(vkCreateBuffer(device, &buffer_info, nullptr, &result.buffer));

    VkMemoryRequirements mem_reqs{};
    vkGetBufferMemoryRequirements(device, result.buffer, &mem_reqs);
    VkMemoryAllocateInfo alloc_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = FindMemoryType(
        physical_device, mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    AETHER_VK_CHECK(vkAllocateMemory(device, &alloc_info, nullptr, &result.memory));
    AETHER_VK_CHECK(vkBindBufferMemory(device, result.buffer, result.memory, 0));

    void* mapped = nullptr;
    AETHER_VK_CHECK(vkMapMemory(device, result.memory, 0, size, 0, &mapped));
    std::memcpy(mapped, data, static_cast<usize>(size));
    vkUnmapMemory(device, result.memory);
    return result;
}
} // namespace

BufferHandle VulkanDevice::CreateBufferInternal(const void* data, u64 size_bytes) {
    // Both usages on every buffer regardless of which CreateXBuffer call
    // made it — simpler than tracking per-buffer usage flags, and harmless:
    // an unused usage bit on a host-visible buffer costs nothing at this
    // demo's scale.
    BufferRecord record = CreateHostVisibleBuffer(device_, physical_device_, size_bytes,
                                                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                                   data);
    buffers_.push_back(record);
    return BufferHandle{static_cast<u32>(buffers_.size() - 1)};
}

BufferHandle VulkanDevice::CreateVertexBuffer(const void* data, u64 size_bytes) {
    return CreateBufferInternal(data, size_bytes);
}

BufferHandle VulkanDevice::CreateIndexBuffer(const void* data, u64 size_bytes, IndexFormat) {
    return CreateBufferInternal(data, size_bytes);
}

SampledTextureRecord VulkanDevice::UploadTextureRecord(u32 width, u32 height, const u8* rgba8_pixels) {
    VkDeviceSize image_size = static_cast<VkDeviceSize>(width) * height * 4;
    BufferRecord staging =
        CreateHostVisibleBuffer(device_, physical_device_, image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rgba8_pixels);

    SampledTextureRecord record;
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    AETHER_VK_CHECK(vkCreateImage(device_, &image_info, nullptr, &record.image));

    VkMemoryRequirements mem_reqs{};
    vkGetImageMemoryRequirements(device_, record.image, &mem_reqs);
    VkMemoryAllocateInfo alloc_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex =
        FindMemoryType(physical_device_, mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    AETHER_VK_CHECK(vkAllocateMemory(device_, &alloc_info, nullptr, &record.memory));
    AETHER_VK_CHECK(vkBindImageMemory(device_, record.image, record.memory, 0));

    std::unique_ptr<ICommandList> cmd = CreateCommandList();
    cmd->Reset();
    auto native_cmd = static_cast<VkCommandBuffer>(cmd->NativeHandle());

    VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = record.image;
    to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_dst.srcAccessMask = 0;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(native_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                          0, nullptr, 1, &to_dst);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(native_cmd, staging.buffer, record.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier to_shader_read{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_shader_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_shader_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_shader_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_shader_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_shader_read.image = record.image;
    to_shader_read.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_shader_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_shader_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(native_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                          nullptr, 0, nullptr, 1, &to_shader_read);

    cmd->Close();
    WaitForFence(Submit(*cmd, nullptr));

    vkDestroyBuffer(device_, staging.buffer, nullptr);
    vkFreeMemory(device_, staging.memory, nullptr);

    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = record.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    AETHER_VK_CHECK(vkCreateImageView(device_, &view_info, nullptr, &record.view));

    return record;
}

SampledTextureHandle VulkanDevice::CreateTexture(u32 width, u32 height, const u8* rgba8_pixels) {
    SampledTextureRecord record = UploadTextureRecord(width, height, rgba8_pixels);
    u32 index = static_cast<u32>(sampled_textures_.size());
    sampled_textures_.push_back(record);

    VkDescriptorImageInfo image_desc{};
    image_desc.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image_desc.imageView = record.view;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = bindless_texture_set_;
    write.dstBinding = 0;
    write.dstArrayElement = index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &image_desc;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    return SampledTextureHandle{index};
}

void VulkanDevice::CreateBindlessTextureInfrastructure() {
    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    AETHER_VK_CHECK(vkCreateSampler(device_, &sampler_info, nullptr, &bindless_sampler_));

    // Two separate set layouts (SAMPLED_IMAGE array + standalone SAMPLER),
    // not one combined-image-sampler binding — see this file's header
    // comment on BindlessTextureSetLayout for why.
    VkDescriptorSetLayoutBinding texture_binding{};
    texture_binding.binding = 0;
    texture_binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    texture_binding.descriptorCount = kMaxBindlessTextures;
    texture_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo texture_layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    texture_layout_info.bindingCount = 1;
    texture_layout_info.pBindings = &texture_binding;
    AETHER_VK_CHECK(vkCreateDescriptorSetLayout(device_, &texture_layout_info, nullptr, &bindless_texture_set_layout_));

    VkDescriptorSetLayoutBinding sampler_binding{};
    sampler_binding.binding = 0;
    sampler_binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    sampler_binding.descriptorCount = 1;
    sampler_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo sampler_layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sampler_layout_info.bindingCount = 1;
    sampler_layout_info.pBindings = &sampler_binding;
    AETHER_VK_CHECK(vkCreateDescriptorSetLayout(device_, &sampler_layout_info, nullptr, &bindless_sampler_set_layout_));

    VkDescriptorPoolSize pool_sizes[2] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxBindlessTextures},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 1},
    };
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 2;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = pool_sizes;
    AETHER_VK_CHECK(vkCreateDescriptorPool(device_, &pool_info, nullptr, &bindless_pool_));

    VkDescriptorSetLayout set_layouts[2] = {bindless_texture_set_layout_, bindless_sampler_set_layout_};
    VkDescriptorSet sets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDescriptorSetAllocateInfo set_alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    set_alloc.descriptorPool = bindless_pool_;
    set_alloc.descriptorSetCount = 2;
    set_alloc.pSetLayouts = set_layouts;
    AETHER_VK_CHECK(vkAllocateDescriptorSets(device_, &set_alloc, sets));
    bindless_texture_set_ = sets[0];
    bindless_sampler_set_ = sets[1];

    VkDescriptorImageInfo sampler_desc{};
    sampler_desc.sampler = bindless_sampler_;
    VkWriteDescriptorSet sampler_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    sampler_write.dstSet = bindless_sampler_set_;
    sampler_write.dstBinding = 0;
    sampler_write.descriptorCount = 1;
    sampler_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    sampler_write.pImageInfo = &sampler_desc;
    vkUpdateDescriptorSets(device_, 1, &sampler_write, 0, nullptr);

    // A 1x1 white dummy texture, duplicated into every bindless texture
    // slot up front — see SampledTextureRecord dummy_texture_'s header
    // comment for why every slot needs a valid descriptor before any
    // enable_bindless_textures pipeline draws, not just the indices a real
    // CreateTexture() call has claimed so far.
    const u8 white_pixel[4] = {255, 255, 255, 255};
    dummy_texture_ = UploadTextureRecord(1, 1, white_pixel);

    std::vector<VkDescriptorImageInfo> image_infos(kMaxBindlessTextures);
    for (VkDescriptorImageInfo& info : image_infos) {
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        info.imageView = dummy_texture_.view;
    }
    VkWriteDescriptorSet texture_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    texture_write.dstSet = bindless_texture_set_;
    texture_write.dstBinding = 0;
    texture_write.dstArrayElement = 0;
    texture_write.descriptorCount = kMaxBindlessTextures;
    texture_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    texture_write.pImageInfo = image_infos.data();
    vkUpdateDescriptorSets(device_, 1, &texture_write, 0, nullptr);
}

PipelineHandle VulkanDevice::CreatePipeline(const PipelineDesc& desc, ISwapChain& swap_chain) {
    auto& vk_swap = static_cast<VulkanSwapChain&>(swap_chain);

    PipelineRecord record;

    if (desc.enable_bindless_textures) {
        AETHER_ASSERT(desc.push_constant_size_bytes > 0);
    }

    VkPushConstantRange push_range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                    desc.push_constant_size_bytes};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    if (desc.push_constant_size_bytes > 0) {
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push_range;
    }
    VkDescriptorSetLayout bindless_set_layouts[2] = {bindless_texture_set_layout_, bindless_sampler_set_layout_};
    if (desc.enable_bindless_textures) {
        layout_info.setLayoutCount = 2;
        layout_info.pSetLayouts = bindless_set_layouts;
    }
    AETHER_VK_CHECK(vkCreatePipelineLayout(device_, &layout_info, nullptr, &record.layout));

    gfx::ShaderBytecode vs_spirv =
        gfx::CompileHLSLToSPIRV(desc.hlsl_source, desc.vs_entry.c_str(), "vs_6_0", "rhi_pipeline_vs");
    gfx::ShaderBytecode ps_spirv =
        gfx::CompileHLSLToSPIRV(desc.hlsl_source, desc.ps_entry.c_str(), "ps_6_0", "rhi_pipeline_ps");
    VkShaderModule vs_module = CreateShaderModuleFromBytecode(device_, vs_spirv);
    VkShaderModule ps_module = CreateShaderModuleFromBytecode(device_, ps_spirv);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs_module;
    stages[0].pName = desc.vs_entry.c_str();
    stages[1] = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps_module;
    stages[1].pName = desc.ps_entry.c_str();

    // The one fixed vertex layout PipelineDesc::use_vertex_buffer means —
    // see its comment for why this isn't a general attribute-list API. Must
    // match the D3D12 backend's D3D12_INPUT_ELEMENT_DESC layout exactly.
    VkVertexInputBindingDescription vertex_binding{0, 20 /* sizeof(float3 pos) + sizeof(float2 uv) */,
                                                    VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription vertex_attributes[2] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, 12},
    };

    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (desc.use_vertex_buffer) {
        vertex_input.vertexBindingDescriptionCount = 1;
        vertex_input.pVertexBindingDescriptions = &vertex_binding;
        vertex_input.vertexAttributeDescriptionCount = 2;
        vertex_input.pVertexAttributeDescriptions = vertex_attributes;
    }

    VkPipelineInputAssemblyStateCreateInfo input_assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = desc.cull_back_face ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Standard non-premultiplied alpha blending — src.rgb*src.a +
    // dst.rgb*(1-src.a) for color, src.a straight through for alpha (the
    // usual "output alpha doesn't itself get blended" convention, matching
    // the D3D12 backend's equivalent blend state exactly).
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend_attachment.blendEnable = desc.enable_blending ? VK_TRUE : VK_FALSE;
    if (desc.enable_blending) {
        blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
        blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }

    VkPipelineColorBlendStateCreateInfo blend_state{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend_state.attachmentCount = 1;
    blend_state.pAttachments = &blend_attachment;

    // The render pass's depth attachment (see CreateDefaultRenderPass) is
    // always present; this is what actually opts a given pipeline in or out
    // of testing/writing against it.
    VkPipelineDepthStencilStateCreateInfo depth_stencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth_stencil.depthTestEnable = desc.depth_test ? VK_TRUE : VK_FALSE;
    depth_stencil.depthWriteEnable = desc.depth_test ? VK_TRUE : VK_FALSE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates = dynamic_states;

    VkGraphicsPipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterizer;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend_state;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = record.layout;
    pipeline_info.renderPass = vk_swap.DefaultRenderPass();
    pipeline_info.subpass = 0;

    VkResult pipeline_result =
        vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &record.pipeline);

    vkDestroyShaderModule(device_, vs_module, nullptr);
    vkDestroyShaderModule(device_, ps_module, nullptr);

    AETHER_VK_CHECK(pipeline_result);

    pipelines_.push_back(record);
    return PipelineHandle{static_cast<u32>(pipelines_.size() - 1)};
}

u64 VulkanDevice::Submit(ICommandList& cmd, ISwapChain* wait_on_swap_chain) {
    std::vector<VkSemaphore> wait_semaphores;
    std::vector<VkPipelineStageFlags> wait_stages;
    std::vector<u64> wait_values;
    std::vector<VkSemaphore> signal_semaphores;
    std::vector<u64> signal_values;

    if (wait_on_swap_chain &&
        !static_cast<VulkanSwapChain*>(wait_on_swap_chain->NativeHandle())->IsOffscreen()) {
        auto* vk_swap_chain = static_cast<VulkanSwapChain*>(wait_on_swap_chain->NativeHandle());
        wait_semaphores.push_back(vk_swap_chain->CurrentImageAvailableSemaphore());
        wait_stages.push_back(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        wait_values.push_back(0); // ignored by the driver for a binary semaphore entry

        signal_semaphores.push_back(vk_swap_chain->CurrentRenderFinishedSemaphore());
        signal_values.push_back(0); // ignored for a binary semaphore entry
    }

    if (pending_graphics_wait_.valid) {
        wait_semaphores.push_back(pending_graphics_wait_.semaphore);
        wait_stages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        wait_values.push_back(pending_graphics_wait_.value);
        pending_graphics_wait_ = {};
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
    : device_(device), window_(native_window_handle), requested_buffer_count_(buffer_count) {
    offscreen_ = window_ == nullptr;
    if (!offscreen_) {
#if defined(_WIN32)
        VkWin32SurfaceCreateInfoKHR surface_info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        surface_info.hinstance = GetModuleHandleW(nullptr);
        surface_info.hwnd = static_cast<HWND>(window_);
        AETHER_VK_CHECK(vkCreateWin32SurfaceKHR(device_.Instance(), &surface_info, nullptr, &surface_));
#elif defined(AETHER_HAS_GLFW)
        AETHER_VK_CHECK(
            glfwCreateWindowSurface(device_.Instance(), static_cast<GLFWwindow*>(window_), nullptr, &surface_));
#else
        AETHER_LOG_FATAL("Vulkan", "Window swap chains need the GLFW window backend (AETHER_BUILD_WINDOWING)");
        throw std::runtime_error("no window surface support");
#endif

        VkBool32 present_supported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device_.PhysicalDevice(), device_.QueueFamilyIndex(), surface_,
                                              &present_supported);
        AETHER_ASSERT(present_supported == VK_TRUE);
    }

    CreateDefaultRenderPass();
    CreateSwapchainAndImages(width, height);
    CreateSyncObjects();
}

VulkanSwapChain::~VulkanSwapChain() {
    vkDeviceWaitIdle(device_.Handle());
    DestroySyncObjects();
    DestroySwapchainAndImages();
    if (default_render_pass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device_.Handle(), default_render_pass_, nullptr);
    }
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(device_.Instance(), surface_, nullptr);
    }
}

// One render pass for this swapchain's format (stable across resize — only
// the framebuffers, tied to specific image views, need recreating there).
// Every frame clears via loadOp=CLEAR and discards prior contents, so
// initialLayout can always be UNDEFINED regardless of what layout the image
// was actually left in — matching rhi_demo's triangle/cube modes' own render
// passes (this is that same pattern, now living in the engine instead of
// duplicated per-demo).
void VulkanSwapChain::CreateDefaultRenderPass() {
    VkAttachmentDescription color_attachment{};
    color_attachment.format = format_;
    color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    // Always present (see depth_image_'s header comment) so any pipeline —
    // whether or not PipelineDesc::depth_test is set — can run in this one
    // render pass; loadOp=CLEAR every frame regardless of whether the
    // current draw actually depth-tests is harmless (same "always clear,
    // discard prior contents" convention the color attachment already uses).
    VkAttachmentDescription depth_attachment{};
    depth_attachment.format = kDepthFormat;
    depth_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth_attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription attachments[2] = {color_attachment, depth_attachment};

    VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    subpass.pDepthStencilAttachment = &depth_ref;

    // Synchronizes subpass 0 with the swapchain's image-available semaphore,
    // which VulkanDevice::Submit waits on at COLOR_ATTACHMENT_OUTPUT — the
    // depth attachment's own read/write hazard is confined entirely within
    // this subpass (no other subpass or resource touches it), so it needs
    // no separate dependency entry.
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp_info.attachmentCount = 2;
    rp_info.pAttachments = attachments;
    rp_info.subpassCount = 1;
    rp_info.pSubpasses = &subpass;
    rp_info.dependencyCount = 1;
    rp_info.pDependencies = &dependency;
    AETHER_VK_CHECK(vkCreateRenderPass(device_.Handle(), &rp_info, nullptr, &default_render_pass_));
}

void VulkanSwapChain::CreateDepthResources() {
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = kDepthFormat;
    image_info.extent = {extent_.width, extent_.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    AETHER_VK_CHECK(vkCreateImage(device_.Handle(), &image_info, nullptr, &depth_image_));

    VkMemoryRequirements mem_reqs{};
    vkGetImageMemoryRequirements(device_.Handle(), depth_image_, &mem_reqs);
    VkMemoryAllocateInfo alloc_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex =
        FindMemoryType(device_.PhysicalDevice(), mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    AETHER_VK_CHECK(vkAllocateMemory(device_.Handle(), &alloc_info, nullptr, &depth_memory_));
    AETHER_VK_CHECK(vkBindImageMemory(device_.Handle(), depth_image_, depth_memory_, 0));

    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = depth_image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = kDepthFormat;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    AETHER_VK_CHECK(vkCreateImageView(device_.Handle(), &view_info, nullptr, &depth_image_view_));
}

void VulkanSwapChain::DestroyDepthResources() {
    if (depth_image_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_.Handle(), depth_image_view_, nullptr);
        depth_image_view_ = VK_NULL_HANDLE;
    }
    if (depth_image_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_.Handle(), depth_image_, nullptr);
        depth_image_ = VK_NULL_HANDLE;
    }
    if (depth_memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_.Handle(), depth_memory_, nullptr);
        depth_memory_ = VK_NULL_HANDLE;
    }
}

void VulkanSwapChain::CreateFramebuffers() {
    framebuffers_.resize(image_views_.size());
    for (usize i = 0; i < image_views_.size(); ++i) {
        VkImageView attachments[] = {image_views_[i], depth_image_view_};
        VkFramebufferCreateInfo fb_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb_info.renderPass = default_render_pass_;
        fb_info.attachmentCount = 2;
        fb_info.pAttachments = attachments;
        fb_info.width = extent_.width;
        fb_info.height = extent_.height;
        fb_info.layers = 1;
        AETHER_VK_CHECK(vkCreateFramebuffer(device_.Handle(), &fb_info, nullptr, &framebuffers_[i]));
    }
}

void VulkanSwapChain::DestroyFramebuffers() {
    for (VkFramebuffer fb : framebuffers_) {
        vkDestroyFramebuffer(device_.Handle(), fb, nullptr);
    }
    framebuffers_.clear();
}

std::vector<VkImage> VulkanSwapChain::CreateOffscreenImages(u32 width, u32 height) {
    extent_ = {std::max(width, 1u), std::max(height, 1u)};
    can_read_back_ = true;
    std::vector<VkImage> images(std::max(requested_buffer_count_, 1u));
    offscreen_memory_.resize(images.size());
    for (usize i = 0; i < images.size(); ++i) {
        VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = format_;
        image_info.extent = {extent_.width, extent_.height, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        AETHER_VK_CHECK(vkCreateImage(device_.Handle(), &image_info, nullptr, &images[i]));

        VkMemoryRequirements mem_reqs{};
        vkGetImageMemoryRequirements(device_.Handle(), images[i], &mem_reqs);
        VkMemoryAllocateInfo alloc_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc_info.allocationSize = mem_reqs.size;
        alloc_info.memoryTypeIndex =
            FindMemoryType(device_.PhysicalDevice(), mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        AETHER_VK_CHECK(vkAllocateMemory(device_.Handle(), &alloc_info, nullptr, &offscreen_memory_[i]));
        AETHER_VK_CHECK(vkBindImageMemory(device_.Handle(), images[i], offscreen_memory_[i], 0));
    }
    offscreen_images_ = images;
    return images;
}

void VulkanSwapChain::CreateSwapchainAndImages(u32 width, u32 height) {
    if (offscreen_) {
        std::vector<VkImage> images = CreateOffscreenImages(width, height);
        const bool first = handles_.empty();
        image_views_.clear();
        for (usize i = 0; i < images.size(); ++i) {
            // Keep the device's texture handles stable across resizes.
            if (first) {
                handles_.push_back(device_.RegisterTexture(images[i]));
            } else {
                device_.UpdateTexture(handles_[i], images[i]);
            }
            VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view_info.image = images[i];
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = format_;
            view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageView view = VK_NULL_HANDLE;
            AETHER_VK_CHECK(vkCreateImageView(device_.Handle(), &view_info, nullptr, &view));
            image_views_.push_back(view);
        }
        current_image_index_ = 0;
        CreateDepthResources();
        CreateFramebuffers();
        return;
    }

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
    // TRANSFER_SRC, where the surface allows it, lets ReadBack copy the
    // back buffer out (editor screenshots).
    can_read_back_ = (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    if (can_read_back_) {
        swapchain_info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
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

    CreateDepthResources();
    CreateFramebuffers();
}

void VulkanSwapChain::DestroySwapchainAndImages() {
    DestroyFramebuffers();
    DestroyDepthResources();
    for (VkImageView view : image_views_) {
        vkDestroyImageView(device_.Handle(), view, nullptr);
    }
    image_views_.clear();
    for (VkImage image : offscreen_images_) {
        vkDestroyImage(device_.Handle(), image, nullptr);
    }
    for (VkDeviceMemory memory : offscreen_memory_) {
        vkFreeMemory(device_.Handle(), memory, nullptr);
    }
    offscreen_images_.clear();
    offscreen_memory_.clear();
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
    if (offscreen_) {
        current_image_index_ = (current_image_index_ + 1) % static_cast<u32>(handles_.size());
        return;
    }
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
    if (offscreen_) {
        return; // nothing to show; the image stays readable until the next AcquireNextImage
    }

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

bool VulkanSwapChain::ReadBack(std::vector<u8>& rgba8) {
    if (!can_read_back_ || handles_.empty()) {
        return false;
    }
    VkDevice device = device_.Handle();
    AETHER_VK_CHECK(vkQueueWaitIdle(device_.Queue()));

    const VkDeviceSize size = static_cast<VkDeviceSize>(extent_.width) * extent_.height * 4;
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    AETHER_VK_CHECK(vkCreateBuffer(device, &buffer_info, nullptr, &buffer));
    VkMemoryRequirements mem_reqs{};
    vkGetBufferMemoryRequirements(device, buffer, &mem_reqs);
    VkMemoryAllocateInfo alloc_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex =
        FindMemoryType(device_.PhysicalDevice(), mem_reqs.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    AETHER_VK_CHECK(vkAllocateMemory(device, &alloc_info, nullptr, &memory));
    AETHER_VK_CHECK(vkBindBufferMemory(device, buffer, memory, 0));

    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = device_.QueueFamilyIndex();
    VkCommandPool pool = VK_NULL_HANDLE;
    AETHER_VK_CHECK(vkCreateCommandPool(device, &pool_info, nullptr, &pool));
    VkCommandBufferAllocateInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmd_info.commandPool = pool;
    cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_info.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    AETHER_VK_CHECK(vkAllocateCommandBuffers(device, &cmd_info, &cmd));

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    AETHER_VK_CHECK(vkBeginCommandBuffer(cmd, &begin));
    // Both ways of drawing a frame (the default render pass, and
    // TransitionTexture to Present) leave the image in PRESENT_SRC.
    VkImage image = device_.GetTexture(handles_[current_image_index_]);
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {extent_.width, extent_.height, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);
    AETHER_VK_CHECK(vkEndCommandBuffer(cmd));

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    AETHER_VK_CHECK(vkQueueSubmit(device_.Queue(), 1, &submit, VK_NULL_HANDLE));
    AETHER_VK_CHECK(vkQueueWaitIdle(device_.Queue()));

    void* mapped = nullptr;
    AETHER_VK_CHECK(vkMapMemory(device, memory, 0, size, 0, &mapped));
    rgba8.resize(static_cast<usize>(size));
    const u8* src = static_cast<const u8*>(mapped);
    const bool bgra = format_ == VK_FORMAT_B8G8R8A8_UNORM || format_ == VK_FORMAT_B8G8R8A8_SRGB;
    for (usize i = 0; i < rgba8.size(); i += 4) {
        rgba8[i + 0] = src[i + (bgra ? 2 : 0)];
        rgba8[i + 1] = src[i + 1];
        rgba8[i + 2] = src[i + (bgra ? 0 : 2)];
        rgba8[i + 3] = src[i + 3];
    }
    vkUnmapMemory(device, memory);

    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyBuffer(device, buffer, nullptr);
    vkFreeMemory(device, memory, nullptr);
    return true;
}

// --------------------------------------------------------------------------

VulkanCommandList::VulkanCommandList(VulkanDevice& device, u32 queue_family_index) : device_(device) {
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_family_index;
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

void VulkanCommandList::BeginRenderPass(ISwapChain& swap_chain, const ClearColor& clear_color) {
    auto& vk_swap = static_cast<VulkanSwapChain&>(swap_chain);

    // Index 0 = color attachment, index 1 = depth (see
    // VulkanSwapChain::CreateDefaultRenderPass) — both attachments use
    // loadOp=CLEAR every frame, so both need a clear value here regardless
    // of whether the pipeline this pass ends up drawing with actually
    // depth-tests.
    VkClearValue clear_values[2]{};
    clear_values[0].color.float32[0] = clear_color.r;
    clear_values[0].color.float32[1] = clear_color.g;
    clear_values[0].color.float32[2] = clear_color.b;
    clear_values[0].color.float32[3] = clear_color.a;
    clear_values[1].depthStencil.depth = 1.0f;

    VkRenderPassBeginInfo rp_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp_begin.renderPass = vk_swap.DefaultRenderPass();
    rp_begin.framebuffer = vk_swap.CurrentFramebuffer();
    rp_begin.renderArea.offset = {0, 0};
    rp_begin.renderArea.extent = {vk_swap.Width(), vk_swap.Height()};
    rp_begin.clearValueCount = 2;
    rp_begin.pClearValues = clear_values;
    vkCmdBeginRenderPass(command_buffer_, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    // Negative-height viewport (y = height, height = -height): the standard
    // trick to make Vulkan's NDC +Y point up like D3D12's, entirely on the
    // host side — callers of this abstract BeginRenderPass never need to
    // know or care that the two backends disagree on NDC Y direction.
    // Requires VK_KHR_maintenance1 (core since Vulkan 1.1; this backend
    // already requires 1.2). Found necessary by actually screenshotting
    // Unified-mode's output on both backends and seeing the same shader
    // produce a vertically mirrored triangle without this.
    VkViewport viewport{0.0f, static_cast<f32>(vk_swap.Height()), static_cast<f32>(vk_swap.Width()),
                         -static_cast<f32>(vk_swap.Height()), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {vk_swap.Width(), vk_swap.Height()}};
    vkCmdSetViewport(command_buffer_, 0, 1, &viewport);
    vkCmdSetScissor(command_buffer_, 0, 1, &scissor);
}

void VulkanCommandList::EndRenderPass() {
    vkCmdEndRenderPass(command_buffer_);
}

void VulkanCommandList::BindPipeline(PipelineHandle pipeline) {
    const PipelineRecord& record = device_.GetPipeline(pipeline);
    vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, record.pipeline);
    bound_pipeline_layout_ = record.layout;
}

void VulkanCommandList::SetPushConstants(const void* data, u32 size_bytes) {
    vkCmdPushConstants(command_buffer_, bound_pipeline_layout_,
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, size_bytes, data);
}

void VulkanCommandList::Draw(u32 vertex_count) {
    vkCmdDraw(command_buffer_, vertex_count, 1, 0, 0);
}

void VulkanCommandList::BindVertexBuffer(BufferHandle buffer, u32 stride_bytes) {
    (void)stride_bytes; // the stride is baked into the pipeline's VkVertexInputBindingDescription, not per-bind state
    const BufferRecord& record = device_.GetBuffer(buffer);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(command_buffer_, 0, 1, &record.buffer, &offset);
}

void VulkanCommandList::BindIndexBuffer(BufferHandle buffer, IndexFormat format) {
    const BufferRecord& record = device_.GetBuffer(buffer);
    VkIndexType index_type = format == IndexFormat::UInt16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    vkCmdBindIndexBuffer(command_buffer_, record.buffer, 0, index_type);
}

void VulkanCommandList::DrawIndexed(u32 index_count) {
    vkCmdDrawIndexed(command_buffer_, index_count, 1, 0, 0, 0);
}

void VulkanCommandList::BindBindlessTextures() {
    VkDescriptorSet sets[2] = {device_.BindlessTextureSet(), device_.BindlessSamplerSet()};
    vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, bound_pipeline_layout_, 0, 2, sets, 0,
                             nullptr);
}

} // namespace aether::gfx::rhi::vulkan_backend
