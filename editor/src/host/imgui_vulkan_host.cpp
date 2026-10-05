#include "host/imgui_vulkan_host.h"

#include "aether/core/log.h"
#include "aether/gfx/rhi/vulkan/vulkan_backend.h"

#include <imgui_impl_vulkan.h>

#include <stdexcept>

namespace aether::editor {

namespace {

void CheckVk(VkResult result) {
    if (result != VK_SUCCESS) {
        AETHER_LOG_ERROR("Editor", "ImGui Vulkan backend: VkResult %d", static_cast<int>(result));
    }
}

} // namespace

ImGuiVulkanHost::ImGuiVulkanHost(rhi::IDevice& device, rhi::ISwapChain& swap_chain) : device_(device) {
    if (device.GetBackend() != rhi::Backend::Vulkan) {
        throw std::runtime_error("ImGuiVulkanHost needs the Vulkan RHI backend");
    }
    auto& vk_device = static_cast<gfx::rhi::vulkan_backend::VulkanDevice&>(device);
    auto* vk_swap = static_cast<gfx::rhi::vulkan_backend::VulkanSwapChain*>(swap_chain.NativeHandle());

    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_2;
    info.Instance = vk_device.Instance();
    info.PhysicalDevice = vk_device.PhysicalDevice();
    info.Device = vk_device.Handle();
    info.QueueFamily = vk_device.QueueFamilyIndex();
    info.Queue = vk_device.Queue();
    info.DescriptorPoolSize = 64; // the backend's own pool: the font atlas and any UI textures
    const u32 images = swap_chain.BufferCount() < 2 ? 2 : swap_chain.BufferCount();
    info.MinImageCount = images;
    info.ImageCount = images;
    info.PipelineInfoMain.RenderPass = vk_swap->DefaultRenderPass();
    info.PipelineInfoMain.Subpass = 0;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.CheckVkResultFn = CheckVk;
    if (!ImGui_ImplVulkan_Init(&info)) {
        throw std::runtime_error("ImGui_ImplVulkan_Init failed");
    }
}

ImGuiVulkanHost::~ImGuiVulkanHost() {
    vkDeviceWaitIdle(static_cast<VkDevice>(device_.NativeHandle()));
    ImGui_ImplVulkan_Shutdown();
}

void ImGuiVulkanHost::NewFrame() {
    ImGui_ImplVulkan_NewFrame();
}

void ImGuiVulkanHost::Render(rhi::ICommandList& cmd, ImDrawData* draw_data) {
    if (draw_data) {
        ImGui_ImplVulkan_RenderDrawData(draw_data, static_cast<VkCommandBuffer>(cmd.NativeHandle()));
    }
}

} // namespace aether::editor
