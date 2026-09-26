// Proves the RHI abstraction (engine/include/aether/gfx/rhi) genuinely
// swaps backends: the exact same frame loop, written entirely against
// IDevice/ISwapChain/ICommandList, runs on both D3D12 and Vulkan depending
// on the AETHER_RHI_BACKEND environment variable ("d3d12" or "vulkan",
// default d3d12).
//
// Two demo modes:
//
//  - Default: clear the backbuffer to a color and present, every frame. No
//    shaders, no pipelines, no draw calls — this is the "does the device/
//    swapchain/command-submission layer actually work on both backends"
//    proof (see the RHI header comments for why shaders/pipelines aren't
//    abstracted).
//
//  - AETHER_RHI_DEMO_DRAW_TRIANGLE=1: draws an actual rotating triangle,
//    using the *same* HLSL source compiled two ways — D3DCompile (fxc) to
//    DXIL for the D3D12 root-signature/PSO path, and DXC to SPIR-V for the
//    Vulkan render-pass/pipeline path (see aether/gfx/shader_compiler.h).
//    This intentionally does NOT go through ICommandList::TransitionTexture/
//    ClearRenderTarget: those two calls exist only to support the
//    clear-to-color mode above (see ToVkImageLayout's comment in
//    vulkan_backend.cpp for why RenderTarget maps to TRANSFER_DST there,
//    which is the wrong Vulkan layout for a render-pass color attachment).
//    Triangle mode instead records genuinely backend-specific commands via
//    each object's NativeHandle() escape hatch, exactly as the RHI headers'
//    "shaders/pipelines are not abstracted" comments anticipate.
//
// Set AETHER_RHI_DEMO_MAX_FRAMES=<N> to auto-close after N frames instead of
// waiting for the window to be closed, for scripted/automated verification.

#include "aether/core/log.h"
#include "aether/gfx/d3d12_common.h"
#include "aether/gfx/rhi/d3d12/d3d12_backend.h"
#include "aether/gfx/rhi/device.h"
#include "aether/gfx/shader_compiler.h"
#include "aether/gfx/swap_chain.h"
#include "aether/platform/window.h"

#if defined(AETHER_HAS_VULKAN)
#include "aether/gfx/rhi/vulkan/vulkan_backend.h"
#endif

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace aether;
using namespace aether::gfx::rhi;

namespace {

// Procedural triangle, no vertex buffer (indices come from SV_VertexID /
// gl_VertexIndex, which DXC maps automatically). A single root/push constant
// (the elapsed time) drives a 2D rotation, just so both backends visibly
// have to get *some* per-frame data to the shader correctly, not just a
// static draw.
constexpr const char* kTriangleShaderSource = R"(
cbuffer PushConstants : register(b0) {
    float g_Time;
};

struct PSInput {
    float4 position : SV_POSITION;
    float3 color : COLOR0;
};

static const float2 kPositions[3] = {
    float2(0.0, 0.5), float2(0.5, -0.5), float2(-0.5, -0.5)
};
static const float3 kColors[3] = {
    float3(1.0, 0.2, 0.2), float3(0.2, 1.0, 0.2), float3(0.2, 0.4, 1.0)
};

PSInput VSMain(uint vertexID : SV_VertexID) {
    float c = cos(g_Time);
    float s = sin(g_Time);
    float2 pos = kPositions[vertexID];
    float2 rotated = float2(pos.x * c - pos.y * s, pos.x * s + pos.y * c);

    PSInput result;
    result.position = float4(rotated, 0.0, 1.0);
    result.color = kColors[vertexID];
    return result;
}

float4 PSMain(PSInput input) : SV_TARGET {
    return float4(input.color, 1.0);
}
)";

// ---------------------------------------------------------------------
// D3D12 triangle pipeline
// ---------------------------------------------------------------------

struct D3D12TriangleResources {
    gfx::ComPtr<ID3D12RootSignature> root_signature;
    gfx::ComPtr<ID3D12PipelineState> pso;
};

D3D12TriangleResources CreateD3D12TriangleResources(ID3D12Device* device, DXGI_FORMAT rtv_format) {
    using gfx::ShaderBytecode;

    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants = {/*ShaderRegister=*/0, /*RegisterSpace=*/0, /*Num32BitValues=*/1};
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 1;
    rs_desc.pParameters = &param;
    rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    gfx::ComPtr<ID3DBlob> signature;
    gfx::ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) {
        const char* message = error ? static_cast<const char*>(error->GetBufferPointer()) : "(no error blob)";
        AETHER_LOG_FATAL("RHIDemo", "Root signature serialization failed: %s", message);
        throw std::runtime_error("root signature serialization failed");
    }

    D3D12TriangleResources result;
    AETHER_D3D_CHECK(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                                  IID_PPV_ARGS(&result.root_signature)));

    ShaderBytecode vs = gfx::CompileHLSL(kTriangleShaderSource, "VSMain", "vs_5_0", "triangle_vs");
    ShaderBytecode ps = gfx::CompileHLSL(kTriangleShaderSource, "PSMain", "ps_5_0", "triangle_ps");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = result.root_signature.Get();
    pso_desc.VS = {vs.Data(), vs.Size()};
    pso_desc.PS = {ps.Data(), ps.Size()};
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.DepthStencilState.DepthEnable = FALSE;
    pso_desc.DepthStencilState.StencilEnable = FALSE;
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = rtv_format;
    pso_desc.SampleDesc.Count = 1;

    AETHER_D3D_CHECK(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&result.pso)));
    return result;
}

void RecordD3D12TriangleFrame(ICommandList& cmd, gfx::SwapChain& native_swap, const D3D12TriangleResources& res,
                               bool used_before, f32 time) {
    auto* native_cmd = static_cast<ID3D12GraphicsCommandList*>(cmd.NativeHandle());
    ID3D12Resource* backbuffer = native_swap.CurrentBackBuffer();
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = native_swap.CurrentBackBufferRTV();

    D3D12_RESOURCE_STATES before_state = used_before ? D3D12_RESOURCE_STATE_PRESENT : D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_BARRIER to_rt =
        gfx::TransitionBarrier(backbuffer, before_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    native_cmd->ResourceBarrier(1, &to_rt);

    const f32 clear[4] = {0.02f, 0.02f, 0.05f, 1.0f};
    native_cmd->ClearRenderTargetView(rtv, clear, 0, nullptr);
    native_cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    D3D12_VIEWPORT viewport{0.0f,
                             0.0f,
                             static_cast<f32>(native_swap.Width()),
                             static_cast<f32>(native_swap.Height()),
                             0.0f,
                             1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(native_swap.Width()), static_cast<LONG>(native_swap.Height())};
    native_cmd->RSSetViewports(1, &viewport);
    native_cmd->RSSetScissorRects(1, &scissor);

    native_cmd->SetGraphicsRootSignature(res.root_signature.Get());
    native_cmd->SetPipelineState(res.pso.Get());
    u32 time_bits;
    std::memcpy(&time_bits, &time, sizeof(time_bits));
    native_cmd->SetGraphicsRoot32BitConstant(0, time_bits, 0);
    native_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    native_cmd->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER to_present =
        gfx::TransitionBarrier(backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    native_cmd->ResourceBarrier(1, &to_present);
}

// ---------------------------------------------------------------------
// Vulkan triangle pipeline
// ---------------------------------------------------------------------

#if defined(AETHER_HAS_VULKAN)

namespace vulkan_backend = aether::gfx::rhi::vulkan_backend;

void VkDemoCheck(VkResult result, const char* expr) {
    if (result != VK_SUCCESS) {
        AETHER_LOG_FATAL("RHIDemo", "%s failed (VkResult=%d)", expr, static_cast<int>(result));
        throw std::runtime_error(expr);
    }
}
#define AETHER_RHI_DEMO_VK_CHECK(expr) ::VkDemoCheck((expr), #expr)

VkShaderModule CreateShaderModule(VkDevice device, const gfx::ShaderBytecode& bytecode) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytecode.Size();
    info.pCode = reinterpret_cast<const u32*>(bytecode.Data());
    VkShaderModule module = VK_NULL_HANDLE;
    AETHER_RHI_DEMO_VK_CHECK(vkCreateShaderModule(device, &info, nullptr, &module));
    return module;
}

// Owns a render pass + pipeline (both resize-independent: viewport/scissor
// are dynamic state) plus one framebuffer per swapchain image (NOT
// resize-independent — rebuilt via RebuildFramebuffers whenever the
// swapchain's images are recreated).
struct VulkanTriangleResources {
    VkDevice device = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers;
    // The actual VkImageView handles each framebuffer[i] was built from —
    // NOT just width/height/count. VulkanSwapChain::Resize() unconditionally
    // destroys and recreates every VkImage/VkImageView, even when the
    // surface capabilities clamp the requested extent back to the same
    // width/height (e.g. this demo's programmatic resize test, which resizes
    // the swapchain without resizing the actual HWND — the surface's
    // min/maxImageExtent then just forces the old size straight back). A
    // dimension-only cache would keep a framebuffer bound to views that were
    // already destroyed underneath it — this is exactly that bug, found by
    // actually running the resize test rather than trusting the dimension
    // check to be sufficient.
    std::vector<VkImageView> framebuffer_views;

    VulkanTriangleResources(const VulkanTriangleResources&) = delete;
    VulkanTriangleResources& operator=(const VulkanTriangleResources&) = delete;
    VulkanTriangleResources() = default;

    ~VulkanTriangleResources() {
        DestroyFramebuffers();
        if (pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, pipeline, nullptr);
        }
        if (pipeline_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        }
        if (render_pass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device, render_pass, nullptr);
        }
    }

    void DestroyFramebuffers() {
        for (VkFramebuffer fb : framebuffers) {
            vkDestroyFramebuffer(device, fb, nullptr);
        }
        framebuffers.clear();
        framebuffer_views.clear();
    }

    // Called every frame; cheap no-op unless the swapchain actually
    // recreated its images (detected via a width/height change, since this
    // demo only resizes explicitly — see main()'s comment on test_resize).
    void EnsureFramebuffers(vulkan_backend::VulkanSwapChain& native_swap, u32 buffer_count) {
        u32 width = native_swap.Width();
        u32 height = native_swap.Height();

        bool needs_rebuild = framebuffers.empty() || framebuffer_views.size() != buffer_count;
        if (!needs_rebuild) {
            for (u32 i = 0; i < buffer_count; ++i) {
                if (framebuffer_views[i] != native_swap.ImageView(i)) {
                    needs_rebuild = true;
                    break;
                }
            }
        }
        if (!needs_rebuild) {
            return;
        }

        DestroyFramebuffers();
        framebuffers.resize(buffer_count);
        framebuffer_views.resize(buffer_count);
        for (u32 i = 0; i < buffer_count; ++i) {
            VkImageView view = native_swap.ImageView(i);
            VkFramebufferCreateInfo fb_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fb_info.renderPass = render_pass;
            fb_info.attachmentCount = 1;
            fb_info.pAttachments = &view;
            fb_info.width = width;
            fb_info.height = height;
            fb_info.layers = 1;
            AETHER_RHI_DEMO_VK_CHECK(vkCreateFramebuffer(device, &fb_info, nullptr, &framebuffers[i]));
            framebuffer_views[i] = view;
        }
    }
};

std::unique_ptr<VulkanTriangleResources> CreateVulkanTriangleResources(VkDevice device, VkFormat color_format) {
    auto res = std::make_unique<VulkanTriangleResources>();
    res->device = device;

    // Every frame clears via loadOp=CLEAR and discards prior contents, so
    // the attachment's initialLayout can always be UNDEFINED regardless of
    // what layout the image was actually left in last frame — no per-frame
    // layout bookkeeping needed, unlike the clear-to-color path.
    VkAttachmentDescription attachment{};
    attachment.format = color_format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    // Synchronizes subpass 0 with the swapchain's image-available semaphore,
    // which VulkanDevice::Submit waits on at COLOR_ATTACHMENT_OUTPUT — the
    // same stage this dependency gates, so the render pass never starts
    // writing before the image is actually available.
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp_info.attachmentCount = 1;
    rp_info.pAttachments = &attachment;
    rp_info.subpassCount = 1;
    rp_info.pSubpasses = &subpass;
    rp_info.dependencyCount = 1;
    rp_info.pDependencies = &dependency;
    AETHER_RHI_DEMO_VK_CHECK(vkCreateRenderPass(device, &rp_info, nullptr, &res->render_pass));

    VkPushConstantRange push_range{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(f32)};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;
    AETHER_RHI_DEMO_VK_CHECK(vkCreatePipelineLayout(device, &layout_info, nullptr, &res->pipeline_layout));

    gfx::ShaderBytecode vs_spirv = gfx::CompileHLSLToSPIRV(kTriangleShaderSource, "VSMain", "vs_6_0", "triangle_vs_spirv");
    gfx::ShaderBytecode ps_spirv = gfx::CompileHLSLToSPIRV(kTriangleShaderSource, "PSMain", "ps_6_0", "triangle_ps_spirv");
    VkShaderModule vs_module = CreateShaderModule(device, vs_spirv);
    VkShaderModule ps_module = CreateShaderModule(device, ps_spirv);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs_module;
    stages[0].pName = "VSMain";
    stages[1] = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps_module;
    stages[1].pName = "PSMain";

    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

    VkPipelineInputAssemblyStateCreateInfo input_assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
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
    pipeline_info.layout = res->pipeline_layout;
    pipeline_info.renderPass = res->render_pass;
    pipeline_info.subpass = 0;

    VkResult pipeline_result =
        vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &res->pipeline);

    vkDestroyShaderModule(device, vs_module, nullptr);
    vkDestroyShaderModule(device, ps_module, nullptr);

    AETHER_RHI_DEMO_VK_CHECK(pipeline_result);

    return res;
}

void RecordVulkanTriangleFrame(ICommandList& cmd, vulkan_backend::VulkanSwapChain& native_swap,
                                VulkanTriangleResources& res, f32 time) {
    auto native_cmd = static_cast<VkCommandBuffer>(cmd.NativeHandle());
    u32 image_index = native_swap.CurrentImageIndex();

    VkClearValue clear_value{};
    clear_value.color.float32[0] = 0.02f;
    clear_value.color.float32[1] = 0.02f;
    clear_value.color.float32[2] = 0.05f;
    clear_value.color.float32[3] = 1.0f;

    VkRenderPassBeginInfo rp_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp_begin.renderPass = res.render_pass;
    rp_begin.framebuffer = res.framebuffers[image_index];
    rp_begin.renderArea.offset = {0, 0};
    rp_begin.renderArea.extent = {native_swap.Width(), native_swap.Height()};
    rp_begin.clearValueCount = 1;
    rp_begin.pClearValues = &clear_value;

    vkCmdBeginRenderPass(native_cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{
        0.0f, 0.0f, static_cast<f32>(native_swap.Width()), static_cast<f32>(native_swap.Height()), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {native_swap.Width(), native_swap.Height()}};
    vkCmdSetViewport(native_cmd, 0, 1, &viewport);
    vkCmdSetScissor(native_cmd, 0, 1, &scissor);

    vkCmdBindPipeline(native_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, res.pipeline);
    vkCmdPushConstants(native_cmd, res.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(f32), &time);
    vkCmdDraw(native_cmd, 3, 1, 0, 0);

    vkCmdEndRenderPass(native_cmd);
}

#endif // defined(AETHER_HAS_VULKAN)

} // namespace

int main() {
    Backend backend = Backend::D3D12;
    if (const char* env = std::getenv("AETHER_RHI_BACKEND")) {
        if (std::strcmp(env, "vulkan") == 0) {
            backend = Backend::Vulkan;
        }
    }

    i32 max_frames = -1;
    if (const char* env = std::getenv("AETHER_RHI_DEMO_MAX_FRAMES")) {
        max_frames = std::atoi(env);
    }

    bool draw_triangle = std::getenv("AETHER_RHI_DEMO_DRAW_TRIANGLE") != nullptr;

    try {
        if (!IsBackendAvailable(backend)) {
            AETHER_LOG_FATAL("RHIDemo", "Requested backend is not available in this build");
            return 1;
        }

        WindowDesc window_desc;
        window_desc.title = (backend == Backend::Vulkan) ? "Aether RHI Demo - Vulkan" : "Aether RHI Demo - D3D12";
        window_desc.width = 800;
        window_desc.height = 600;
        Window window(window_desc);

        std::unique_ptr<IDevice> device = CreateDevice(backend, /*enable_debug_layer=*/true);
        if (!device) {
            AETHER_LOG_FATAL("RHIDemo", "Failed to create device");
            return 1;
        }

        std::unique_ptr<ISwapChain> swap_chain =
            device->CreateSwapChain(window.NativeHandle(), window.Width(), window.Height());

        window.on_resize = [&](u32 w, u32 h) { swap_chain->Resize(w, h); };

        // Triangle-mode pipeline objects, created once up front for whichever
        // backend is active. Declared here (rather than deeper in main) so
        // they're destroyed, in reverse order, before `device`/`swap_chain`
        // go out of scope.
        D3D12TriangleResources d3d12_triangle;
#if defined(AETHER_HAS_VULKAN)
        std::unique_ptr<VulkanTriangleResources> vulkan_triangle;
#endif

        if (draw_triangle) {
            if (backend == Backend::D3D12) {
                auto* native_swap = static_cast<gfx::SwapChain*>(swap_chain->NativeHandle());
                d3d12_triangle = CreateD3D12TriangleResources(static_cast<ID3D12Device*>(device->NativeHandle()),
                                                               native_swap->Format());
            }
#if defined(AETHER_HAS_VULKAN)
            else {
                auto* native_swap = static_cast<vulkan_backend::VulkanSwapChain*>(swap_chain->NativeHandle());
                vulkan_triangle =
                    CreateVulkanTriangleResources(static_cast<VkDevice>(device->NativeHandle()), native_swap->Format());
                vulkan_triangle->EnsureFramebuffers(*native_swap, swap_chain->BufferCount());
            }
#endif
        }

        std::vector<std::unique_ptr<ICommandList>> command_lists;
        std::vector<u64> frame_fences(swap_chain->BufferCount(), 0);
        // Vulkan swap chain images start life in an undefined layout;
        // D3D12's convention (matching the rest of this engine) is to treat
        // a fresh backbuffer as already PRESENT. Tracking "have we used this
        // slot yet" lets the same code transition correctly on both
        // backends without the RHI needing a full RenderGraph-style
        // automatic-state-tracking layer of its own.
        std::vector<bool> used_before(swap_chain->BufferCount(), false);
        for (u32 i = 0; i < swap_chain->BufferCount(); ++i) {
            command_lists.push_back(device->CreateCommandList());
        }

        AETHER_LOG_INFO("RHIDemo", "Entering main loop (backend=%s, %u buffers, triangle=%s)",
                         backend == Backend::Vulkan ? "Vulkan" : "D3D12", swap_chain->BufferCount(),
                         draw_triangle ? "yes" : "no");

        bool test_resize = std::getenv("AETHER_RHI_DEMO_TEST_RESIZE") != nullptr;

        i32 frame_index = 0;
        f32 t = 0.0f;
        while (window.PumpMessages()) {
            if (window.IsMinimized()) {
                continue;
            }

            // Programmatic resize exercise, since triggering a real window
            // resize needs interactive input this demo can't script: proves
            // ISwapChain::Resize() (swapchain/image/semaphore recreation on
            // Vulkan, RTV recreation on D3D12) doesn't crash on either
            // backend without needing a human to drag the window edge.
            if (test_resize && frame_index == 10) {
                AETHER_LOG_INFO("RHIDemo", "Testing programmatic resize to 400x300");
                for (u64 fence : frame_fences) {
                    device->WaitForFence(fence);
                }
                swap_chain->Resize(400, 300);
                used_before.assign(swap_chain->BufferCount(), false);
            }

            swap_chain->AcquireNextImage();
            TextureHandle back_buffer = swap_chain->CurrentBackBuffer();

            // There's no direct "which slot is this" query on ISwapChain
            // (backends differ on whether that's the acquired image index or
            // a separate frame-in-flight counter — see VulkanSwapChain's
            // comments) so we key bookkeeping off the TextureHandle's own
            // index, which is stable and unique per backbuffer on both
            // backends.
            u32 slot = back_buffer.index % static_cast<u32>(frame_fences.size());
            device->WaitForFence(frame_fences[slot]);

            ICommandList& cmd = *command_lists[slot];
            cmd.Reset();

            t += 0.01f;

            if (draw_triangle) {
                if (backend == Backend::D3D12) {
                    auto* native_swap = static_cast<gfx::SwapChain*>(swap_chain->NativeHandle());
                    RecordD3D12TriangleFrame(cmd, *native_swap, d3d12_triangle, used_before[slot], t);
                }
#if defined(AETHER_HAS_VULKAN)
                else {
                    auto* native_swap = static_cast<vulkan_backend::VulkanSwapChain*>(swap_chain->NativeHandle());
                    vulkan_triangle->EnsureFramebuffers(*native_swap, swap_chain->BufferCount());
                    RecordVulkanTriangleFrame(cmd, *native_swap, *vulkan_triangle, t);
                }
#endif
                used_before[slot] = true;
            } else {
                ResourceState before = used_before[slot] ? ResourceState::Present : ResourceState::Undefined;
                cmd.TransitionTexture(back_buffer, before, ResourceState::RenderTarget);

                f32 pulse = 0.5f + 0.5f * std::sin(t);
                cmd.ClearRenderTarget(back_buffer, {0.05f, 0.05f * pulse, 0.15f + 0.1f * pulse, 1.0f});

                cmd.TransitionTexture(back_buffer, ResourceState::RenderTarget, ResourceState::Present);
                used_before[slot] = true;
            }

            cmd.Close();
            frame_fences[slot] = device->Submit(cmd, swap_chain.get());
            swap_chain->Present(/*vsync=*/true);

            ++frame_index;
            if (max_frames >= 0 && frame_index >= max_frames) {
                AETHER_LOG_INFO("RHIDemo", "Reached AETHER_RHI_DEMO_MAX_FRAMES=%d, exiting", max_frames);
                break;
            }
        }

        for (u64 fence : frame_fences) {
            device->WaitForFence(fence);
        }

        AETHER_LOG_INFO("RHIDemo", "Shutting down cleanly");
    } catch (const std::exception& e) {
        AETHER_LOG_FATAL("RHIDemo", "Unhandled exception: %s", e.what());
        return 1;
    }

    return 0;
}
