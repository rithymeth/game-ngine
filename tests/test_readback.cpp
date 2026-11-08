#include "aether/assets/environment.h"
#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"
#include "test_framework.h"

#include <memory>
#include <stdexcept>
#include <vector>

using namespace aether;
namespace rhi = aether::gfx::rhi;

// Asynchronous readback of render textures (IDevice::CreateReadbackBuffer, ICommandList::CopyRenderTextureToReadback): the copy is recorded in
// the frame's command list and the CPU reads it after the fence, so nothing stalls. Used by the shadow feedback pass.

namespace {

constexpr char kGradientShader[] = R"(
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float4 PSMain(float4 position : SV_Position) : SV_Target {
    return float4(floor(position.x), floor(position.y), 7.0, 0.5);
}
)";

} // namespace

static void Readback_ARenderTextureComesBackRowByRow(rhi::Backend backend) {
    if (!rhi::IsBackendAvailable(backend)) return;
    std::unique_ptr<rhi::IDevice> device;
    try {
        device = rhi::CreateDevice(backend, false);
    } catch (const std::exception&) {
    }
    if (!device) return;
    AETHER_CHECK(device->SupportsReadback());
    std::unique_ptr<rhi::ISwapChain> swap = device->CreateSwapChain(nullptr, 16, 16, 2);
    constexpr u32 kWidth = 37, kHeight = 5; // a width whose rows need padding
    const rhi::SampledTextureHandle texture = device->CreateRenderTexture(kWidth, kHeight);
    AETHER_CHECK(texture.IsValid());
    if (!texture.IsValid()) return;
    rhi::PipelineDesc desc;
    desc.hlsl_source = kGradientShader;
    desc.render_texture_target = true;
    const rhi::PipelineHandle pipeline = device->CreatePipeline(desc, *swap);
    AETHER_CHECK(pipeline.IsValid());
    const u32 pitch = rhi::ReadbackRowPitch(kWidth, 8);
    AETHER_CHECK(pitch % 256 == 0 && pitch >= kWidth * 8);
    const rhi::BufferHandle readback = device->CreateReadbackBuffer(static_cast<u64>(pitch) * kHeight);
    AETHER_CHECK(readback.IsValid());
    if (!pipeline.IsValid() || !readback.IsValid()) return;

    std::unique_ptr<rhi::ICommandList> cmd = device->CreateCommandList();
    cmd->Reset();
    AETHER_CHECK(cmd->BeginTexturePass(texture, nullptr));
    cmd->BindPipeline(pipeline);
    cmd->Draw(3);
    cmd->EndRenderPass();
    AETHER_CHECK(cmd->CopyRenderTextureToReadback(texture, readback));
    cmd->Close();
    device->WaitForFence(device->Submit(*cmd, nullptr));

    std::vector<u16> rows(static_cast<usize>(pitch) / 2 * kHeight);
    AETHER_CHECK(device->ReadReadbackBuffer(readback, rows.data(), rows.size() * 2));
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            const u16* texel = &rows[static_cast<usize>(y) * (pitch / 2) + static_cast<usize>(x) * 4];
            AETHER_CHECK(assets::HalfToFloat(texel[0]) == static_cast<f32>(x) && assets::HalfToFloat(texel[1]) == static_cast<f32>(y) &&
                         assets::HalfToFloat(texel[2]) == 7.0f && assets::HalfToFloat(texel[3]) == 0.5f);
        }
    }

    // The texture is still a render target afterwards: a second pass and copy work, and give the same answer.
    cmd->Reset();
    AETHER_CHECK(cmd->BeginTexturePass(texture, nullptr));
    cmd->BindPipeline(pipeline);
    cmd->Draw(3);
    cmd->EndRenderPass();
    AETHER_CHECK(cmd->CopyRenderTextureToReadback(texture, readback));
    cmd->Close();
    device->WaitForFence(device->Submit(*cmd, nullptr));
    std::vector<u16> again(rows.size());
    AETHER_CHECK(device->ReadReadbackBuffer(readback, again.data(), again.size() * 2) && again == rows);

    // Refusals: a buffer that is not a readback buffer, one too small, a pass in progress; and reading a non-readback buffer or past the end.
    const f32 vertex_data[12] = {};
    const rhi::BufferHandle vertex = device->CreateVertexBuffer(vertex_data, sizeof(vertex_data));
    const rhi::BufferHandle small = device->CreateReadbackBuffer(pitch); // one row
    cmd->Reset();
    AETHER_CHECK(!cmd->CopyRenderTextureToReadback(texture, vertex));
    AETHER_CHECK(!cmd->CopyRenderTextureToReadback(texture, small));
    AETHER_CHECK(!cmd->CopyRenderTextureToReadback({}, readback));
    AETHER_CHECK(cmd->BeginTexturePass(texture, nullptr));
    AETHER_CHECK(!cmd->CopyRenderTextureToReadback(texture, readback));
    cmd->EndRenderPass();
    cmd->Close();
    u8 byte = 0;
    AETHER_CHECK(!device->ReadReadbackBuffer(vertex, &byte, 1));
    AETHER_CHECK(!device->ReadReadbackBuffer(readback, &byte, 2 * static_cast<u64>(pitch) * kHeight));
    AETHER_CHECK(!device->ReadReadbackBuffer(readback, &byte, 1, static_cast<u64>(pitch) * kHeight));
    AETHER_CHECK(!device->ReadReadbackBuffer({}, &byte, 1));
    bool threw = false;
    try {
        device->CreateReadbackBuffer(0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    AETHER_CHECK(threw);

    device->WaitForFence(device->Submit(*cmd, nullptr));
    device->DestroyBuffer(vertex);
    device->DestroyBuffer(small);
    device->DestroyBuffer(readback);
    device->DestroyPipeline(pipeline);
    device->DestroyTexture(texture);
}

AETHER_TEST(Readback_ARenderTextureComesBackRowByRow_D3D12) { Readback_ARenderTextureComesBackRowByRow(rhi::Backend::D3D12); }
AETHER_TEST(Readback_ARenderTextureComesBackRowByRow_Vulkan) { Readback_ARenderTextureComesBackRowByRow(rhi::Backend::Vulkan); }
