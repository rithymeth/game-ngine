#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"
#include "test_framework.h"

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace aether;
namespace rhi = aether::gfx::rhi;

// The bindless table holds 128 textures (it held 32; the HDR target, the depth copy and the bloom chain took seven of them). Every slot
// must be both allocatable and readable by a shader declaring the full array: fill the table, then draw the texture in its last slot.

namespace {

constexpr char kSampleShader[] = R"(
struct PushConstants { uint texture_index; uint pad0; uint pad1; uint pad2; };
#ifdef __spirv__
[[vk::push_constant]]
#endif
ConstantBuffer<PushConstants> g_PC : register(b0);
Texture2D g_Textures[128] : register(t0, space0);
SamplerState g_Sampler : register(s0, space1);
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float4 PSMain(float4 position : SV_Position) : SV_Target {
    return g_Textures[g_PC.texture_index].Load(int3(0, 0, 0));
}
)";

void FillsAndSamplesTheWholeTable(rhi::Backend backend) {
    if (!rhi::IsBackendAvailable(backend)) return;
    std::unique_ptr<rhi::IDevice> device;
    try {
        device = rhi::CreateDevice(backend, false);
    } catch (const std::exception&) {
    }
    if (!device) {
        std::printf("    (no device: skipping)\n");
        return;
    }
    std::unique_ptr<rhi::ISwapChain> target = device->CreateSwapChain(nullptr, 16, 16, 2);
    static_assert(rhi::kMaxBindlessTextures == 128);

    // Grey fillers, then one green texture: as many as the table takes, and then one more, refused.
    const u8 grey[4] = {90, 90, 90, 255}, green[4] = {0, 255, 0, 255};
    std::vector<rhi::SampledTextureHandle> textures;
    for (u32 i = 0; i + 1 < rhi::kMaxUserBindlessTextures; ++i) {
        const rhi::SampledTextureHandle t = device->CreateTexture(1, 1, grey);
        if (!t.IsValid()) break;
        textures.push_back(t);
    }
    const rhi::SampledTextureHandle last = device->CreateTexture(1, 1, green);
    AETHER_CHECK(textures.size() == rhi::kMaxUserBindlessTextures - 1 && last.IsValid());
    if (!last.IsValid()) return;
    AETHER_CHECK(last.index == rhi::kMaxBindlessTextures - 1); // the table's last slot, past every old 6-bit and 32-slot limit
    AETHER_CHECK(!device->CreateTexture(1, 1, grey).IsValid()); // full: refused, not out of bounds

    rhi::PipelineDesc desc;
    desc.hlsl_source = kSampleShader;
    desc.push_constant_size_bytes = 16;
    desc.enable_bindless_textures = true;
    const rhi::PipelineHandle pipeline = device->CreatePipeline(desc, *target);
    AETHER_CHECK(pipeline.IsValid());
    if (pipeline.IsValid()) {
        std::unique_ptr<rhi::ICommandList> cmd = device->CreateCommandList();
        target->AcquireNextImage();
        cmd->Reset();
        cmd->BeginRenderPass(*target, {1, 0, 1, 1});
        cmd->BindPipeline(pipeline);
        cmd->BindBindlessTextures();
        const u32 push[4] = {last.index, 0, 0, 0};
        cmd->SetPushConstants(push, sizeof(push));
        cmd->Draw(3);
        cmd->EndRenderPass();
        cmd->Close();
        device->WaitForFence(device->Submit(*cmd, target.get()));
        std::vector<u8> pixels;
        AETHER_CHECK(target->ReadBack(pixels) && pixels.size() == 16 * 16 * 4);
        if (pixels.size() == 16 * 16 * 4) {
            const u8* p = &pixels[(8 * 16 + 8) * 4];
            AETHER_CHECK(p[1] > 250 && p[0] < 5 && p[2] < 5); // green, whatever the channel order of red and blue
        }
        target->Present(false);
        device->DestroyPipeline(pipeline);
    }
    for (const rhi::SampledTextureHandle t : textures) device->DestroyTexture(t);
    device->DestroyTexture(last);
}

} // namespace

AETHER_TEST(BindlessTable_FillsAndSamplesTheLastSlotOnD3D12) { FillsAndSamplesTheWholeTable(rhi::Backend::D3D12); }
AETHER_TEST(BindlessTable_FillsAndSamplesTheLastSlotOnVulkan) { FillsAndSamplesTheWholeTable(rhi::Backend::Vulkan); }

// A bindless pipeline without push constants has its texture table as the first root parameter: binding it must not assume the second
// (D3D12 did, binding the table onto whatever came next, or past the end, and the draw crashed the device).
AETHER_TEST(BindlessTable_BindsWithoutPushConstantsOnD3D12) {
    if (!rhi::IsBackendAvailable(rhi::Backend::D3D12)) return;
    std::unique_ptr<rhi::IDevice> device;
    try {
        device = rhi::CreateDevice(rhi::Backend::D3D12, false);
    } catch (const std::exception&) {
    }
    if (!device) return;
    std::unique_ptr<rhi::ISwapChain> target = device->CreateSwapChain(nullptr, 16, 16, 2);
    const u8 green[4] = {0, 255, 0, 255};
    const rhi::SampledTextureHandle texture = device->CreateTexture(1, 1, green);
    AETHER_CHECK(texture.IsValid());
    if (!texture.IsValid()) return;
    rhi::PipelineDesc desc;
    desc.hlsl_source = R"(
Texture2D g_Textures[128] : register(t0, space0);
SamplerState g_Sampler : register(s0, space1);
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float4 PSMain(float4 position : SV_Position) : SV_Target { return g_Textures[)" + std::to_string(texture.index) + R"(].Load(int3(0, 0, 0)); }
)";
    desc.enable_bindless_textures = true; // and no push constants
    const rhi::PipelineHandle pipeline = device->CreatePipeline(desc, *target);
    AETHER_CHECK(pipeline.IsValid());
    if (pipeline.IsValid()) {
        std::unique_ptr<rhi::ICommandList> cmd = device->CreateCommandList();
        target->AcquireNextImage();
        cmd->Reset();
        cmd->BeginRenderPass(*target, {1, 0, 1, 1});
        cmd->BindPipeline(pipeline);
        cmd->BindBindlessTextures();
        cmd->Draw(3);
        cmd->EndRenderPass();
        cmd->Close();
        device->WaitForFence(device->Submit(*cmd, target.get()));
        std::vector<u8> pixels;
        AETHER_CHECK(target->ReadBack(pixels) && pixels.size() == 16 * 16 * 4);
        if (pixels.size() == 16 * 16 * 4) AETHER_CHECK(pixels[(8 * 16 + 8) * 4 + 1] > 250 && pixels[(8 * 16 + 8) * 4] < 5);
        target->Present(false);
        device->DestroyPipeline(pipeline);
    }
    device->DestroyTexture(texture);
}
