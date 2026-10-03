#pragma once

#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"
#include "aether/gfx/rhi/swap_chain.h"

#include <imgui.h>

namespace aether::editor {

namespace rhi = gfx::rhi;

// Dear ImGui drawn through the RHI's Vulkan backend (Phase 24,
// docs/design/PHASE_SPECS.md §24.4): imgui_impl_vulkan on the engine's own
// VkDevice and queue, its pipeline built for the swap chain's default render
// pass, and draw lists recorded into an RHI command list between
// BeginRenderPass and EndRenderPass. Works the same for window and
// offscreen swap chains, so the editor UI renders on lavapipe in CI.
// One per ImGui context; the context must exist first.
class ImGuiVulkanHost {
public:
    ImGuiVulkanHost(rhi::IDevice& device, rhi::ISwapChain& swap_chain);
    ~ImGuiVulkanHost();

    ImGuiVulkanHost(const ImGuiVulkanHost&) = delete;
    ImGuiVulkanHost& operator=(const ImGuiVulkanHost&) = delete;

    // Before ImGui::NewFrame.
    void NewFrame();
    // After ImGui::Render, inside the swap chain's render pass.
    void Render(rhi::ICommandList& cmd, ImDrawData* draw_data);

private:
    rhi::IDevice& device_;
};

} // namespace aether::editor
