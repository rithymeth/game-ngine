#include "aether/gfx/rhi/vulkan/vulkan_backend.h"

#include "aether/core/log.h"
#include "aether/gfx/shader_compiler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#if !defined(_WIN32) && defined(AETHER_HAS_GLFW)
#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#endif

#if defined(__ANDROID__)
// Android's libvulkan.so exports only Vulkan 1.0 entry points below API 31, so the 1.2 timeline-semaphore calls are
// looked up through vkGetDeviceProcAddr (core name first, then the VK_KHR_timeline_semaphore one) on first use.
namespace {

template <typename Fn>
Fn AetherDeviceProc(VkDevice device, const char* core_name, const char* khr_name) {
    PFN_vkVoidFunction fn = vkGetDeviceProcAddr(device, core_name);
    if (!fn) fn = vkGetDeviceProcAddr(device, khr_name);
    if (!fn) throw std::runtime_error(std::string("Vulkan driver lacks ") + core_name);
    return reinterpret_cast<Fn>(fn);
}

VkResult AetherVkWaitSemaphores(VkDevice device, const VkSemaphoreWaitInfo* info, uint64_t timeout) {
    static const PFN_vkWaitSemaphores fn =
        AetherDeviceProc<PFN_vkWaitSemaphores>(device, "vkWaitSemaphores", "vkWaitSemaphoresKHR");
    return fn(device, info, timeout);
}

VkResult AetherVkGetSemaphoreCounterValue(VkDevice device, VkSemaphore semaphore, uint64_t* value) {
    static const PFN_vkGetSemaphoreCounterValue fn = AetherDeviceProc<PFN_vkGetSemaphoreCounterValue>(
        device, "vkGetSemaphoreCounterValue", "vkGetSemaphoreCounterValueKHR");
    return fn(device, semaphore, value);
}

} // namespace
#define vkWaitSemaphores AetherVkWaitSemaphores
#define vkGetSemaphoreCounterValue AetherVkGetSemaphoreCounterValue
#endif

namespace aether::gfx::rhi::vulkan_backend {

namespace {

bool IsFiniteClearColor(const ClearColor& color) {
    return std::isfinite(color.r) && std::isfinite(color.g) && std::isfinite(color.b) && std::isfinite(color.a);
}

bool IsValidResourceState(ResourceState state) {
    switch (state) {
        case ResourceState::Undefined:
        case ResourceState::RenderTarget:
        case ResourceState::Present:
        case ResourceState::CopyDest:
        case ResourceState::CopySource: return true;
        default: return false;
    }
}

bool IsValidVertexLayout(VertexLayout layout) {
    switch (layout) {
        case VertexLayout::PositionUv:
        case VertexLayout::PositionNormalUv:
        case VertexLayout::PositionNormalUvTangent: return true;
        default: return false;
    }
}

bool SupportsSampledTextureUpload(VkPhysicalDevice physical_device, VkFormat format) {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(physical_device, format, &properties);
    constexpr VkFormatFeatureFlags required =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    return (properties.optimalTilingFeatures & required) == required;
}

VkFormat SelectDepthAttachmentFormat(VkPhysicalDevice physical_device) {
    constexpr VkFormat candidates[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM, VK_FORMAT_D24_UNORM_S8_UINT,
                                       VK_FORMAT_D32_SFLOAT_S8_UINT};
    for (VkFormat candidate : candidates) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physical_device, candidate, &properties);
        if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
            return candidate;
        }
    }
    return VK_FORMAT_UNDEFINED;
}

VkImageAspectFlags DepthAttachmentAspect(VkFormat format) {
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
        aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    return aspect;
}

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
                                          "VK_KHR_wayland_surface", "VK_EXT_metal_surface",
                                          "VK_KHR_android_surface"};
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
    // macOS: MoltenVK is a "portability" implementation, which the loader
    // only lists when asked to (VK_KHR_portability_enumeration).
    bool portability = false;
    for (const VkExtensionProperties& ext : available) {
        if (std::strcmp(ext.extensionName, "VK_KHR_portability_enumeration") == 0) portability = true;
    }
    if (portability) {
        extensions.push_back("VK_KHR_portability_enumeration");
    }
#endif

    VkInstanceCreateInfo create_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
#if !defined(_WIN32)
    if (portability) {
        create_info.flags |= 0x00000001; // VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
    }
#endif
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
        VkPhysicalDeviceFeatures supported_features{};
        vkGetPhysicalDeviceFeatures(device, &supported_features);
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(device, &props);
        if (supported_features.shaderSampledImageArrayDynamicIndexing != VK_TRUE ||
            props.limits.maxPerStageDescriptorSampledImages < kMaxBindlessTextures ||
            props.limits.maxDescriptorSetSampledImages < kMaxBindlessTextures) {
            AETHER_LOG_INFO("Vulkan", "Skipping adapter %s: dynamic sampled-image indexing or 32-image limits missing",
                            props.deviceName);
            continue;
        }

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
    std::vector<const char*> extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    // A portability implementation (MoltenVK) must have its subset enabled.
    u32 ext_count = 0;
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> device_exts(ext_count);
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &ext_count, device_exts.data());
    for (const VkExtensionProperties& ext : device_exts) {
        if (std::strcmp(ext.extensionName, "VK_KHR_portability_subset") == 0) {
            extensions.push_back("VK_KHR_portability_subset");
        }
    }

    // Block-compressed (BC) textures where the GPU supports them: what CreateCompressedTexture uploads (desktop GPUs; most phones have ASTC instead).
    VkPhysicalDeviceFeatures supported_features{};
    vkGetPhysicalDeviceFeatures(physical_device, &supported_features);
    VkPhysicalDeviceFeatures enabled_features{};
    enabled_features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    enabled_features.textureCompressionBC = supported_features.textureCompressionBC;
    enabled_features.textureCompressionASTC_LDR = supported_features.textureCompressionASTC_LDR; // what phones have instead

    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pNext = &features_12;
    device_info.pEnabledFeatures = &enabled_features;
    device_info.queueCreateInfoCount = static_cast<u32>(queue_infos.size());
    device_info.pQueueCreateInfos = queue_infos.data();
    device_info.enabledExtensionCount = static_cast<u32>(extensions.size());
    device_info.ppEnabledExtensionNames = extensions.data();

    VkDevice device = VK_NULL_HANDLE;
    AETHER_VK_CHECK(vkCreateDevice(physical_device, &device_info, nullptr, &device));
    return device;
}

} // namespace

VulkanDevice::VulkanDevice(bool enable_debug_layer) {
    // Texture registration follows a GPU upload and a global descriptor write.
    // Reserve the fixed table before creating Vulkan objects so registry growth
    // cannot fail after a descriptor starts referencing the new image.
    sampled_textures_.reserve(kMaxUserBindlessTextures);
    try {
        instance_ = CreateInstance(enable_debug_layer);

        PhysicalDeviceChoice choice = PickPhysicalDevice(instance_);
        physical_device_ = choice.device;
        VkPhysicalDeviceProperties physical_device_properties{};
        vkGetPhysicalDeviceProperties(physical_device_, &physical_device_properties);
        std::copy(std::begin(physical_device_properties.limits.maxComputeWorkGroupCount),
                  std::end(physical_device_properties.limits.maxComputeWorkGroupCount),
                  max_compute_work_group_count_.begin());
        max_uniform_buffer_range_ = physical_device_properties.limits.maxUniformBufferRange;
        max_storage_buffer_range_ = physical_device_properties.limits.maxStorageBufferRange;
        max_compute_storage_buffers_ = physical_device_properties.limits.maxPerStageDescriptorStorageBuffers;
        max_compute_uniform_buffers_ = physical_device_properties.limits.maxPerStageDescriptorUniformBuffers;
        max_compute_sampled_images_ = physical_device_properties.limits.maxPerStageDescriptorSampledImages;
        max_compute_resources_ = physical_device_properties.limits.maxPerStageResources;
        max_descriptor_set_storage_buffers_ = physical_device_properties.limits.maxDescriptorSetStorageBuffers;
        max_descriptor_set_uniform_buffers_ = physical_device_properties.limits.maxDescriptorSetUniformBuffers;
        max_descriptor_set_sampled_images_ = physical_device_properties.limits.maxDescriptorSetSampledImages;
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
    } catch (...) {
        Destroy();
        throw;
    }
}

VulkanDevice::~VulkanDevice() {
    Destroy();
}

void VulkanDevice::DeferCommandListResources(VkCommandPool command_pool,
                                             std::vector<VkDescriptorPool> descriptor_pools) noexcept {
    try {
        std::lock_guard<std::mutex> lock(deferred_command_list_resources_mutex_);
        deferred_command_list_resources_.push_back({command_pool, std::move(descriptor_pools)});
    } catch (...) {
        // The Vulkan handles remain owned by the device and will be reclaimed
        // when it is destroyed; never destroy them early after a failed fence wait.
        AETHER_LOG_ERROR("Vulkan", "Could not record command-list pools for deferred teardown");
    }
}

VkResult VulkanDevice::WaitForDeviceIdle() {
    std::unique_lock<std::mutex> graphics_lock(graphics_queue_mutex_);
    if (compute_queue_ != queue_) {
        std::unique_lock<std::mutex> compute_lock(compute_queue_mutex_);
        return vkDeviceWaitIdle(device_);
    }
    return vkDeviceWaitIdle(device_);
}

void VulkanDevice::Destroy() noexcept {
    if (device_ != VK_NULL_HANDLE) {
        const VkResult wait_result = WaitForDeviceIdle();
        if (wait_result != VK_SUCCESS) {
            AETHER_LOG_WARN("Vulkan", "Could not drain device queues before teardown (VkResult=%d)",
                            static_cast<int>(wait_result));
        }
        for (const DeferredCommandListResources& resources : deferred_command_list_resources_) {
            for (VkDescriptorPool pool : resources.descriptor_pools) {
                if (pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, pool, nullptr);
            }
            if (resources.command_pool != VK_NULL_HANDLE) vkDestroyCommandPool(device_, resources.command_pool, nullptr);
        }
        deferred_command_list_resources_.clear();
        for (const PipelineRecord& record : pipelines_) {
            if (record.pipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, record.pipeline, nullptr);
            }
            if (record.layout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device_, record.layout, nullptr);
            }
        }
        for (const ComputePipelineRecord& record : compute_pipelines_) {
            if (record.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device_, record.pipeline, nullptr);
            if (record.layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, record.layout, nullptr);
            if (record.storage_layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, record.storage_layout, nullptr);
            if (record.constant_layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, record.constant_layout, nullptr);
            if (record.sampled_layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, record.sampled_layout, nullptr);
        }
        if (timeline_semaphore_ != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, timeline_semaphore_, nullptr);
        }
        if (compute_timeline_semaphore_ != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, compute_timeline_semaphore_, nullptr);
        }
        for (const BufferRecord& buffer : buffers_) {
            if (buffer.mapped) vkUnmapMemory(device_, buffer.memory);
            if (buffer.buffer != VK_NULL_HANDLE) vkDestroyBuffer(device_, buffer.buffer, nullptr);
            if (buffer.memory != VK_NULL_HANDLE) vkFreeMemory(device_, buffer.memory, nullptr);
        }
        auto destroy_texture_record = [this](const SampledTextureRecord& record) {
            if (record.view != VK_NULL_HANDLE) vkDestroyImageView(device_, record.view, nullptr);
            if (record.image != VK_NULL_HANDLE) vkDestroyImage(device_, record.image, nullptr);
            if (record.memory != VK_NULL_HANDLE) vkFreeMemory(device_, record.memory, nullptr);
        };
        for (const SampledTextureRecord& texture : sampled_textures_) {
            if (texture.image != VK_NULL_HANDLE) destroy_texture_record(texture);
        }
        destroy_texture_record(dummy_texture_);
        if (frame_constants_pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, frame_constants_pool_, nullptr);
        }
        if (frame_constants_set_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_, frame_constants_set_layout_, nullptr);
        }
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
        device_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
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
    return std::make_unique<VulkanCommandList>(*this, compute_queue_family_index_, true);
}

u64 VulkanDevice::SubmitCompute(ICommandList& cmd) {
    auto* rhi_cmd = dynamic_cast<VulkanCommandList*>(&cmd);
    if (!rhi_cmd || !rhi_cmd->BelongsTo(*this) || !rhi_cmd->IsComputeList() || !rhi_cmd->NativeHandle()) {
        AETHER_LOG_ERROR("Vulkan", "SubmitCompute requires a closed compute command list created by this device");
        return 0;
    }
    std::lock_guard<std::mutex> queue_lock(ComputeQueueMutex());
    if (!rhi_cmd->IsReadyForSubmit() || rhi_cmd->IsSubmissionInFlight()) {
        AETHER_LOG_ERROR("Vulkan", "SubmitCompute requires a closed compute command list that is not already in flight");
        return 0;
    }
    if (next_compute_fence_value_ == ~u64{0}) {
        AETHER_LOG_FATAL("Vulkan", "Compute timeline value space is exhausted; refusing queue submission");
        throw std::overflow_error("Vulkan compute timeline value space is exhausted");
    }
    auto native_cmd = static_cast<VkCommandBuffer>(rhi_cmd->NativeHandle());

    std::vector<VkSemaphore> wait_semaphores;
    std::vector<VkPipelineStageFlags> wait_stages;
    std::vector<u64> wait_values;

    if (pending_compute_wait_.valid) {
        wait_semaphores.push_back(pending_compute_wait_.semaphore);
        wait_stages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        wait_values.push_back(pending_compute_wait_.value);
    }

    const u64 fence_value = next_compute_fence_value_;
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
    ++next_compute_fence_value_;
    rhi_cmd->MarkSubmitted(fence_value);
    if (!wait_semaphores.empty() && pending_compute_wait_.valid) pending_compute_wait_ = {};
    return fence_value;
}

void VulkanDevice::WaitForComputeFence(u64 fence_value) {
    if (fence_value == 0) {
        return;
    }
    if (fence_value >= next_compute_fence_value_) {
        AETHER_LOG_FATAL("Vulkan", "Cannot wait for unsubmitted compute fence %llu",
                         static_cast<unsigned long long>(fence_value));
        throw std::invalid_argument("Vulkan compute fence value was not submitted");
    }
    if (IsComputeFenceComplete(fence_value)) return;
    VkSemaphoreWaitInfo wait_info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wait_info.semaphoreCount = 1;
    wait_info.pSemaphores = &compute_timeline_semaphore_;
    wait_info.pValues = &fence_value;
    AETHER_VK_CHECK(vkWaitSemaphores(device_, &wait_info, UINT64_MAX));
}

void VulkanDevice::WithQueuesIdle(const std::function<void()>& operation) {
    if (!operation) {
        throw std::invalid_argument("Vulkan idle-queue operation must not be empty");
    }

    std::unique_lock<std::mutex> graphics_lock(graphics_queue_mutex_, std::defer_lock);
    std::unique_lock<std::mutex> compute_lock(compute_queue_mutex_, std::defer_lock);
    if (compute_queue_ == queue_) {
        graphics_lock.lock();
    } else {
        std::lock(graphics_lock, compute_lock);
    }

    const u64 latest_graphics_fence = next_fence_value_.load(std::memory_order_acquire) - 1;
    const u64 latest_compute_fence = next_compute_fence_value_.load(std::memory_order_acquire) - 1;
    if (latest_graphics_fence != 0) WaitForFence(latest_graphics_fence);
    if (latest_compute_fence != 0) WaitForComputeFence(latest_compute_fence);
    operation();
}

void VulkanDevice::WithGraphicsQueueIdle(const std::function<void()>& operation) {
    if (!operation) {
        throw std::invalid_argument("Vulkan idle-graphics operation must not be empty");
    }

    std::lock_guard<std::mutex> graphics_lock(graphics_queue_mutex_);
    const u64 latest_graphics_fence = next_fence_value_.load(std::memory_order_acquire) - 1;
    if (latest_graphics_fence != 0) WaitForFence(latest_graphics_fence);
    operation();
}

bool VulkanDevice::IsComputeFenceComplete(u64 fence_value) const {
    if (fence_value == 0) return true;
    if (fence_value >= next_compute_fence_value_) return false;
    u64 current_value = 0;
    const VkResult result = vkGetSemaphoreCounterValue(device_, compute_timeline_semaphore_, &current_value);
    if (result != VK_SUCCESS) {
        AETHER_LOG_ERROR("Vulkan", "Compute fence status query failed (VkResult=%d)", static_cast<int>(result));
        return false;
    }
    return current_value >= fence_value;
}

void VulkanDevice::ComputeQueueWaitOnGraphics(u64 graphics_fence_value) {
    std::lock_guard<std::mutex> queue_lock(ComputeQueueMutex());
    if (graphics_fence_value == 0) {
        pending_compute_wait_ = {};
        return;
    }
    if (graphics_fence_value >= next_fence_value_) {
        AETHER_LOG_FATAL("Vulkan", "Cannot queue compute wait for unsubmitted graphics fence %llu",
                         static_cast<unsigned long long>(graphics_fence_value));
        throw std::invalid_argument("Vulkan compute queue dependency references an unsubmitted graphics fence");
    }
    pending_compute_wait_ = {true, timeline_semaphore_, graphics_fence_value};
}

void VulkanDevice::GraphicsQueueWaitOnCompute(u64 compute_fence_value) {
    std::lock_guard<std::mutex> queue_lock(GraphicsQueueMutex());
    if (compute_fence_value == 0) {
        pending_graphics_wait_ = {};
        return;
    }
    if (compute_fence_value >= next_compute_fence_value_) {
        AETHER_LOG_FATAL("Vulkan", "Cannot queue graphics wait for unsubmitted compute fence %llu",
                         static_cast<unsigned long long>(compute_fence_value));
        throw std::invalid_argument("Vulkan graphics queue dependency references an unsubmitted compute fence");
    }
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

class ScopedBufferRecord final {
public:
    explicit ScopedBufferRecord(VkDevice device) : device_(device) {}
    ~ScopedBufferRecord() { Reset(); }
    ScopedBufferRecord(const ScopedBufferRecord&) = delete;
    ScopedBufferRecord& operator=(const ScopedBufferRecord&) = delete;

    BufferRecord& Get() { return record_; }

    BufferRecord Release() {
        BufferRecord released = record_;
        record_ = {};
        return released;
    }

    void Reset() {
        if (record_.mapped && record_.memory != VK_NULL_HANDLE) vkUnmapMemory(device_, record_.memory);
        if (record_.buffer != VK_NULL_HANDLE) vkDestroyBuffer(device_, record_.buffer, nullptr);
        if (record_.memory != VK_NULL_HANDLE) vkFreeMemory(device_, record_.memory, nullptr);
        record_ = {};
    }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    BufferRecord record_{};
};

class ScopedSampledTextureRecord final {
public:
    explicit ScopedSampledTextureRecord(VkDevice device) : device_(device) {}
    ~ScopedSampledTextureRecord() { Reset(); }
    ScopedSampledTextureRecord(const ScopedSampledTextureRecord&) = delete;
    ScopedSampledTextureRecord& operator=(const ScopedSampledTextureRecord&) = delete;

    SampledTextureRecord& Get() { return record_; }

    SampledTextureRecord Release() {
        SampledTextureRecord released = record_;
        record_ = {};
        return released;
    }

    void Reset() {
        if (record_.view != VK_NULL_HANDLE) vkDestroyImageView(device_, record_.view, nullptr);
        if (record_.image != VK_NULL_HANDLE) vkDestroyImage(device_, record_.image, nullptr);
        if (record_.memory != VK_NULL_HANDLE) vkFreeMemory(device_, record_.memory, nullptr);
        record_ = {};
    }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    SampledTextureRecord record_{};
};

class ScopedCommandPool final {
public:
    explicit ScopedCommandPool(VkDevice device) : device_(device) {}
    ~ScopedCommandPool() {
        if (pool_ != VK_NULL_HANDLE) vkDestroyCommandPool(device_, pool_, nullptr);
    }
    ScopedCommandPool(const ScopedCommandPool&) = delete;
    ScopedCommandPool& operator=(const ScopedCommandPool&) = delete;

    VkCommandPool& Get() { return pool_; }

    VkCommandPool Release() {
        VkCommandPool released = pool_;
        pool_ = VK_NULL_HANDLE;
        return released;
    }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
};

class ScopedShaderModule final {
public:
    ScopedShaderModule(VkDevice device, const gfx::ShaderBytecode& bytecode)
        : device_(device), module_(CreateShaderModuleFromBytecode(device, bytecode)) {}
    ~ScopedShaderModule() {
        if (module_ != VK_NULL_HANDLE) vkDestroyShaderModule(device_, module_, nullptr);
    }
    ScopedShaderModule(const ScopedShaderModule&) = delete;
    ScopedShaderModule& operator=(const ScopedShaderModule&) = delete;

    VkShaderModule Get() const { return module_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkShaderModule module_ = VK_NULL_HANDLE;
};

class ScopedPipelineRecord final {
public:
    explicit ScopedPipelineRecord(VkDevice device) : device_(device) {}
    ~ScopedPipelineRecord() { Reset(); }
    ScopedPipelineRecord(const ScopedPipelineRecord&) = delete;
    ScopedPipelineRecord& operator=(const ScopedPipelineRecord&) = delete;

    PipelineRecord& Get() { return record_; }

    void Release() { record_ = {}; }

    void Reset() {
        if (record_.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device_, record_.pipeline, nullptr);
        if (record_.layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, record_.layout, nullptr);
        record_ = {};
    }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    PipelineRecord record_{};
};

class ScopedComputePipelineRecord final {
public:
    explicit ScopedComputePipelineRecord(VkDevice device) : device_(device) {}
    ~ScopedComputePipelineRecord() { Reset(); }
    ScopedComputePipelineRecord(const ScopedComputePipelineRecord&) = delete;
    ScopedComputePipelineRecord& operator=(const ScopedComputePipelineRecord&) = delete;

    ComputePipelineRecord& Get() { return record_; }
    void Release() { record_ = {}; }

    void Reset() {
        if (record_.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device_, record_.pipeline, nullptr);
        if (record_.layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, record_.layout, nullptr);
        if (record_.storage_layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, record_.storage_layout, nullptr);
        if (record_.constant_layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, record_.constant_layout, nullptr);
        if (record_.sampled_layout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, record_.sampled_layout, nullptr);
        record_ = {};
    }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    ComputePipelineRecord record_{};
};

// Host-visible + host-coherent — see BufferRecord's comment for why this is
// the right trade-off for vertex/index data at this RHI's demo scale (and
// also reused for CreateTexture's staging buffer, which is genuinely
// short-lived regardless).
BufferRecord CreateHostVisibleBuffer(VkDevice device, VkPhysicalDevice physical_device, VkDeviceSize size,
                                      VkBufferUsageFlags usage, const void* data, bool keep_mapped = false,
                                      u32 family_a = VK_QUEUE_FAMILY_IGNORED,
                                      u32 family_b = VK_QUEUE_FAMILY_IGNORED) {
    if (size == 0 || size > std::numeric_limits<usize>::max()) {
        AETHER_LOG_ERROR("Vulkan", "Host-visible buffer size must be non-zero and fit the host address space");
        throw std::invalid_argument("Host-visible buffer size must fit the host address space");
    }
    ScopedBufferRecord owned_result(device);
    BufferRecord& result = owned_result.Get();
    result.size_bytes = size;
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = size;
    buffer_info.usage = usage;
    u32 queue_families[2] = {family_a, family_b};
    if (family_a != VK_QUEUE_FAMILY_IGNORED && family_b != VK_QUEUE_FAMILY_IGNORED && family_a != family_b) {
        buffer_info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        buffer_info.queueFamilyIndexCount = 2;
        buffer_info.pQueueFamilyIndices = queue_families;
    } else {
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
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
    result.mapped = mapped;
    if (data) std::memcpy(mapped, data, static_cast<usize>(size));
    else std::memset(mapped, 0, static_cast<usize>(size));
    if (!keep_mapped) {
        vkUnmapMemory(device, result.memory);
        result.mapped = nullptr;
    }
    return owned_result.Release();
}
} // namespace

BufferHandle VulkanDevice::CreateBufferInternal(const void* data, u64 size_bytes) {
    // Both usages on every buffer regardless of which CreateXBuffer call
    // made it — simpler than tracking per-buffer usage flags, and harmless:
    // an unused usage bit on a host-visible buffer costs nothing at this
    // demo's scale.
    ScopedBufferRecord owned_record(device_);
    owned_record.Get() = CreateHostVisibleBuffer(device_, physical_device_, size_bytes,
                                                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                  data, true);
    buffers_.push_back(owned_record.Get());
    owned_record.Release();
    return BufferHandle{static_cast<u32>(buffers_.size() - 1)};
}

BufferHandle VulkanDevice::CreateVertexBuffer(const void* data, u64 size_bytes) {
    const BufferHandle handle = CreateBufferInternal(data, size_bytes);
    buffers_[handle.index].is_vertex = true;
    return handle;
}

BufferHandle VulkanDevice::CreateIndexBuffer(const void* data, u64 size_bytes, IndexFormat format) {
    const u32 index_size = format == IndexFormat::UInt16 ? 2u : format == IndexFormat::UInt32 ? 4u : 0u;
    if (index_size == 0 || size_bytes == 0 || size_bytes % index_size != 0) {
        throw std::invalid_argument("Index buffer size must be non-zero, aligned, and use a supported index format");
    }
    const BufferHandle handle = CreateBufferInternal(data, size_bytes);
    buffers_[handle.index].is_index = true;
    buffers_[handle.index].index_format = format;
    return handle;
}

BufferHandle VulkanDevice::CreateStorageBuffer(u64 size_bytes, u32 element_stride_bytes, const void* initial_data) {
    if (size_bytes == 0 || size_bytes > std::numeric_limits<VkDeviceSize>::max() ||
        size_bytes > max_storage_buffer_range_) {
        throw std::invalid_argument("Storage buffer size must be non-zero and within maxStorageBufferRange");
    }
    if (element_stride_bytes != 0 && (size_bytes % element_stride_bytes != 0 || element_stride_bytes > 2048)) {
        throw std::invalid_argument("Storage buffer size must be divisible by its valid element stride");
    }
    if (element_stride_bytes == 0 && size_bytes % 4 != 0) {
        throw std::invalid_argument("Raw storage buffer size must be divisible by four bytes");
    }
    ScopedBufferRecord owned_record(device_);
    owned_record.Get() = CreateHostVisibleBuffer(device_, physical_device_, size_bytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        initial_data, true, queue_family_index_, compute_queue_family_index_);
    owned_record.Get().is_storage = true;
    buffers_.push_back(owned_record.Get());
    owned_record.Release();
    return BufferHandle{static_cast<u32>(buffers_.size() - 1)};
}

BufferHandle VulkanDevice::CreateConstantBuffer(u64 size_bytes, const void* initial_data) {
    if (size_bytes == 0 || size_bytes > std::numeric_limits<VkDeviceSize>::max() ||
        size_bytes > max_uniform_buffer_range_) {
        throw std::invalid_argument("Constant buffer size must be non-zero and within maxUniformBufferRange");
    }
    ScopedBufferRecord owned_record(device_);
    owned_record.Get() = CreateHostVisibleBuffer(device_, physical_device_, size_bytes,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, initial_data, true, queue_family_index_, compute_queue_family_index_);
    owned_record.Get().is_constant = true;
    buffers_.push_back(owned_record.Get());
    owned_record.Release();
    return BufferHandle{static_cast<u32>(buffers_.size() - 1)};
}

void VulkanDevice::UpdateConstantBuffer(BufferHandle buffer, const void* data, u64 size_bytes, u64 offset) {
    if (!buffer.IsValid() || buffer.index >= buffers_.size() || !data || size_bytes == 0) {
        AETHER_LOG_ERROR("Vulkan", "UpdateConstantBuffer received an invalid handle, source, or size");
        return;
    }
    BufferRecord& record = buffers_[buffer.index];
    if (!record.is_constant || !record.mapped || offset > record.size_bytes || size_bytes > record.size_bytes - offset) {
        AETHER_LOG_ERROR("Vulkan", "UpdateConstantBuffer range exceeds constant buffer capacity");
        return;
    }
    WithQueuesIdle([&] { std::memcpy(static_cast<u8*>(record.mapped) + offset, data, static_cast<usize>(size_bytes)); });
}

void VulkanDevice::UpdateStorageBuffer(BufferHandle buffer, const void* data, u64 size_bytes, u64 offset) {
    if (!buffer.IsValid() || buffer.index >= buffers_.size() || !data || size_bytes == 0) {
        AETHER_LOG_ERROR("Vulkan", "UpdateStorageBuffer received an invalid handle, source, or size");
        return;
    }
    BufferRecord& record = buffers_[buffer.index];
    if (!record.is_storage || !record.mapped || offset > record.size_bytes || size_bytes > record.size_bytes - offset) {
        AETHER_LOG_ERROR("Vulkan", "UpdateStorageBuffer range exceeds storage buffer capacity");
        return;
    }
    WithQueuesIdle([&] { std::memcpy(static_cast<u8*>(record.mapped) + offset, data, static_cast<usize>(size_bytes)); });
}

void VulkanDevice::UpdateVertexBuffer(BufferHandle buffer, const void* data, u64 size_bytes) {
    if (!IsVertexBuffer(buffer) || buffer.index >= buffers_.size()) {
        if (buffer.IsValid() && buffer.index < buffers_.size()) {
            const BufferRecord& r = buffers_[buffer.index];
            AETHER_LOG_ERROR("Vulkan",
                             "UpdateVertexBuffer received a handle (%u of %zu) that is not a vertex buffer "
                             "(vertex=%d index=%d constant=%d storage=%d, %llu bytes)",
                             buffer.index, buffers_.size(), r.is_vertex, r.is_index, r.is_constant, r.is_storage,
                             static_cast<unsigned long long>(r.size_bytes));
        } else {
            AETHER_LOG_ERROR("Vulkan", "UpdateVertexBuffer received an invalid buffer handle (%u of %zu)", buffer.index,
                             buffers_.size());
        }
        return;
    }
    BufferRecord& record = buffers_[buffer.index];
    if (!data || !record.mapped || size_bytes == 0 || size_bytes > record.size_bytes) {
        AETHER_LOG_ERROR("Vulkan", "UpdateVertexBuffer received a null source, unmapped buffer, or invalid size "
                                    "(%llu bytes; capacity %llu)",
                         static_cast<unsigned long long>(size_bytes),
                         static_cast<unsigned long long>(record.size_bytes));
        return;
    }
    WithGraphicsQueueIdle([&] { std::memcpy(record.mapped, data, static_cast<usize>(size_bytes)); });
}

SampledTextureRecord VulkanDevice::UploadTextureRecord(u32 width, u32 height, const u8* rgba8_pixels) {
    if (width == 0 || height == 0 || !rgba8_pixels ||
        static_cast<VkDeviceSize>(width) > std::numeric_limits<VkDeviceSize>::max() / 4 / height) {
        AETHER_LOG_ERROR("Vulkan", "Sampled texture upload requires valid dimensions and pixel data");
        throw std::invalid_argument("Sampled texture upload requires valid dimensions and pixel data");
    }
    return UploadImageRecord(VK_FORMAT_R8G8B8A8_UNORM, width, height,
                             {{rgba8_pixels, static_cast<u64>(width) * height * 4, width, height}});
}

SampledTextureRecord VulkanDevice::UploadImageRecord(VkFormat format, u32 width, u32 height, const std::vector<ImageLevel>& levels) {
    const u32 level_count = static_cast<u32>(levels.size());
    // The levels go into one staging buffer back to back, each starting on a 16-byte boundary (a multiple of every texel and block size here).
    std::vector<VkDeviceSize> offsets;
    VkDeviceSize image_size = 0;
    for (const ImageLevel& level : levels) {
        offsets.push_back(image_size);
        image_size += (static_cast<VkDeviceSize>(level.size) + 15) / 16 * 16;
    }
    std::vector<u8> packed(static_cast<usize>(image_size), 0);
    for (u32 i = 0; i < level_count; ++i) std::memcpy(packed.data() + offsets[i], levels[i].data, static_cast<usize>(levels[i].size));
    ScopedBufferRecord owned_staging(device_);
    owned_staging.Get() =
        CreateHostVisibleBuffer(device_, physical_device_, image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, packed.data());
    const BufferRecord& staging = owned_staging.Get();

    ScopedSampledTextureRecord owned_record(device_);
    SampledTextureRecord& record = owned_record.Get();
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = level_count;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const std::array<u32, 2> sampled_texture_queue_families = {queue_family_index_, compute_queue_family_index_};
    if (queue_family_index_ != compute_queue_family_index_) {
        image_info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        image_info.queueFamilyIndexCount = static_cast<u32>(sampled_texture_queue_families.size());
        image_info.pQueueFamilyIndices = sampled_texture_queue_families.data();
    } else {
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
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
    to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, level_count, 0, 1};
    to_dst.srcAccessMask = 0;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(native_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                          0, nullptr, 1, &to_dst);

    std::vector<VkBufferImageCopy> regions(level_count);
    for (u32 i = 0; i < level_count; ++i) {
        regions[i] = {};
        regions[i].bufferOffset = offsets[i];
        regions[i].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
        regions[i].imageExtent = {levels[i].width, levels[i].height, 1};
    }
    vkCmdCopyBufferToImage(native_cmd, staging.buffer, record.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, level_count, regions.data());

    VkImageMemoryBarrier to_shader_read{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_shader_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_shader_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_shader_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_shader_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_shader_read.image = record.image;
    to_shader_read.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, level_count, 0, 1};
    to_shader_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_shader_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(native_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                          nullptr, 0, nullptr, 1, &to_shader_read);

    cmd->Close();
    const u64 upload_fence = Submit(*cmd, nullptr);
    if (upload_fence == 0) {
        throw std::runtime_error("Vulkan sampled texture upload submission failed");
    }
    if (compute_queue_ != queue_) {
        std::lock_guard<std::mutex> queue_lock(ComputeQueueMutex());
        if (!pending_compute_wait_.valid || pending_compute_wait_.semaphore != timeline_semaphore_ ||
            pending_compute_wait_.value < upload_fence) {
            pending_compute_wait_ = {true, timeline_semaphore_, upload_fence};
        }
    }
    try {
        WaitForFence(upload_fence);
    } catch (...) {
        // If a timeline wait itself fails after submission, don't release the
        // image or staging memory while queued commands may still reference
        // them. Device-idle is only a failure-path recovery attempt; normal
        // uploads continue to wait on their exact fence above.
        const VkResult idle_result = WaitForDeviceIdle();
        if (idle_result != VK_SUCCESS) {
            AETHER_LOG_ERROR("Vulkan", "Device idle failed after texture upload wait failure (VkResult=%d); "
                                      "leaving upload resources for device teardown",
                             static_cast<int>(idle_result));
            owned_staging.Release();
            owned_record.Release();
        }
        throw;
    }

    owned_staging.Reset();

    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = record.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, level_count, 0, 1};
    AETHER_VK_CHECK(vkCreateImageView(device_, &view_info, nullptr, &record.view));

    return owned_record.Release();
}

SampledTextureHandle VulkanDevice::CreateTexture(u32 width, u32 height, const u8* rgba8_pixels) {
    if (width == 0 || height == 0 || !rgba8_pixels ||
        static_cast<VkDeviceSize>(width) > std::numeric_limits<VkDeviceSize>::max() / 4 / height) {
        AETHER_LOG_ERROR("Vulkan", "CreateTexture requires non-zero dimensions and non-null RGBA8 pixel data");
        throw std::invalid_argument("CreateTexture requires non-zero dimensions and non-null RGBA8 pixel data");
    }
    if (TextureTableFull()) {
        AETHER_LOG_ERROR("Vulkan", "CreateTexture exhausted the bindless texture table (capacity=%u real textures)",
                         kMaxUserBindlessTextures);
        return {};
    }

    ScopedSampledTextureRecord owned_record(device_);
    owned_record.Get() = UploadTextureRecord(width, height, rgba8_pixels);
    const SampledTextureRecord record = owned_record.Get();
    // Slot zero remains the white fallback, matching the D3D12 descriptor
    // heap and the RHI's no-texture convention.
    const u32 index = StoreTextureRecord(record);
    owned_record.Release();

    WriteBindlessTexture(index, record);
    return SampledTextureHandle{index};
}

void VulkanDevice::WriteBindlessTexture(u32 index, const SampledTextureRecord& record) {
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
}

namespace {
VkFormat VkFormatOf(CompressedTextureFormat format) {
    switch (format) {
    case CompressedTextureFormat::BC1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case CompressedTextureFormat::BC3: return VK_FORMAT_BC3_UNORM_BLOCK;
    case CompressedTextureFormat::BC7: return VK_FORMAT_BC7_UNORM_BLOCK;
    case CompressedTextureFormat::ASTC_4x4: return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    case CompressedTextureFormat::ASTC_6x6: return VK_FORMAT_ASTC_6x6_UNORM_BLOCK;
    case CompressedTextureFormat::ASTC_8x8: return VK_FORMAT_ASTC_8x8_UNORM_BLOCK;
    }
    return VK_FORMAT_BC7_UNORM_BLOCK;
}
} // namespace

bool VulkanDevice::SupportsCompressedTexture(CompressedTextureFormat format) const {
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(physical_device_, &features);
    const bool astc = format == CompressedTextureFormat::ASTC_4x4 || format == CompressedTextureFormat::ASTC_6x6 || format == CompressedTextureFormat::ASTC_8x8;
    // The device was made with these features only where the GPU has them, so using the formats without them would be invalid.
    if (astc ? !features.textureCompressionASTC_LDR : !features.textureCompressionBC) return false;
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(physical_device_, VkFormatOf(format), &props);
    return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0 &&
           (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
}

SampledTextureHandle VulkanDevice::CreateCompressedTexture(CompressedTextureFormat format, u32 width, u32 height, std::span<const CompressedMip> mips) {
    if (width == 0 || height == 0 || mips.empty() || mips.size() > 16) {
        AETHER_LOG_ERROR("Vulkan", "CreateCompressedTexture requires non-zero dimensions and 1 to 16 mip levels");
        throw std::invalid_argument("CreateCompressedTexture requires non-zero dimensions and 1 to 16 mip levels");
    }
    std::vector<ImageLevel> levels;
    u32 w = width, h = height;
    for (const CompressedMip& mip : mips) {
        if (mip.data == nullptr || mip.width != w || mip.height != h || mip.size_bytes != CompressedLevelBytes(format, w, h)) {
            AETHER_LOG_ERROR("Vulkan", "CreateCompressedTexture: a mip level does not match its size or format");
            throw std::invalid_argument("CreateCompressedTexture: a mip level does not match its size or format");
        }
        levels.push_back({mip.data, mip.size_bytes, w, h});
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    if (!SupportsCompressedTexture(format)) {
        AETHER_LOG_ERROR("Vulkan", "CreateCompressedTexture: this device cannot sample that format");
        return {};
    }
    if (TextureTableFull()) {
        AETHER_LOG_ERROR("Vulkan", "CreateCompressedTexture exhausted the bindless texture table (capacity=%u real textures)", kMaxUserBindlessTextures);
        return {};
    }
    ScopedSampledTextureRecord owned_record(device_);
    owned_record.Get() = UploadImageRecord(VkFormatOf(format), width, height, levels);
    const SampledTextureRecord record = owned_record.Get();
    const u32 index = StoreTextureRecord(record);
    owned_record.Release();
    WriteBindlessTexture(index, record);
    return SampledTextureHandle{index};
}

SampledTextureHandle VulkanDevice::CreateR32FloatTexture(u32 width, u32 height, const f32* values) {
    if (width == 0 || height == 0 || !values ||
        static_cast<VkDeviceSize>(width) > std::numeric_limits<VkDeviceSize>::max() / sizeof(f32) / height) {
        throw std::invalid_argument("CreateR32FloatTexture requires non-zero dimensions and float data");
    }
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical_device_, &properties);
    if (width > properties.limits.maxImageDimension2D || height > properties.limits.maxImageDimension2D) {
        throw std::invalid_argument("CreateR32FloatTexture extent exceeds the Vulkan 2D image limit");
    }
    VkFormatProperties format_properties{};
    vkGetPhysicalDeviceFormatProperties(physical_device_, VK_FORMAT_R32_SFLOAT, &format_properties);
    const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((format_properties.optimalTilingFeatures & required) != required) {
        AETHER_LOG_ERROR("Vulkan", "R32_SFLOAT images do not support sampling and transfer-destination usage");
        return {};
    }
    if (TextureTableFull()) {
        AETHER_LOG_ERROR("Vulkan", "CreateR32FloatTexture exhausted the bindless texture table (capacity=%u real textures)",
                         kMaxUserBindlessTextures);
        return {};
    }

    ScopedSampledTextureRecord owned_record(device_);
    const u64 size_bytes = static_cast<u64>(width) * height * sizeof(f32);
    const std::vector<ImageLevel> levels = {{reinterpret_cast<const u8*>(values), size_bytes, width, height}};
    owned_record.Get() = UploadImageRecord(VK_FORMAT_R32_SFLOAT, width, height, levels);
    const SampledTextureRecord record = owned_record.Get();
    const u32 index = StoreTextureRecord(record);
    owned_record.Release();
    WriteBindlessTexture(index, record);
    return SampledTextureHandle{index};
}

u32 VulkanDevice::StoreTextureRecord(const SampledTextureRecord& record) {
    if (!free_texture_slots_.empty()) {
        const u32 position = free_texture_slots_.back();
        free_texture_slots_.pop_back();
        sampled_textures_[position] = record;
        return position + kFirstUserBindlessTextureIndex;
    }
    sampled_textures_.push_back(record);
    return static_cast<u32>(sampled_textures_.size() - 1) + kFirstUserBindlessTextureIndex;
}

bool VulkanDevice::DestroyTexture(SampledTextureHandle texture) {
    if (!texture.IsValid() || texture.index < kFirstUserBindlessTextureIndex) return false;
    const u32 position = texture.index - kFirstUserBindlessTextureIndex;
    if (position >= sampled_textures_.size() || sampled_textures_[position].image == VK_NULL_HANDLE) return false;
    AETHER_VK_CHECK(vkDeviceWaitIdle(device_)); // nothing in flight still reads it
    const SampledTextureRecord record = sampled_textures_[position];
    WriteBindlessTexture(texture.index, dummy_texture_); // a stale handle samples white, not freed memory
    vkDestroyImageView(device_, record.view, nullptr);
    vkDestroyImage(device_, record.image, nullptr);
    vkFreeMemory(device_, record.memory, nullptr);
    sampled_textures_[position] = {};
    free_texture_slots_.push_back(position);
    return true;
}

// Frame constant buffers are host-visible and mapped (CreateBufferInternal) but are not vertex buffers, so this cannot
// go through UpdateVertexBuffer, which rejects them: it used to, and the player's lights never reached the GPU. The
// caller only rewrites a slot whose previous frame has finished (SceneRenderer keeps one per frame in flight).
void VulkanDevice::UpdateFrameConstantBuffer(BufferHandle buffer, const void* data, u64 size_bytes) {
    if (!buffer.IsValid() || buffer.index >= buffers_.size() || buffers_[buffer.index].constants_set == VK_NULL_HANDLE) {
        AETHER_LOG_ERROR("Vulkan", "UpdateFrameConstantBuffer needs a buffer made by CreateFrameConstantBuffer (%u)",
                         buffer.index);
        return;
    }
    BufferRecord& record = buffers_[buffer.index];
    if (!data || !record.mapped || size_bytes == 0 || size_bytes > record.size_bytes) {
        AETHER_LOG_ERROR("Vulkan", "UpdateFrameConstantBuffer: null data, unmapped buffer, or %llu bytes into %llu",
                         static_cast<unsigned long long>(size_bytes), static_cast<unsigned long long>(record.size_bytes));
        return;
    }
    std::memcpy(record.mapped, data, static_cast<usize>(size_bytes));
}

BufferHandle VulkanDevice::CreateFrameConstantBuffer(u64 size_bytes) {
    if (size_bytes == 0) {
        AETHER_LOG_ERROR("Vulkan", "CreateConstantBuffer requires a non-zero size");
        throw std::invalid_argument("CreateConstantBuffer requires a non-zero size");
    }
    if (constant_buffer_count_ >= kMaxConstantBuffers) {
        AETHER_LOG_ERROR("Vulkan", "CreateConstantBuffer: out of constant buffers (capacity=%u)", kMaxConstantBuffers);
        return {};
    }
    const u64 padded = (size_bytes + 255) / 256 * 256; // covers any minUniformBufferOffsetAlignment
    const std::vector<u8> zeros(static_cast<usize>(padded), 0);
    const BufferHandle handle = CreateBufferInternal(zeros.data(), padded);
    BufferRecord& record = buffers_[handle.index];

    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = frame_constants_pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &frame_constants_set_layout_;
    AETHER_VK_CHECK(vkAllocateDescriptorSets(device_, &alloc, &record.constants_set));
    ++constant_buffer_count_;

    VkDescriptorBufferInfo buffer_desc{record.buffer, 0, padded};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = record.constants_set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &buffer_desc;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    return handle;
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

    // A 1x1 white dummy texture, permanently kept in slot zero and copied
    // into every other bindless texture slot up front — see the
    // SampledTextureRecord dummy_texture_ header comment for why every slot
    // needs a valid descriptor before any bindless pipeline draws.
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

    // Frame constants: set 2, one uniform buffer, visible to both stages; a pool with a set per constant buffer.
    VkDescriptorSetLayoutBinding constants_binding{};
    constants_binding.binding = 0;
    constants_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    constants_binding.descriptorCount = 1;
    constants_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo constants_layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    constants_layout_info.bindingCount = 1;
    constants_layout_info.pBindings = &constants_binding;
    AETHER_VK_CHECK(vkCreateDescriptorSetLayout(device_, &constants_layout_info, nullptr, &frame_constants_set_layout_));
    VkDescriptorPoolSize constants_pool_size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxConstantBuffers};
    VkDescriptorPoolCreateInfo constants_pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    constants_pool_info.maxSets = kMaxConstantBuffers;
    constants_pool_info.poolSizeCount = 1;
    constants_pool_info.pPoolSizes = &constants_pool_size;
    AETHER_VK_CHECK(vkCreateDescriptorPool(device_, &constants_pool_info, nullptr, &frame_constants_pool_));
}

ComputePipelineHandle VulkanDevice::CreateComputePipeline(const ComputePipelineDesc& desc) {
    if (desc.hlsl_source.empty() || desc.entry.empty()) {
        throw std::invalid_argument("ComputePipelineDesc requires HLSL source and an entry point");
    }
    if (desc.storage_buffer_count > 32 ||
        desc.storage_buffer_count > max_compute_storage_buffers_ ||
        desc.storage_buffer_count > max_descriptor_set_storage_buffers_) {
        throw std::invalid_argument("Compute pipeline storage-buffer count exceeds the supported or selected-device limit");
    }
    if (desc.sampled_texture_count > 32 ||
        desc.sampled_texture_count > max_compute_sampled_images_ ||
        desc.sampled_texture_count > max_descriptor_set_sampled_images_) {
        throw std::invalid_argument("Compute pipeline sampled-texture count exceeds the supported or selected-device limit");
    }
    if (desc.constant_buffer_count > 14 ||
        desc.constant_buffer_count > max_compute_uniform_buffers_ ||
        desc.constant_buffer_count > max_descriptor_set_uniform_buffers_) {
        throw std::invalid_argument("Compute pipeline constant-buffer count exceeds the supported or selected-device limit");
    }
    const u64 total_descriptors = static_cast<u64>(desc.storage_buffer_count) +
                                  desc.sampled_texture_count + desc.constant_buffer_count;
    if (total_descriptors > 64 || total_descriptors > max_compute_resources_) {
        throw std::invalid_argument("Compute pipeline descriptor count exceeds the supported or selected-device per-stage limit");
    }
    ScopedComputePipelineRecord owned_record(device_);
    ComputePipelineRecord& record = owned_record.Get();
    record.storage_buffer_count = desc.storage_buffer_count;
    record.sampled_texture_count = desc.sampled_texture_count;
    record.constant_buffer_count = desc.constant_buffer_count;
    {
        std::vector<VkDescriptorSetLayoutBinding> bindings(record.storage_buffer_count);
        for (u32 i = 0; i < record.storage_buffer_count; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        set_info.bindingCount = static_cast<u32>(bindings.size());
        set_info.pBindings = bindings.data();
        AETHER_VK_CHECK(vkCreateDescriptorSetLayout(device_, &set_info, nullptr, &record.storage_layout));
    }
    {
        std::vector<VkDescriptorSetLayoutBinding> bindings(record.constant_buffer_count);
        for (u32 i = 0; i < record.constant_buffer_count; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        set_info.bindingCount = static_cast<u32>(bindings.size());
        set_info.pBindings = bindings.data();
        AETHER_VK_CHECK(vkCreateDescriptorSetLayout(device_, &set_info, nullptr, &record.constant_layout));
    }
    {
        std::vector<VkDescriptorSetLayoutBinding> bindings(record.sampled_texture_count);
        for (u32 i = 0; i < record.sampled_texture_count; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        set_info.bindingCount = static_cast<u32>(bindings.size());
        set_info.pBindings = bindings.data();
        AETHER_VK_CHECK(vkCreateDescriptorSetLayout(device_, &set_info, nullptr, &record.sampled_layout));
    }
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkDescriptorSetLayout set_layouts[3] = {record.storage_layout, record.constant_layout, record.sampled_layout};
    layout_info.setLayoutCount = 3;
    layout_info.pSetLayouts = set_layouts;
    AETHER_VK_CHECK(vkCreatePipelineLayout(device_, &layout_info, nullptr, &record.layout));
    const gfx::ShaderBytecode spirv = gfx::CompileHLSLToSPIRV(desc.hlsl_source, desc.entry.c_str(), "cs_6_0",
                                                              "rhi_compute_pipeline");
    ScopedShaderModule module(device_, spirv);
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module.Get();
    stage.pName = desc.entry.c_str();
    VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline_info.stage = stage;
    pipeline_info.layout = record.layout;
    AETHER_VK_CHECK(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &record.pipeline));
    compute_pipelines_.push_back(record);
    owned_record.Release();
    return ComputePipelineHandle{static_cast<u32>(compute_pipelines_.size() - 1)};
}

PipelineHandle VulkanDevice::CreatePipeline(const PipelineDesc& desc, ISwapChain& swap_chain) {
    auto* vk_swap_ptr = dynamic_cast<VulkanSwapChain*>(&swap_chain);
    if (!vk_swap_ptr || !vk_swap_ptr->BelongsTo(*this)) {
        AETHER_LOG_ERROR("Vulkan", "CreatePipeline requires a Vulkan swap chain created by this device");
        return {};
    }
    if (!IsValidVertexLayout(desc.vertex_layout)) {
        AETHER_LOG_ERROR("Vulkan", "CreatePipeline received an unsupported vertex layout");
        return {};
    }
    auto& vk_swap = *vk_swap_ptr;

    VkPhysicalDeviceProperties device_properties{};
    vkGetPhysicalDeviceProperties(physical_device_, &device_properties);
    if (desc.push_constant_size_bytes % 4 != 0 ||
        desc.push_constant_size_bytes > device_properties.limits.maxPushConstantsSize) {
        AETHER_LOG_ERROR("Vulkan", "Graphics push-constant size must be four-byte aligned and within the device limit");
        throw std::invalid_argument("Graphics push-constant size exceeds Vulkan device limits");
    }

    ScopedPipelineRecord owned_record(device_);
    PipelineRecord& record = owned_record.Get();
    record.push_constant_size_bytes = desc.push_constant_size_bytes;
    record.enable_bindless_textures = desc.enable_bindless_textures;
    record.use_vertex_buffer = desc.use_vertex_buffer;
    record.vertex_stride_bytes = desc.vertex_layout == VertexLayout::PositionNormalUvTangent
                                     ? 48u
                                     : desc.vertex_layout == VertexLayout::PositionNormalUv ? 32u : 20u;

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
    // Sets 0 and 1: the bindless textures and sampler. Set 2: the frame constants (which need the other two, so the set indices match the HLSL spaces).
    VkDescriptorSetLayout bindless_set_layouts[3] = {bindless_texture_set_layout_, bindless_sampler_set_layout_, frame_constants_set_layout_};
    if (desc.frame_constants_size_bytes > 0) {
        AETHER_ASSERT(desc.enable_bindless_textures);
        AETHER_ASSERT(desc.frame_constants_size_bytes % 16 == 0);
        layout_info.setLayoutCount = 3;
        layout_info.pSetLayouts = bindless_set_layouts;
    } else if (desc.enable_bindless_textures) {
        layout_info.setLayoutCount = 2;
        layout_info.pSetLayouts = bindless_set_layouts;
    }
    AETHER_VK_CHECK(vkCreatePipelineLayout(device_, &layout_info, nullptr, &record.layout));

    gfx::ShaderBytecode vs_spirv =
        gfx::CompileHLSLToSPIRV(desc.hlsl_source, desc.vs_entry.c_str(), "vs_6_0", "rhi_pipeline_vs");
    gfx::ShaderBytecode ps_spirv =
        gfx::CompileHLSLToSPIRV(desc.hlsl_source, desc.ps_entry.c_str(), "ps_6_0", "rhi_pipeline_ps");
    ScopedShaderModule owned_vs_module(device_, vs_spirv);
    ScopedShaderModule owned_ps_module(device_, ps_spirv);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = owned_vs_module.Get();
    stages[0].pName = desc.vs_entry.c_str();
    stages[1] = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = owned_ps_module.Get();
    stages[1].pName = desc.ps_entry.c_str();

    // Keep POSITION / TEXCOORD as the default and match optional NORMAL and
    // TANGENT elements in the D3D12 backend exactly.
    const bool has_normals = desc.vertex_layout == VertexLayout::PositionNormalUv;
    const bool has_tangents = desc.vertex_layout == VertexLayout::PositionNormalUvTangent;
    VkVertexInputBindingDescription vertex_binding{
        0, has_tangents ? 48u : has_normals ? 32u : 20u, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription vertex_attributes[4] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        {1, 0, has_normals || has_tangents ? VK_FORMAT_R32G32B32_SFLOAT : VK_FORMAT_R32G32_SFLOAT, 12},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, 24},
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32},
    };

    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (desc.use_vertex_buffer) {
        vertex_input.vertexBindingDescriptionCount = 1;
        vertex_input.pVertexBindingDescriptions = &vertex_binding;
        vertex_input.vertexAttributeDescriptionCount = has_tangents ? 4u : has_normals ? 3u : 2u;
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
        blend_attachment.dstColorBlendFactor = desc.additive_blending ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
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
    depth_stencil.depthWriteEnable = desc.depth_test && desc.depth_write ? VK_TRUE : VK_FALSE;
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

    AETHER_VK_CHECK(pipeline_result);

    pipelines_.push_back(record);
    owned_record.Release();
    return PipelineHandle{static_cast<u32>(pipelines_.size() - 1)};
}

u64 VulkanDevice::Submit(ICommandList& cmd, ISwapChain* wait_on_swap_chain) {
    auto* rhi_cmd = dynamic_cast<VulkanCommandList*>(&cmd);
    auto* vk_swap_chain = wait_on_swap_chain ? dynamic_cast<VulkanSwapChain*>(wait_on_swap_chain) : nullptr;
    if (!rhi_cmd || !rhi_cmd->BelongsTo(*this) || rhi_cmd->IsComputeList() || !rhi_cmd->NativeHandle() ||
        (wait_on_swap_chain && (!vk_swap_chain || !vk_swap_chain->BelongsTo(*this)))) {
        AETHER_LOG_ERROR("Vulkan", "Submit requires a closed graphics command list and swap chain created by this device");
        return 0;
    }
    std::lock_guard<std::mutex> queue_lock(GraphicsQueueMutex());
    if (!rhi_cmd->IsReadyForSubmit() || rhi_cmd->IsSubmissionInFlight()) {
        AETHER_LOG_ERROR("Vulkan", "Submit requires a closed graphics command list that is not already in flight");
        return 0;
    }
    if (next_fence_value_ == ~u64{0}) {
        AETHER_LOG_FATAL("Vulkan", "Graphics timeline value space is exhausted; refusing queue submission");
        throw std::overflow_error("Vulkan graphics timeline value space is exhausted");
    }
    std::vector<VkSemaphore> wait_semaphores;
    std::vector<VkPipelineStageFlags> wait_stages;
    std::vector<u64> wait_values;
    std::vector<VkSemaphore> signal_semaphores;
    std::vector<u64> signal_values;

    if (vk_swap_chain && !vk_swap_chain->IsOffscreen()) {
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
    }

    const u64 fence_value = next_fence_value_;
    signal_semaphores.push_back(timeline_semaphore_);
    signal_values.push_back(fence_value);

    VkTimelineSemaphoreSubmitInfo timeline_info{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timeline_info.waitSemaphoreValueCount = static_cast<u32>(wait_values.size());
    timeline_info.pWaitSemaphoreValues = wait_values.data();
    timeline_info.signalSemaphoreValueCount = static_cast<u32>(signal_values.size());
    timeline_info.pSignalSemaphoreValues = signal_values.data();

    auto native_cmd = static_cast<VkCommandBuffer>(rhi_cmd->NativeHandle());

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
    ++next_fence_value_;
    rhi_cmd->MarkSubmitted(fence_value);
    if (pending_graphics_wait_.valid) pending_graphics_wait_ = {};
    return fence_value;
}

void VulkanDevice::WaitForFence(u64 fence_value) {
    if (fence_value == 0) {
        return;
    }
    if (fence_value >= next_fence_value_) {
        AETHER_LOG_FATAL("Vulkan", "Cannot wait for unsubmitted graphics fence %llu",
                         static_cast<unsigned long long>(fence_value));
        throw std::invalid_argument("Vulkan graphics fence value was not submitted");
    }
    if (IsFenceComplete(fence_value)) return;
    VkSemaphoreWaitInfo wait_info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wait_info.semaphoreCount = 1;
    wait_info.pSemaphores = &timeline_semaphore_;
    wait_info.pValues = &fence_value;
    AETHER_VK_CHECK(vkWaitSemaphores(device_, &wait_info, UINT64_MAX));
}

bool VulkanDevice::IsFenceComplete(u64 fence_value) const {
    if (fence_value == 0) return true;
    if (fence_value >= next_fence_value_) return false;
    u64 current_value = 0;
    const VkResult result = vkGetSemaphoreCounterValue(device_, timeline_semaphore_, &current_value);
    if (result != VK_SUCCESS) {
        AETHER_LOG_ERROR("Vulkan", "Graphics fence status query failed (VkResult=%d)", static_cast<int>(result));
        return false;
    }
    return current_value >= fence_value;
}

TextureHandle VulkanDevice::RegisterTexture(VkImage image) {
    if (image == VK_NULL_HANDLE) {
        AETHER_LOG_ERROR("Vulkan", "Cannot register a null texture image");
        return {};
    }
    for (usize i = 0; i < textures_.size(); ++i) {
        const u32 index = static_cast<u32>(i);
        if (textures_[index].image == VK_NULL_HANDLE) {
            textures_[index].image = image;
            return TextureHandle{index};
        }
    }
    if (textures_.size() >= static_cast<usize>(static_cast<u32>(-1))) {
        AETHER_LOG_ERROR("Vulkan", "Texture registry exhausted its 32-bit handle space");
        return {};
    }
    TextureHandle handle{static_cast<u32>(textures_.size())};
    textures_.push_back({image});
    return handle;
}

void VulkanDevice::UpdateTexture(TextureHandle handle, VkImage image) {
    if (!handle.IsValid() || handle.index >= textures_.size() || image == VK_NULL_HANDLE ||
        textures_[handle.index].image == VK_NULL_HANDLE) {
        AETHER_LOG_ERROR("Vulkan", "Cannot update an invalid texture handle (%u)", handle.index);
        return;
    }
    textures_[handle.index].image = image;
}

void VulkanDevice::UnregisterTexture(TextureHandle handle) {
    if (!handle.IsValid() || handle.index >= textures_.size() ||
        textures_[handle.index].image == VK_NULL_HANDLE) {
        AETHER_LOG_ERROR("Vulkan", "Cannot unregister an invalid texture handle (%u)", handle.index);
        return;
    }
    textures_[handle.index].image = VK_NULL_HANDLE;
}

VkImage VulkanDevice::GetTexture(TextureHandle handle) const {
    if (!handle.IsValid() || handle.index >= textures_.size() ||
        textures_[handle.index].image == VK_NULL_HANDLE) {
        AETHER_LOG_ERROR("Vulkan", "Cannot resolve an invalid texture handle (%u)", handle.index);
        return VK_NULL_HANDLE;
    }
    return textures_[handle.index].image;
}

// --------------------------------------------------------------------------

VulkanSwapChain::VulkanSwapChain(VulkanDevice& device, void* native_window_handle, u32 width, u32 height,
                                  u32 buffer_count)
    : device_(device), window_(native_window_handle), requested_buffer_count_(buffer_count) {
    offscreen_ = window_ == nullptr;
    try {
        depth_format_ = SelectDepthAttachmentFormat(device_.PhysicalDevice());
        if (depth_format_ == VK_FORMAT_UNDEFINED) {
            AETHER_LOG_ERROR("Vulkan", "Physical device exposes no supported depth attachment format");
            throw std::runtime_error("Vulkan device has no supported depth attachment format");
        }
        if (!offscreen_) {
#if defined(_WIN32)
            VkWin32SurfaceCreateInfoKHR surface_info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
            surface_info.hinstance = GetModuleHandleW(nullptr);
            surface_info.hwnd = static_cast<HWND>(window_);
            AETHER_VK_CHECK(vkCreateWin32SurfaceKHR(device_.Instance(), &surface_info, nullptr, &surface_));
#elif defined(__ANDROID__)
            // On Android the native handle is the ANativeWindow* of a SurfaceView (ANativeWindow_fromSurface).
            VkAndroidSurfaceCreateInfoKHR surface_info{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
            surface_info.window = static_cast<ANativeWindow*>(window_);
            AETHER_VK_CHECK(vkCreateAndroidSurfaceKHR(device_.Instance(), &surface_info, nullptr, &surface_));
#elif defined(AETHER_HAS_GLFW)
            AETHER_VK_CHECK(
                glfwCreateWindowSurface(device_.Instance(), static_cast<GLFWwindow*>(window_), nullptr, &surface_));
#else
            AETHER_LOG_FATAL("Vulkan", "Window swap chains need the GLFW window backend (AETHER_BUILD_WINDOWING)");
            throw std::runtime_error("no window surface support");
#endif

            VkBool32 present_supported = VK_FALSE;
            AETHER_VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(device_.PhysicalDevice(), device_.QueueFamilyIndex(),
                                                                  surface_, &present_supported));
            if (present_supported != VK_TRUE) {
                AETHER_LOG_ERROR("Vulkan", "Graphics queue family %u cannot present to the requested surface",
                                 device_.QueueFamilyIndex());
                throw std::runtime_error("Vulkan graphics queue family does not support presentation");
            }

            std::vector<VkSurfaceFormatKHR> surface_formats;
            bool enumerated_formats = false;
            for (u32 attempt = 0; attempt < 3 && !enumerated_formats; ++attempt) {
                u32 format_count = 0;
                AETHER_VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(device_.PhysicalDevice(), surface_, &format_count,
                                                                      nullptr));
                if (format_count == 0) {
                    AETHER_LOG_ERROR("Vulkan", "Surface returned no swap-chain formats");
                    throw std::runtime_error("Vulkan surface returned no swap-chain formats");
                }
                surface_formats.resize(format_count);
                const VkResult format_result = vkGetPhysicalDeviceSurfaceFormatsKHR(
                    device_.PhysicalDevice(), surface_, &format_count, surface_formats.data());
                if (format_result == VK_SUCCESS) {
                    surface_formats.resize(format_count);
                    enumerated_formats = true;
                } else if (format_result == VK_INCOMPLETE) {
                    surface_formats.clear();
                } else {
                    AETHER_VK_CHECK(format_result);
                }
            }
            if (!enumerated_formats) {
                AETHER_LOG_ERROR("Vulkan", "Surface format enumeration remained incomplete after retries");
                throw std::runtime_error("Vulkan surface format enumeration did not stabilize");
            }

            constexpr VkFormat preferred_formats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                                                      VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB};
            const VkSurfaceFormatKHR& first_format = surface_formats.front();
            if (surface_formats.size() == 1 && first_format.format == VK_FORMAT_UNDEFINED) {
                format_ = preferred_formats[0];
                color_space_ = first_format.colorSpace;
            } else {
                bool selected_format = false;
                for (VkFormat preferred : preferred_formats) {
                    for (const VkSurfaceFormatKHR& candidate : surface_formats) {
                        if (candidate.format == preferred && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                            format_ = candidate.format;
                            color_space_ = candidate.colorSpace;
                            selected_format = true;
                            break;
                        }
                    }
                    if (selected_format) break;
                }
                if (!selected_format) {
                    AETHER_LOG_ERROR("Vulkan", "Surface exposes no supported 8-bit RGBA/BGRA format in sRGB color space");
                    throw std::runtime_error("Vulkan surface has no supported color format");
                }
            }
        }

        CreateDefaultRenderPass();
        CreateSwapchainAndImages(width, height);
        CreateSyncObjects();
    } catch (...) {
        DestroySyncObjects();
        DestroySwapchainAndImages();
        if (default_render_pass_ != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device_.Handle(), default_render_pass_, nullptr);
            default_render_pass_ = VK_NULL_HANDLE;
        }
        for (TextureHandle handle : handles_) {
            device_.UnregisterTexture(handle);
        }
        handles_.clear();
        if (surface_ != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(device_.Instance(), surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
        throw;
    }
}

VulkanSwapChain::~VulkanSwapChain() {
    VkResult wait_result;
    {
        std::lock_guard<std::mutex> queue_lock(device_.GraphicsQueueMutex());
        wait_result = vkQueueWaitIdle(device_.Queue());
    }
    if (wait_result != VK_SUCCESS) {
        AETHER_LOG_WARN("Vulkan", "Could not drain graphics/present queue before swap-chain teardown (VkResult=%d)",
                        static_cast<int>(wait_result));
    }
    DestroySyncObjects();
    DestroySwapchainAndImages();
    if (default_render_pass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device_.Handle(), default_render_pass_, nullptr);
    }
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(device_.Instance(), surface_, nullptr);
    }
    for (TextureHandle handle : handles_) {
        device_.UnregisterTexture(handle);
    }
    handles_.clear();
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

    // Always present (see depth resource vectors' header comment) so any pipeline —
    // whether or not PipelineDesc::depth_test is set — can run in this one
    // render pass; loadOp=CLEAR every frame regardless of whether the
    // current draw actually depth-tests is harmless (same "always clear,
    // discard prior contents" convention the color attachment already uses).
    VkAttachmentDescription depth_attachment{};
    depth_attachment.format = depth_format_;
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
    image_info.format = depth_format_;
    image_info.extent = {extent_.width, extent_.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth_images_.resize(image_views_.size(), VK_NULL_HANDLE);
    depth_memories_.resize(image_views_.size(), VK_NULL_HANDLE);
    depth_image_views_.resize(image_views_.size(), VK_NULL_HANDLE);
    for (usize i = 0; i < image_views_.size(); ++i) {
        AETHER_VK_CHECK(vkCreateImage(device_.Handle(), &image_info, nullptr, &depth_images_[i]));

        VkMemoryRequirements mem_reqs{};
        vkGetImageMemoryRequirements(device_.Handle(), depth_images_[i], &mem_reqs);
        VkMemoryAllocateInfo alloc_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc_info.allocationSize = mem_reqs.size;
        alloc_info.memoryTypeIndex =
            FindMemoryType(device_.PhysicalDevice(), mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        AETHER_VK_CHECK(vkAllocateMemory(device_.Handle(), &alloc_info, nullptr, &depth_memories_[i]));
        AETHER_VK_CHECK(vkBindImageMemory(device_.Handle(), depth_images_[i], depth_memories_[i], 0));

        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = depth_images_[i];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = depth_format_;
        view_info.subresourceRange = {DepthAttachmentAspect(depth_format_), 0, 1, 0, 1};
        AETHER_VK_CHECK(vkCreateImageView(device_.Handle(), &view_info, nullptr, &depth_image_views_[i]));
    }
}

void VulkanSwapChain::DestroyDepthResources() {
    for (usize i = 0; i < depth_images_.size(); ++i) {
        if (i < depth_image_views_.size() && depth_image_views_[i] != VK_NULL_HANDLE) {
            vkDestroyImageView(device_.Handle(), depth_image_views_[i], nullptr);
        }
        if (depth_images_[i] != VK_NULL_HANDLE) {
            vkDestroyImage(device_.Handle(), depth_images_[i], nullptr);
        }
        if (i < depth_memories_.size() && depth_memories_[i] != VK_NULL_HANDLE) {
            vkFreeMemory(device_.Handle(), depth_memories_[i], nullptr);
        }
    }
    depth_image_views_.clear();
    depth_images_.clear();
    depth_memories_.clear();
}

void VulkanSwapChain::CreateFramebuffers() {
    framebuffers_.resize(image_views_.size());
    for (usize i = 0; i < image_views_.size(); ++i) {
        VkImageView attachments[] = {image_views_[i], depth_image_views_[i]};
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
        if (fb != VK_NULL_HANDLE) vkDestroyFramebuffer(device_.Handle(), fb, nullptr);
    }
    framebuffers_.clear();
}

std::vector<VkImage> VulkanSwapChain::CreateOffscreenImages(u32 width, u32 height) {
    extent_ = {std::max(width, 1u), std::max(height, 1u)};
    can_read_back_ = true;
    const usize image_count = std::max(requested_buffer_count_, 1u);
    offscreen_images_.assign(image_count, VK_NULL_HANDLE);
    offscreen_memory_.assign(image_count, VK_NULL_HANDLE);
    for (usize i = 0; i < offscreen_images_.size(); ++i) {
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
        AETHER_VK_CHECK(vkCreateImage(device_.Handle(), &image_info, nullptr, &offscreen_images_[i]));

        VkMemoryRequirements mem_reqs{};
        vkGetImageMemoryRequirements(device_.Handle(), offscreen_images_[i], &mem_reqs);
        VkMemoryAllocateInfo alloc_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc_info.allocationSize = mem_reqs.size;
        alloc_info.memoryTypeIndex =
            FindMemoryType(device_.PhysicalDevice(), mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        AETHER_VK_CHECK(vkAllocateMemory(device_.Handle(), &alloc_info, nullptr, &offscreen_memory_[i]));
        AETHER_VK_CHECK(vkBindImageMemory(device_.Handle(), offscreen_images_[i], offscreen_memory_[i], 0));
    }
    return offscreen_images_;
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
    AETHER_VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device_.PhysicalDevice(), surface_, &capabilities));

    constexpr VkImageUsageFlags required_usage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if ((capabilities.supportedUsageFlags & required_usage) != required_usage) {
        AETHER_LOG_ERROR("Vulkan", "Surface does not support transfer-destination and color-attachment swap-chain images");
        throw std::runtime_error("Vulkan surface lacks required swap-chain image usage flags");
    }

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
    swapchain_info.imageColorSpace = color_space_;
    swapchain_info.imageExtent = extent_;
    swapchain_info.imageArrayLayers = 1;
    // TRANSFER_DST covers the abstract ICommandList::ClearRenderTarget path
    // (no render pass involved); COLOR_ATTACHMENT covers a real render-pass-
    // based draw (rhi_demo's triangle mode) — both are near-universally
    // supported for swapchain images, so requesting both up front avoids
    // needing two different swapchains depending on how the demo is run.
    swapchain_info.imageUsage = required_usage;
    // TRANSFER_SRC, where the surface allows it, lets ReadBack copy the
    // back buffer out (editor screenshots).
    can_read_back_ = (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    if (can_read_back_) {
        swapchain_info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchain_info.preTransform = capabilities.currentTransform;
    constexpr VkCompositeAlphaFlagBitsKHR composite_alpha_preferences[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
    };
    bool has_composite_alpha = false;
    for (VkCompositeAlphaFlagBitsKHR candidate : composite_alpha_preferences) {
        if ((capabilities.supportedCompositeAlpha & candidate) != 0) {
            swapchain_info.compositeAlpha = candidate;
            has_composite_alpha = true;
            break;
        }
    }
    if (!has_composite_alpha) {
        AETHER_LOG_ERROR("Vulkan", "Surface reports no supported composite-alpha mode");
        throw std::runtime_error("Vulkan surface has no supported composite-alpha mode");
    }
    swapchain_info.presentMode = VK_PRESENT_MODE_FIFO_KHR; // always supported; vsync-equivalent
    swapchain_info.clipped = VK_TRUE;

    AETHER_VK_CHECK(vkCreateSwapchainKHR(device_.Handle(), &swapchain_info, nullptr, &swapchain_));

    std::vector<VkImage> images;
    bool enumerated_images = false;
    for (u32 attempt = 0; attempt < 3 && !enumerated_images; ++attempt) {
        u32 actual_image_count = 0;
        AETHER_VK_CHECK(vkGetSwapchainImagesKHR(device_.Handle(), swapchain_, &actual_image_count, nullptr));
        if (actual_image_count == 0) {
            AETHER_LOG_ERROR("Vulkan", "Swap-chain returned no images");
            throw std::runtime_error("Vulkan swap-chain returned no images");
        }
        images.resize(actual_image_count);
        VkResult image_result = vkGetSwapchainImagesKHR(device_.Handle(), swapchain_, &actual_image_count, images.data());
        if (image_result == VK_SUCCESS) {
            images.resize(actual_image_count);
            enumerated_images = true;
        } else if (image_result == VK_INCOMPLETE) {
            images.clear();
        } else {
            AETHER_VK_CHECK(image_result);
        }
    }
    if (!enumerated_images) {
        AETHER_LOG_ERROR("Vulkan", "Swap-chain image enumeration remained incomplete after retries");
        throw std::runtime_error("Vulkan swap-chain image enumeration did not stabilize");
    }

    image_views_.clear();
    for (usize i = 0; i < images.size(); ++i) {
        if (i < handles_.size()) {
            device_.UpdateTexture(handles_[i], images[i]);
        } else {
            handles_.push_back(device_.RegisterTexture(images[i]));
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
    for (usize i = images.size(); i < handles_.size(); ++i) {
        device_.UnregisterTexture(handles_[i]);
    }
    handles_.resize(images.size());

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
        if (s != VK_NULL_HANDLE) vkDestroySemaphore(device_.Handle(), s, nullptr);
    }
    for (VkSemaphore s : render_finished_semaphores_) {
        if (s != VK_NULL_HANDLE) vkDestroySemaphore(device_.Handle(), s, nullptr);
    }
    image_available_semaphores_.clear();
    render_finished_semaphores_.clear();
}

void VulkanSwapChain::Resize(u32 width, u32 height) {
    if (offscreen_ && std::max(width, 1u) == extent_.width && std::max(height, 1u) == extent_.height) {
        return;
    }
    VkResult wait_result;
    {
        std::lock_guard<std::mutex> queue_lock(device_.GraphicsQueueMutex());
        wait_result = vkQueueWaitIdle(device_.Queue());
    }
    if (wait_result != VK_SUCCESS) {
        AETHER_LOG_ERROR("Vulkan", "Could not drain graphics/present queue before swap-chain resize (VkResult=%d)",
                         static_cast<int>(wait_result));
        return;
    }
    DestroySyncObjects();
    DestroySwapchainAndImages();
    try {
        CreateSwapchainAndImages(width, height);
        CreateSyncObjects();
    } catch (...) {
        DestroySyncObjects();
        DestroySwapchainAndImages();
        for (TextureHandle handle : handles_) {
            device_.UnregisterTexture(handle);
        }
        handles_.clear();
        throw;
    }
    frame_index_ = 0;
    current_image_index_ = 0;
}

void VulkanSwapChain::AcquireNextImage() {
    if (image_available_semaphores_.empty()) {
        if (offscreen_ && !handles_.empty()) {
            current_image_index_ = (current_image_index_ + 1) % static_cast<u32>(handles_.size());
            return;
        }
        AETHER_LOG_ERROR("Vulkan", "Cannot acquire an image without swap-chain synchronization slots");
        throw std::runtime_error("Vulkan swap-chain has no acquire slots");
    }
    frame_index_ = (frame_index_ + 1) % static_cast<u32>(image_available_semaphores_.size());
    if (offscreen_) {
        if (handles_.empty()) {
            AETHER_LOG_ERROR("Vulkan", "Cannot acquire an image from an empty offscreen swap chain");
            throw std::runtime_error("Vulkan offscreen swap-chain has no images");
        }
        current_image_index_ = (current_image_index_ + 1) % static_cast<u32>(handles_.size());
        return;
    }
    VkResult result = vkAcquireNextImageKHR(device_.Handle(), swapchain_, UINT64_MAX,
                                             image_available_semaphores_[frame_index_], VK_NULL_HANDLE,
                                             &current_image_index_);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        Resize(extent_.width, extent_.height);
        if (image_available_semaphores_.empty()) {
            throw std::runtime_error("Vulkan swap-chain recreation produced no acquire slots");
        }
        result = vkAcquireNextImageKHR(device_.Handle(), swapchain_, UINT64_MAX,
                                       image_available_semaphores_[frame_index_], VK_NULL_HANDLE,
                                       &current_image_index_);
        if (result == VK_SUBOPTIMAL_KHR) {
            recreate_after_present_ = true;
            return;
        }
        AETHER_VK_CHECK(result);
    } else if (result == VK_SUBOPTIMAL_KHR) {
        // The image was acquired and its semaphore may be signaled asynchronously.
        // Render and present this image before recreating the swap chain or its semaphores.
        recreate_after_present_ = true;
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

    VkResult result;
    {
        std::lock_guard<std::mutex> queue_lock(device_.GraphicsQueueMutex());
        result = vkQueuePresentKHR(device_.Queue(), &present_info);
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        recreate_after_present_ = true;
    } else {
        AETHER_VK_CHECK(result);
    }
    if (recreate_after_present_) {
        recreate_after_present_ = false;
        Resize(extent_.width, extent_.height);
    }
}

bool VulkanSwapChain::ReadBack(std::vector<u8>& rgba8) {
    if (!can_read_back_ || extent_.width == 0 || extent_.height == 0 || handles_.empty() ||
        current_image_index_ >= handles_.size() ||
        static_cast<VkDeviceSize>(extent_.width) >
            std::numeric_limits<VkDeviceSize>::max() / 4 / extent_.height) {
        return false;
    }
    VkDevice device = device_.Handle();
    VkImage image = device_.GetTexture(handles_[current_image_index_]);
    if (image == VK_NULL_HANDLE) return false;
    {
        std::lock_guard<std::mutex> queue_lock(device_.GraphicsQueueMutex());
        AETHER_VK_CHECK(vkQueueWaitIdle(device_.Queue()));
    }

    const VkDeviceSize size = static_cast<VkDeviceSize>(extent_.width) * extent_.height * 4;
    if (size > static_cast<VkDeviceSize>(std::numeric_limits<usize>::max())) {
        return false;
    }
    ScopedBufferRecord owned_readback(device);
    BufferRecord& readback = owned_readback.Get();
    readback.size_bytes = size;
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    AETHER_VK_CHECK(vkCreateBuffer(device, &buffer_info, nullptr, &readback.buffer));
    VkMemoryRequirements mem_reqs{};
    vkGetBufferMemoryRequirements(device, readback.buffer, &mem_reqs);
    VkMemoryAllocateInfo alloc_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex =
        FindMemoryType(device_.PhysicalDevice(), mem_reqs.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    AETHER_VK_CHECK(vkAllocateMemory(device, &alloc_info, nullptr, &readback.memory));
    AETHER_VK_CHECK(vkBindBufferMemory(device, readback.buffer, readback.memory, 0));

    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = device_.QueueFamilyIndex();
    ScopedCommandPool owned_pool(device);
    AETHER_VK_CHECK(vkCreateCommandPool(device, &pool_info, nullptr, &owned_pool.Get()));
    VkCommandBufferAllocateInfo cmd_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmd_info.commandPool = owned_pool.Get();
    cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_info.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    AETHER_VK_CHECK(vkAllocateCommandBuffers(device, &cmd_info, &cmd));

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    AETHER_VK_CHECK(vkBeginCommandBuffer(cmd, &begin));
    // Both ways of drawing a frame (the default render pass, and
    // TransitionTexture to Present) leave the image in PRESENT_SRC.
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
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
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
    bool submitted = false;
    try {
        {
            std::lock_guard<std::mutex> queue_lock(device_.GraphicsQueueMutex());
            AETHER_VK_CHECK(vkQueueSubmit(device_.Queue(), 1, &submit, VK_NULL_HANDLE));
            submitted = true;
            AETHER_VK_CHECK(vkQueueWaitIdle(device_.Queue()));
        }
    } catch (...) {
        if (submitted) {
            const VkResult idle_result = device_.WaitForDeviceIdle();
            if (idle_result != VK_SUCCESS) {
                AETHER_LOG_ERROR("Vulkan", "Device idle failed after readback wait failure (VkResult=%d); "
                                          "leaving readback resources for device teardown",
                                 static_cast<int>(idle_result));
                owned_readback.Release();
                owned_pool.Release();
            }
        }
        throw;
    }

    void* mapped = nullptr;
    AETHER_VK_CHECK(vkMapMemory(device, readback.memory, 0, size, 0, &mapped));
    readback.mapped = mapped;
    rgba8.resize(static_cast<usize>(size));
    const u8* src = static_cast<const u8*>(mapped);
    const bool bgra = format_ == VK_FORMAT_B8G8R8A8_UNORM || format_ == VK_FORMAT_B8G8R8A8_SRGB;
    for (usize i = 0; i < rgba8.size(); i += 4) {
        rgba8[i + 0] = src[i + (bgra ? 2 : 0)];
        rgba8[i + 1] = src[i + 1];
        rgba8[i + 2] = src[i + (bgra ? 0 : 2)];
        rgba8[i + 3] = src[i + 3];
    }
    vkUnmapMemory(device, readback.memory);
    readback.mapped = nullptr;
    return true;
}

// --------------------------------------------------------------------------

VulkanCommandList::VulkanCommandList(VulkanDevice& device, u32 queue_family_index, bool compute_queue)
    : device_(device), compute_queue_(compute_queue) {
    ScopedCommandPool owned_pool(device_.Handle());
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_family_index;
    AETHER_VK_CHECK(vkCreateCommandPool(device_.Handle(), &pool_info, nullptr, &owned_pool.Get()));

    VkCommandBufferAllocateInfo alloc_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc_info.commandPool = owned_pool.Get();
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;
    AETHER_VK_CHECK(vkAllocateCommandBuffers(device_.Handle(), &alloc_info, &command_buffer_));
    const VkDescriptorPool initial_pool = CreateComputeDescriptorPool();
    try {
        compute_descriptor_pools_.push_back(initial_pool);
    } catch (...) {
        vkDestroyDescriptorPool(device_.Handle(), initial_pool, nullptr);
        throw;
    }
    command_pool_ = owned_pool.Release();
}

VkDescriptorPool VulkanCommandList::CreateComputeDescriptorPool() {
    VkDescriptorPoolSize pool_sizes[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096},
                                          {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096},
                                          {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4096}};
    VkDescriptorPoolCreateInfo descriptor_pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    descriptor_pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    descriptor_pool_info.maxSets = 256;
    descriptor_pool_info.poolSizeCount = 3;
    descriptor_pool_info.pPoolSizes = pool_sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    AETHER_VK_CHECK(vkCreateDescriptorPool(device_.Handle(), &descriptor_pool_info, nullptr, &pool));
    return pool;
}

VulkanCommandList::~VulkanCommandList() {
    bool defer_resources = false;
    if (IsSubmissionInFlight()) {
        try {
            if (compute_queue_) {
                device_.WaitForComputeFence(last_submitted_fence_);
            } else {
                device_.WaitForFence(last_submitted_fence_);
            }
        } catch (...) {
            AETHER_LOG_ERROR("Vulkan", "Could not wait for command-list submission; deferring its pools to device teardown");
            device_.DeferCommandListResources(command_pool_, std::move(compute_descriptor_pools_));
            command_pool_ = VK_NULL_HANDLE;
            defer_resources = true;
        }
    }
    if (!defer_resources) {
        for (VkDescriptorPool pool : compute_descriptor_pools_) {
            if (pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_.Handle(), pool, nullptr);
        }
    }
    if (!defer_resources && command_pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_.Handle(), command_pool_, nullptr);
    }
}

void VulkanCommandList::Reset() {
    std::lock_guard<std::mutex> queue_lock(compute_queue_ ? device_.ComputeQueueMutex() : device_.GraphicsQueueMutex());
    if (recording_) {
        AETHER_LOG_ERROR("Vulkan", "Reset received a command buffer that is still recording");
        return;
    }
    if (IsSubmissionInFlight()) {
        AETHER_LOG_ERROR("Vulkan", "Reset requires the command list's last submission fence to complete");
        return;
    }
    for (VkDescriptorPool pool : compute_descriptor_pools_) {
        AETHER_VK_CHECK(vkResetDescriptorPool(device_.Handle(), pool, 0));
    }
    AETHER_VK_CHECK(vkResetCommandBuffer(command_buffer_, 0));
    last_submitted_fence_ = 0;
    render_pass_active_ = false;
    graphics_pipeline_bound_ = false;
    graphics_bindless_enabled_ = false;
    graphics_bindless_bound_ = false;
    graphics_uses_vertex_buffer_ = false;
    graphics_vertex_stride_bytes_ = 0;
    vertex_buffer_bound_ = false;
    bound_vertex_count_ = 0;
    index_buffer_bound_ = false;
    bound_index_count_ = 0;
    graphics_push_constant_size_bytes_ = 0;
    bound_pipeline_layout_ = VK_NULL_HANDLE;
    bound_compute_descriptor_set_ = VK_NULL_HANDLE;
    bound_compute_sampled_set_ = VK_NULL_HANDLE;
    bound_compute_constant_set_ = VK_NULL_HANDLE;
    bound_compute_pipeline_ = {};
    compute_storage_bound_.clear();
    compute_sampled_texture_bound_.clear();
    compute_constant_buffer_bound_.clear();
    VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    AETHER_VK_CHECK(vkBeginCommandBuffer(command_buffer_, &begin_info));
    recording_ = true;
    has_closed_recording_ = false;
}

void VulkanCommandList::Close() {
    std::lock_guard<std::mutex> queue_lock(compute_queue_ ? device_.ComputeQueueMutex() : device_.GraphicsQueueMutex());
    if (!recording_) {
        AETHER_LOG_ERROR("Vulkan", "Close received a command buffer that is not recording");
        return;
    }
    if (render_pass_active_) {
        AETHER_LOG_ERROR("Vulkan", "Close requires EndRenderPass before closing the command buffer");
        return;
    }
    AETHER_VK_CHECK(vkEndCommandBuffer(command_buffer_));
    recording_ = false;
    has_closed_recording_ = true;
}

void VulkanCommandList::TransitionTexture(TextureHandle texture, ResourceState before, ResourceState after) {
    if (!recording_) {
        AETHER_LOG_ERROR("Vulkan", "TransitionTexture requires a recording command list");
        return;
    }
    if (compute_queue_ || !IsValidResourceState(before) || !IsValidResourceState(after)) {
        AETHER_LOG_ERROR("Vulkan", "TransitionTexture requires a graphics list and valid resource states");
        return;
    }
    const VkImage image = device_.GetTexture(texture);
    if (image == VK_NULL_HANDLE) return;

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = ToVkImageLayout(before);
    barrier.newLayout = ToVkImageLayout(after);
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
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
    if (!recording_) {
        AETHER_LOG_ERROR("Vulkan", "ClearRenderTarget requires a recording command list");
        return;
    }
    if (compute_queue_ || !IsFiniteClearColor(color)) {
        AETHER_LOG_ERROR("Vulkan", "ClearRenderTarget requires a graphics list and finite clear color");
        return;
    }
    const VkImage image = device_.GetTexture(texture);
    if (image == VK_NULL_HANDLE) return;

    VkClearColorValue clear_value{};
    clear_value.float32[0] = color.r;
    clear_value.float32[1] = color.g;
    clear_value.float32[2] = color.b;
    clear_value.float32[3] = color.a;

    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    // Requires the image to already be in ResourceState::RenderTarget (see
    // ToVkImageLayout's comment for why that maps to TRANSFER_DST here).
    vkCmdClearColorImage(command_buffer_, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          &clear_value, 1, &range);
}

void VulkanCommandList::SetViewportAndScissor(const Viewport& viewport, const Rect& scissor) {
    if (!recording_) {
        AETHER_LOG_ERROR("Vulkan", "SetViewportAndScissor requires a recording command list");
        return;
    }
    const std::int64_t scissor_width = static_cast<std::int64_t>(scissor.right) - scissor.left;
    const std::int64_t scissor_height = static_cast<std::int64_t>(scissor.bottom) - scissor.top;
    if (!render_pass_active_ || !std::isfinite(viewport.x) || !std::isfinite(viewport.y) ||
        !std::isfinite(viewport.width) || !std::isfinite(viewport.height) ||
        !std::isfinite(viewport.min_depth) || !std::isfinite(viewport.max_depth) || viewport.width <= 0.0f ||
        viewport.height <= 0.0f || viewport.min_depth < 0.0f || viewport.max_depth > 1.0f ||
        viewport.min_depth > viewport.max_depth || scissor_width <= 0 || scissor_height <= 0 ||
        scissor_width > std::numeric_limits<u32>::max() || scissor_height > std::numeric_limits<u32>::max()) {
        AETHER_LOG_ERROR("Vulkan", "SetViewportAndScissor requires an active pass and valid viewport/scissor bounds");
        return;
    }
    VkViewport vp{viewport.x, viewport.y, viewport.width, viewport.height, viewport.min_depth, viewport.max_depth};
    VkRect2D rect{{scissor.left, scissor.top},
                  {static_cast<u32>(scissor_width), static_cast<u32>(scissor_height)}};
    vkCmdSetViewport(command_buffer_, 0, 1, &vp);
    vkCmdSetScissor(command_buffer_, 0, 1, &rect);
}

void VulkanCommandList::BeginRenderPass(ISwapChain& swap_chain, const ClearColor& clear_color) {
    if (!recording_) {
        AETHER_LOG_ERROR("Vulkan", "BeginRenderPass requires a recording command list");
        return;
    }
    if (!IsFiniteClearColor(clear_color)) {
        AETHER_LOG_ERROR("Vulkan", "BeginRenderPass requires a finite clear color");
        return;
    }
    auto* vk_swap_ptr = dynamic_cast<VulkanSwapChain*>(&swap_chain);
    if (compute_queue_ || render_pass_active_ || !vk_swap_ptr || !vk_swap_ptr->BelongsTo(device_)) {
        AETHER_LOG_ERROR("Vulkan", "BeginRenderPass requires a non-nested Vulkan swap chain from this device");
        return;
    }
    auto& vk_swap = *vk_swap_ptr;

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
    render_pass_active_ = true;

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
    if (!recording_) {
        AETHER_LOG_ERROR("Vulkan", "EndRenderPass requires a recording command list");
        return;
    }
    if (!render_pass_active_) {
        AETHER_LOG_ERROR("Vulkan", "EndRenderPass requires an active render pass");
        return;
    }
    vkCmdEndRenderPass(command_buffer_);
    render_pass_active_ = false;
    graphics_pipeline_bound_ = false;
    graphics_bindless_enabled_ = false;
    graphics_bindless_bound_ = false;
    graphics_uses_vertex_buffer_ = false;
    vertex_buffer_bound_ = false;
    index_buffer_bound_ = false;
    graphics_push_constant_size_bytes_ = 0;
    bound_pipeline_layout_ = VK_NULL_HANDLE;
}

void VulkanCommandList::BindPipeline(PipelineHandle pipeline) {
    if (!recording_) {
        AETHER_LOG_ERROR("Vulkan", "BindPipeline requires a recording command list");
        return;
    }
    if (!render_pass_active_) {
        AETHER_LOG_ERROR("Vulkan", "BindPipeline requires an active render pass");
        graphics_pipeline_bound_ = false;
        graphics_bindless_enabled_ = false;
        graphics_bindless_bound_ = false;
        graphics_uses_vertex_buffer_ = false;
        vertex_buffer_bound_ = false;
        index_buffer_bound_ = false;
        graphics_push_constant_size_bytes_ = 0;
        bound_pipeline_layout_ = VK_NULL_HANDLE;
        return;
    }
    if (!pipeline.IsValid() || pipeline.index >= device_.PipelineCount()) {
        AETHER_LOG_ERROR("Vulkan", "BindPipeline received an invalid graphics-pipeline handle");
        graphics_pipeline_bound_ = false;
        graphics_bindless_enabled_ = false;
        graphics_bindless_bound_ = false;
        graphics_uses_vertex_buffer_ = false;
        vertex_buffer_bound_ = false;
        index_buffer_bound_ = false;
        graphics_push_constant_size_bytes_ = 0;
        bound_pipeline_layout_ = VK_NULL_HANDLE;
        return;
    }
    const PipelineRecord& record = device_.GetPipeline(pipeline);
    vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, record.pipeline);
    bound_pipeline_layout_ = record.layout;
    graphics_pipeline_bound_ = true;
    graphics_push_constant_size_bytes_ = record.push_constant_size_bytes;
    graphics_bindless_enabled_ = record.enable_bindless_textures;
    graphics_bindless_bound_ = false;
    graphics_uses_vertex_buffer_ = record.use_vertex_buffer;
    graphics_vertex_stride_bytes_ = record.vertex_stride_bytes;
    vertex_buffer_bound_ = false;
    index_buffer_bound_ = false;
}

void VulkanCommandList::SetPushConstants(const void* data, u32 size_bytes) {
    if (!recording_) {
        AETHER_LOG_ERROR("Vulkan", "SetPushConstants requires a recording command list");
        return;
    }
    if (!render_pass_active_ || !graphics_pipeline_bound_ || !data || size_bytes == 0 || size_bytes % 4 != 0 ||
        size_bytes > graphics_push_constant_size_bytes_) {
        AETHER_LOG_ERROR("Vulkan", "SetPushConstants requires a valid pipeline and data within its aligned push-constant range");
        return;
    }
    vkCmdPushConstants(command_buffer_, bound_pipeline_layout_,
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, size_bytes, data);
}

void VulkanCommandList::Draw(u32 vertex_count) {
    if (!recording_) {
        AETHER_LOG_ERROR("Vulkan", "Draw requires a recording command list");
        return;
    }
    if (!render_pass_active_ || !graphics_pipeline_bound_ ||
        (graphics_uses_vertex_buffer_ && (!vertex_buffer_bound_ || vertex_count > bound_vertex_count_)) ||
        (graphics_bindless_enabled_ && !graphics_bindless_bound_)) {
        AETHER_LOG_ERROR("Vulkan", "Draw requires a valid bound graphics pipeline");
        return;
    }
    vkCmdDraw(command_buffer_, vertex_count, 1, 0, 0);
}

void VulkanCommandList::BindVertexBuffer(BufferHandle buffer, u32 stride_bytes) {
    if (!recording_) {
        AETHER_LOG_ERROR("Vulkan", "BindVertexBuffer requires a recording command list");
        return;
    }
    if (!render_pass_active_ || !graphics_pipeline_bound_ || !graphics_uses_vertex_buffer_ ||
        stride_bytes != graphics_vertex_stride_bytes_ || !device_.IsVertexBuffer(buffer)) {
        AETHER_LOG_ERROR("Vulkan", "BindVertexBuffer received an invalid handle or a stride that does not match the pipeline");
        return;
    }
    const BufferRecord& record = device_.GetBuffer(buffer);
    if (record.size_bytes % stride_bytes != 0) {
        AETHER_LOG_ERROR("Vulkan", "BindVertexBuffer size must contain whole vertices for the pipeline stride");
        return;
    }
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(command_buffer_, 0, 1, &record.buffer, &offset);
    vertex_buffer_bound_ = true;
    bound_vertex_count_ = record.size_bytes / stride_bytes;
}

void VulkanCommandList::BindIndexBuffer(BufferHandle buffer, IndexFormat format) {
    if (!recording_) {
        AETHER_LOG_ERROR("Vulkan", "BindIndexBuffer requires a recording command list");
        return;
    }
    if (!render_pass_active_ || !graphics_pipeline_bound_ || !graphics_uses_vertex_buffer_ ||
        (format != IndexFormat::UInt16 && format != IndexFormat::UInt32) ||
        !device_.IsIndexBuffer(buffer, format)) {
        AETHER_LOG_ERROR("Vulkan", "BindIndexBuffer received an invalid format, state, or buffer handle");
        return;
    }
    const BufferRecord& record = device_.GetBuffer(buffer);
    const u32 index_size_bytes = format == IndexFormat::UInt16 ? 2u : 4u;
    VkIndexType index_type = format == IndexFormat::UInt16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    vkCmdBindIndexBuffer(command_buffer_, record.buffer, 0, index_type);
    index_buffer_bound_ = true;
    bound_index_count_ = record.size_bytes / index_size_bytes;
}

void VulkanCommandList::DrawIndexed(u32 index_count) {
    if (!recording_) {
        AETHER_LOG_ERROR("Vulkan", "DrawIndexed requires a recording command list");
        return;
    }
    if (!render_pass_active_ || !graphics_pipeline_bound_ || !graphics_uses_vertex_buffer_ ||
        !vertex_buffer_bound_ || !index_buffer_bound_ || index_count > bound_index_count_ ||
        (graphics_bindless_enabled_ && !graphics_bindless_bound_)) {
        AETHER_LOG_ERROR("Vulkan", "DrawIndexed requires a valid bound graphics pipeline");
        return;
    }
    vkCmdDrawIndexed(command_buffer_, index_count, 1, 0, 0, 0);
}

void VulkanCommandList::BindFrameConstants(BufferHandle buffer) {
    if (!buffer.IsValid() || device_.GetBuffer(buffer).constants_set == VK_NULL_HANDLE) {
        AETHER_LOG_ERROR("Vulkan", "BindFrameConstants needs a buffer made by CreateConstantBuffer");
        return;
    }
    const VkDescriptorSet set = device_.GetBuffer(buffer).constants_set;
    vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, bound_pipeline_layout_, 2, 1, &set, 0, nullptr);
}
void VulkanCommandList::BindComputePipeline(ComputePipelineHandle pipeline) {
    if (!recording_) {
        AETHER_LOG_ERROR("Vulkan", "BindComputePipeline requires a recording command list");
        return;
    }
    if (!compute_queue_ || !pipeline.IsValid() || pipeline.index >= device_.ComputePipelineCount()) {
        AETHER_LOG_ERROR("Vulkan", "BindComputePipeline requires a compute command list and valid pipeline handle");
        bound_compute_pipeline_ = {};
        compute_storage_bound_.clear();
        compute_sampled_texture_bound_.clear();
        compute_constant_buffer_bound_.clear();
        bound_compute_descriptor_set_ = VK_NULL_HANDLE;
        bound_compute_sampled_set_ = VK_NULL_HANDLE;
        bound_compute_constant_set_ = VK_NULL_HANDLE;
        return;
    }
    const ComputePipelineRecord& record = device_.GetComputePipeline(pipeline);
    bound_compute_pipeline_ = pipeline;
    compute_storage_bound_.assign(record.storage_buffer_count, false);
    compute_sampled_texture_bound_.assign(record.sampled_texture_count, false);
    compute_constant_buffer_bound_.assign(record.constant_buffer_count, false);
    bound_compute_descriptor_set_ = VK_NULL_HANDLE;
    bound_compute_sampled_set_ = VK_NULL_HANDLE;
    bound_compute_constant_set_ = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo alloc_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    VkDescriptorSetLayout set_layouts[3] = {record.storage_layout, record.constant_layout, record.sampled_layout};
    VkDescriptorSet sets[3]{};
    alloc_info.descriptorSetCount = 3;
    alloc_info.pSetLayouts = set_layouts;
    alloc_info.descriptorPool = compute_descriptor_pools_.back();
    VkResult allocation = vkAllocateDescriptorSets(device_.Handle(), &alloc_info, sets);
    if (allocation == VK_ERROR_OUT_OF_POOL_MEMORY || allocation == VK_ERROR_FRAGMENTED_POOL) {
        const VkDescriptorPool next_pool = CreateComputeDescriptorPool();
        try {
            compute_descriptor_pools_.push_back(next_pool);
        } catch (...) {
            vkDestroyDescriptorPool(device_.Handle(), next_pool, nullptr);
            throw;
        }
        alloc_info.descriptorPool = next_pool;
        allocation = vkAllocateDescriptorSets(device_.Handle(), &alloc_info, sets);
    }
    AETHER_VK_CHECK(allocation);
    bound_compute_descriptor_set_ = sets[0];
    bound_compute_constant_set_ = sets[1];
    bound_compute_sampled_set_ = sets[2];
    vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, record.pipeline);
    vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, record.layout, 0, 3,
                            sets, 0, nullptr);
}

void VulkanCommandList::BindComputeConstantBuffer(u32 slot, BufferHandle buffer) {
    if (!recording_) {
        AETHER_LOG_ERROR("Vulkan", "BindComputeConstantBuffer requires a recording command list");
        return;
    }
    if (!compute_queue_ || !bound_compute_pipeline_.IsValid()) {
        AETHER_LOG_ERROR("Vulkan", "BindComputeConstantBuffer requires a bound compute pipeline");
        return;
    }
    const ComputePipelineRecord& pipeline = device_.GetComputePipeline(bound_compute_pipeline_);
    if (slot >= pipeline.constant_buffer_count || !buffer.IsValid() || buffer.index >= device_.BufferCount() ||
        !device_.GetBuffer(buffer).is_constant) {
        AETHER_LOG_ERROR("Vulkan", "BindComputeConstantBuffer received an invalid slot or buffer handle");
        return;
    }
    const BufferRecord& record = device_.GetBuffer(buffer);
    VkDescriptorBufferInfo buffer_info{record.buffer, 0, record.size_bytes};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = bound_compute_constant_set_;
    write.dstBinding = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &buffer_info;
    vkUpdateDescriptorSets(device_.Handle(), 1, &write, 0, nullptr);
    compute_constant_buffer_bound_[slot] = true;
}

void VulkanCommandList::BindComputeStorageBuffer(u32 slot, BufferHandle buffer) {
    if (!recording_) {
        AETHER_LOG_ERROR("Vulkan", "BindComputeStorageBuffer requires a recording command list");
        return;
    }
    if (!compute_queue_ || !bound_compute_pipeline_.IsValid()) {
        AETHER_LOG_ERROR("Vulkan", "BindComputeStorageBuffer requires a bound compute pipeline");
        return;
    }
    const ComputePipelineRecord& pipeline = device_.GetComputePipeline(bound_compute_pipeline_);
    if (slot >= pipeline.storage_buffer_count || !buffer.IsValid() || buffer.index >= device_.BufferCount() ||
        !device_.GetBuffer(buffer).is_storage) {
        AETHER_LOG_ERROR("Vulkan", "BindComputeStorageBuffer received an invalid slot or buffer handle");
        return;
    }
    const BufferRecord& record = device_.GetBuffer(buffer);
    VkDescriptorBufferInfo buffer_info{record.buffer, 0, record.size_bytes};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = bound_compute_descriptor_set_;
    write.dstBinding = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &buffer_info;
    vkUpdateDescriptorSets(device_.Handle(), 1, &write, 0, nullptr);
    compute_storage_bound_[slot] = true;
}

void VulkanCommandList::BindComputeSampledTexture(u32 slot, SampledTextureHandle texture) {
    if (!recording_) {
        AETHER_LOG_ERROR("Vulkan", "BindComputeSampledTexture requires a recording command list");
        return;
    }
    if (!compute_queue_ || !bound_compute_pipeline_.IsValid()) {
        AETHER_LOG_ERROR("Vulkan", "BindComputeSampledTexture requires a bound compute pipeline");
        return;
    }
    const ComputePipelineRecord& pipeline = device_.GetComputePipeline(bound_compute_pipeline_);
    if (slot >= pipeline.sampled_texture_count || !texture.IsValid() ||
        texture.index < kFirstUserBindlessTextureIndex ||
        texture.index - kFirstUserBindlessTextureIndex >= device_.SampledTextureCount()) {
        AETHER_LOG_ERROR("Vulkan", "BindComputeSampledTexture received an invalid slot or texture handle");
        return;
    }
    const SampledTextureRecord& record = device_.GetSampledTexture(texture);
    VkDescriptorImageInfo image_info{};
    image_info.imageView = record.view;
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = bound_compute_sampled_set_;
    write.dstBinding = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &image_info;
    vkUpdateDescriptorSets(device_.Handle(), 1, &write, 0, nullptr);
    compute_sampled_texture_bound_[slot] = true;
}

void VulkanCommandList::ComputeBufferBarrier(BufferHandle buffer) {
    if (!recording_) {
        AETHER_LOG_ERROR("Vulkan", "ComputeBufferBarrier requires a recording command list");
        return;
    }
    if (!compute_queue_ || !buffer.IsValid() || buffer.index >= device_.BufferCount() ||
        !device_.GetBuffer(buffer).is_storage) {
        AETHER_LOG_ERROR("Vulkan", "ComputeBufferBarrier requires a compute command list and storage buffer");
        return;
    }
    const BufferRecord& record = device_.GetBuffer(buffer);
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = record.buffer;
    barrier.offset = 0;
    barrier.size = record.size_bytes;
    vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);
}

void VulkanCommandList::Dispatch(u32 groups_x, u32 groups_y, u32 groups_z) {
    if (!recording_) {
        AETHER_LOG_ERROR("Vulkan", "Dispatch requires a recording command list");
        return;
    }
    if (groups_x == 0 || groups_y == 0 || groups_z == 0 ||
        !device_.IsComputeDispatchWithinLimits(groups_x, groups_y, groups_z)) {
        AETHER_LOG_ERROR("Vulkan", "Compute dispatch group counts are zero or exceed device limits");
        return;
    }
    if (!compute_queue_ || !bound_compute_pipeline_.IsValid()) {
        AETHER_LOG_ERROR("Vulkan", "Dispatch requires a bound compute pipeline");
        return;
    }
    for (bool bound : compute_storage_bound_) {
        if (!bound) {
            AETHER_LOG_ERROR("Vulkan", "Dispatch requires every compute storage buffer to be bound");
            return;
        }
    }
    for (bool bound : compute_sampled_texture_bound_) {
        if (!bound) {
            AETHER_LOG_ERROR("Vulkan", "Dispatch requires every compute sampled texture to be bound");
            return;
        }
    }
    for (bool bound : compute_constant_buffer_bound_) {
        if (!bound) {
            AETHER_LOG_ERROR("Vulkan", "Dispatch requires every compute constant buffer to be bound");
            return;
        }
    }
    vkCmdDispatch(command_buffer_, groups_x, groups_y, groups_z);
}

void VulkanCommandList::BindBindlessTextures() {
    if (!recording_) {
        AETHER_LOG_ERROR("Vulkan", "BindBindlessTextures requires a recording command list");
        return;
    }
    if (!render_pass_active_ || !graphics_pipeline_bound_ || !graphics_bindless_enabled_) {
        AETHER_LOG_ERROR("Vulkan", "BindBindlessTextures requires an active pass and a bindless graphics pipeline");
        return;
    }
    VkDescriptorSet sets[2] = {device_.BindlessTextureSet(), device_.BindlessSamplerSet()};
    vkCmdBindDescriptorSets(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, bound_pipeline_layout_, 0, 2, sets, 0,
                             nullptr);
    graphics_bindless_bound_ = true;
}

} // namespace aether::gfx::rhi::vulkan_backend
