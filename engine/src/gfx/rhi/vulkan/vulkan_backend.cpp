#include "aether/gfx/rhi/vulkan/vulkan_backend.h"

#include "aether/core/log.h"
#include "aether/gfx/shader_compiler.h"

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
            bool present = vkGetPhysicalDeviceWin32PresentationSupportKHR(device, i) == VK_TRUE;
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
} // namespace

PipelineHandle VulkanDevice::CreatePipeline(const PipelineDesc& desc, ISwapChain& swap_chain) {
    auto& vk_swap = static_cast<VulkanSwapChain&>(swap_chain);

    PipelineRecord record;

    VkPushConstantRange push_range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                    desc.push_constant_size_bytes};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    if (desc.push_constant_size_bytes > 0) {
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push_range;
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

    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

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

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend_attachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo blend_state{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend_state.attachmentCount = 1;
    blend_state.pAttachments = &blend_attachment;

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

    if (wait_on_swap_chain) {
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
    : device_(device), hwnd_(native_window_handle), requested_buffer_count_(buffer_count) {
    VkWin32SurfaceCreateInfoKHR surface_info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surface_info.hinstance = GetModuleHandleW(nullptr);
    surface_info.hwnd = static_cast<HWND>(hwnd_);
    AETHER_VK_CHECK(vkCreateWin32SurfaceKHR(device_.Instance(), &surface_info, nullptr, &surface_));

    VkBool32 present_supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(device_.PhysicalDevice(), device_.QueueFamilyIndex(), surface_,
                                          &present_supported);
    AETHER_ASSERT(present_supported == VK_TRUE);

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

    VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    // Synchronizes subpass 0 with the swapchain's image-available semaphore,
    // which VulkanDevice::Submit waits on at COLOR_ATTACHMENT_OUTPUT.
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp_info.attachmentCount = 1;
    rp_info.pAttachments = &color_attachment;
    rp_info.subpassCount = 1;
    rp_info.pSubpasses = &subpass;
    rp_info.dependencyCount = 1;
    rp_info.pDependencies = &dependency;
    AETHER_VK_CHECK(vkCreateRenderPass(device_.Handle(), &rp_info, nullptr, &default_render_pass_));
}

void VulkanSwapChain::CreateFramebuffers() {
    framebuffers_.resize(image_views_.size());
    for (usize i = 0; i < image_views_.size(); ++i) {
        VkImageView attachments[] = {image_views_[i]};
        VkFramebufferCreateInfo fb_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb_info.renderPass = default_render_pass_;
        fb_info.attachmentCount = 1;
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

    CreateFramebuffers();
}

void VulkanSwapChain::DestroySwapchainAndImages() {
    DestroyFramebuffers();
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

    VkClearValue clear_value{};
    clear_value.color.float32[0] = clear_color.r;
    clear_value.color.float32[1] = clear_color.g;
    clear_value.color.float32[2] = clear_color.b;
    clear_value.color.float32[3] = clear_color.a;

    VkRenderPassBeginInfo rp_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp_begin.renderPass = vk_swap.DefaultRenderPass();
    rp_begin.framebuffer = vk_swap.CurrentFramebuffer();
    rp_begin.renderArea.offset = {0, 0};
    rp_begin.renderArea.extent = {vk_swap.Width(), vk_swap.Height()};
    rp_begin.clearValueCount = 1;
    rp_begin.pClearValues = &clear_value;
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

} // namespace aether::gfx::rhi::vulkan_backend
