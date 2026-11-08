#include "aether/assets/gltf_loader.h"
#include "aether/assets/image.h"
#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"
#include "aether/player/scene_renderer.h"
#include "test_framework.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace aether;
namespace rhi = aether::gfx::rhi;

// Loader fixes: mip chains for the textures the player decodes itself (assets::BuildMipChain, IDevice::CreateTextureWithMips), the
// D3D12 sampler reaching them, and glTF's doubleSided (back faces culled on single-sided materials, lit from behind on double-sided ones).

namespace {

std::unique_ptr<rhi::IDevice> MakeDevice(rhi::Backend backend) {
    std::unique_ptr<rhi::IDevice> device;
    if (rhi::IsBackendAvailable(backend)) {
        try {
            device = rhi::CreateDevice(backend, false);
        } catch (const std::exception&) {
        }
    }
    if (!device) std::printf("    (no %s device: skipping)\n", backend == rhi::Backend::Vulkan ? "Vulkan" : "D3D12");
    return device;
}

// Two triangles side by side in NDC (y up): the left one counter-clockwise as seen, the right one clockwise.
constexpr char kWindingShader[] = R"(
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    const float2 p[6] = {float2(-0.9, -0.5), float2(-0.1, -0.5), float2(-0.5, 0.5),
                         float2(0.1, -0.5), float2(0.5, 0.5), float2(0.9, -0.5)};
    return float4(p[id], 0.5, 1.0);
}
float4 PSMain(float4 position : SV_Position) : SV_Target { return float4(0, 1, 0, 1); }
)";

// Which of the two triangles a pipeline with cull_back_face draws: bit 0 the counter-clockwise one, bit 1 the clockwise one.
int DrawnTriangles(rhi::IDevice& device, bool cull) {
    constexpr u32 kSize = 64;
    std::unique_ptr<rhi::ISwapChain> target = device.CreateSwapChain(nullptr, kSize, kSize, 2);
    rhi::PipelineDesc desc;
    desc.hlsl_source = kWindingShader;
    desc.cull_back_face = cull;
    const rhi::PipelineHandle pipeline = device.CreatePipeline(desc, *target);
    if (!pipeline.IsValid()) return -1;
    std::unique_ptr<rhi::ICommandList> cmd = device.CreateCommandList();
    target->AcquireNextImage();
    cmd->Reset();
    cmd->BeginRenderPass(*target, {0, 0, 0, 1});
    cmd->BindPipeline(pipeline);
    cmd->Draw(6);
    cmd->EndRenderPass();
    cmd->Close();
    device.WaitForFence(device.Submit(*cmd, target.get()));
    std::vector<u8> pixels;
    const bool read = target->ReadBack(pixels) && pixels.size() == kSize * kSize * 4;
    target->Present(false);
    device.DestroyPipeline(pipeline);
    if (!read) return -1;
    const auto green = [&](u32 x, u32 y) { return pixels[(static_cast<usize>(y) * kSize + x) * 4 + 1] > 128; };
    return (green(16, 36) ? 1 : 0) | (green(48, 36) ? 2 : 0);
}

} // namespace

AETHER_TEST(Material_CullingKeepsCounterClockwiseFaces) {
    for (const rhi::Backend backend : {rhi::Backend::D3D12, rhi::Backend::Vulkan}) {
        std::unique_ptr<rhi::IDevice> device = MakeDevice(backend);
        if (!device) continue;
        AETHER_CHECK(DrawnTriangles(*device, false) == 3);
        // glTF's convention: counter-clockwise is the front.
        const int culled = DrawnTriangles(*device, true);
        if (culled != 1) std::printf("    backend %d with culling drew %d\n", static_cast<int>(backend), culled);
        AETHER_CHECK(culled == 1);
    }
}

AETHER_TEST(Material_MipChainsAverageEachKindOfTexture) {
    u32 levels = 0;
    // 4x2: levels 4x2, 2x1, 1x1, packed one after another.
    std::vector<u8> pixels(4 * 2 * 4, 255);
    std::vector<u8> chain = assets::BuildMipChain(4, 2, pixels, assets::MipFilter::Srgb, levels);
    AETHER_CHECK(levels == 3 && chain.size() == (8 + 2 + 1) * 4);
    // Black and white side by side: in light, half way is 0.5, which sRGB stores as 188; plain data stores 128.
    const std::vector<u8> pair = {0, 0, 0, 255, 255, 255, 255, 255};
    chain = assets::BuildMipChain(2, 1, pair, assets::MipFilter::Srgb, levels);
    AETHER_CHECK(levels == 2 && chain.size() == 12 && chain[8] == 188 && chain[11] == 255);
    chain = assets::BuildMipChain(2, 1, pair, assets::MipFilter::Linear, levels);
    AETHER_CHECK(chain.size() == 12 && (chain[8] == 127 || chain[8] == 128));
    // Normals tilted left and right average to straight up, renormalised (not shortened).
    const std::vector<u8> normals = {37, 128, 218, 255, 218, 128, 218, 255}; // (-0.7, 0, 0.7) and (0.7, 0, 0.7)
    chain = assets::BuildMipChain(2, 1, normals, assets::MipFilter::NormalMap, levels);
    AETHER_CHECK(chain.size() == 12 && chain[8] >= 126 && chain[8] <= 129 && chain[10] >= 253);
    // An odd size halves down, and the clamped edge is still averaged in; a size that doesn't match the pixels gives nothing.
    chain = assets::BuildMipChain(3, 3, std::vector<u8>(36, 100), assets::MipFilter::Linear, levels);
    AETHER_CHECK(levels == 2 && chain.size() == (9 + 1) * 4 && chain[36] == 100);
    chain = assets::BuildMipChain(3, 3, std::vector<u8>(8, 0), assets::MipFilter::Linear, levels);
    AETHER_CHECK(chain.empty() && levels == 0);
}

AETHER_TEST(Material_TexturesWithMipsAreSampledAtEveryLevel) {
    for (const rhi::Backend backend : {rhi::Backend::D3D12, rhi::Backend::Vulkan}) {
        std::unique_ptr<rhi::IDevice> device = MakeDevice(backend);
        if (!device) continue;
        // 4x2 with three levels: red, green, blue.
        std::vector<u8> levels;
        const auto fill = [&](usize texels, u8 r, u8 g, u8 b) {
            for (usize i = 0; i < texels; ++i) levels.insert(levels.end(), {r, g, b, 255});
        };
        fill(8, 255, 0, 0);
        fill(2, 0, 255, 0);
        fill(1, 0, 0, 255);
        const rhi::SampledTextureHandle texture = device->CreateTextureWithMips(4, 2, 3, levels.data());
        AETHER_CHECK(texture.IsValid());
        if (!texture.IsValid()) continue;
        constexpr u32 kSize = 48;
        std::unique_ptr<rhi::ISwapChain> target = device->CreateSwapChain(nullptr, kSize, kSize, 2);
        rhi::PipelineDesc desc;
        desc.hlsl_source = R"(
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
    return float4(g_Textures[g_PC.texture_index].SampleLevel(g_Sampler, float2(0.5, 0.5), floor(position.x / 16.0)).rgb, 1.0);
}
)";
        desc.push_constant_size_bytes = 16;
        desc.enable_bindless_textures = true;
        const rhi::PipelineHandle pipeline = device->CreatePipeline(desc, *target);
        std::unique_ptr<rhi::ICommandList> cmd = device->CreateCommandList();
        target->AcquireNextImage();
        cmd->Reset();
        cmd->BeginRenderPass(*target, {1, 1, 1, 1});
        cmd->BindPipeline(pipeline);
        cmd->BindBindlessTextures();
        const u32 push[4] = {texture.index, 0, 0, 0};
        cmd->SetPushConstants(push, sizeof(push));
        cmd->Draw(3);
        cmd->EndRenderPass();
        cmd->Close();
        device->WaitForFence(device->Submit(*cmd, target.get()));
        std::vector<u8> pixels;
        AETHER_CHECK(target->ReadBack(pixels) && pixels.size() == kSize * kSize * 4);
        target->Present(false);
        if (pixels.size() == kSize * kSize * 4) {
            const auto at = [&](u32 x, u32 c) { return pixels[(static_cast<usize>(kSize / 2) * kSize + x) * 4 + c]; };
            AETHER_CHECK(at(8, 0) > 240 && at(8, 1) < 16); // level 0
            // Both backends keep the chain: each band reads its own level.
            AETHER_CHECK(at(24, 1) > 240 && at(24, 0) < 16);
            AETHER_CHECK(at(40, 2) > 240 && at(40, 0) < 16);
        }
        device->DestroyPipeline(pipeline);
        device->DestroyTexture(texture);
    }
}

AETHER_TEST(Material_GltfDoubleSidedIsRead) {
    // One triangle (three float positions, base64), two materials: the first double-sided.
    const std::string document = R"({"asset":{"version":"2.0"},
        "buffers":[{"byteLength":36,"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}],
        "bufferViews":[{"buffer":0,"byteLength":36}],
        "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}],
        "materials":[{"doubleSided":true},{"doubleSided":"yes"}],
        "meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0}]}],
        "nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
    assets::GltfScene scene;
    AETHER_CHECK(assets::LoadGltfFromMemory(std::span<const u8>(reinterpret_cast<const u8*>(document.data()), document.size()), scene));
    AETHER_CHECK(scene.materials.size() == 2);
    if (scene.materials.size() == 2) AETHER_CHECK(scene.materials[0].double_sided && !scene.materials[1].double_sided); // not a boolean: the default
}

AETHER_TEST(Material_SceneShaderLightsDoubleSidedBackFaces) {
    const std::string source = player::SceneShaderSource(true, true);
    AETHER_CHECK(source.find("bool is_front : SV_IsFrontFace") != std::string::npos);
    AETHER_CHECK(source.find("((g_PC.g_AlphaOptions >> 10) & 1) != 0 && is_front == (NormalDeterminant() < 0.0)") != std::string::npos);
}
