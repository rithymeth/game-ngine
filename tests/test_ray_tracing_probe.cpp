#include "aether/assets/environment.h"
#include "aether/core/log.h"
#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"
#include "aether/gfx/shader_compiler.h"
#include "test_framework.h"

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace aether;
namespace rhi = aether::gfx::rhi;

// IDevice::SupportsRayTracing is a probe: it must answer without throwing, and say the same thing every time. What the answer is depends on
// the GPU, so the test reports it rather than demanding it.

static void RayTracingProbe_AnswersConsistently(rhi::Backend backend, const char* name) {
    if (!rhi::IsBackendAvailable(backend)) return;
    std::unique_ptr<rhi::IDevice> device;
    try {
        device = rhi::CreateDevice(backend, false);
    } catch (const std::exception&) {
    }
    if (!device) return;
    const bool first = device->SupportsRayTracing();
    AETHER_CHECK(device->SupportsRayTracing() == first);
    std::fprintf(stderr, "[ray tracing] %s: %s\n", name, first ? "supported" : "not supported");
}

AETHER_TEST(RayTracingProbe_AnswersConsistently_D3D12) { RayTracingProbe_AnswersConsistently(rhi::Backend::D3D12, "D3D12"); }
AETHER_TEST(RayTracingProbe_AnswersConsistently_Vulkan) { RayTracingProbe_AnswersConsistently(rhi::Backend::Vulkan, "Vulkan"); }

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

// PipelineDesc::shader_model_6 sends the shaders through DXC as DXIL 6.5 on D3D12: the same HLSL draws the same pixels as through FXC.
static void ShaderModel6_DrawsWhatFxcDraws(rhi::Backend backend) {
    if (!rhi::IsBackendAvailable(backend)) return;
    std::unique_ptr<rhi::IDevice> device;
    try {
        device = rhi::CreateDevice(backend, false);
    } catch (const std::exception&) {
    }
    if (!device) return;
    std::unique_ptr<rhi::ISwapChain> swap = device->CreateSwapChain(nullptr, 16, 16, 2);
    constexpr u32 kWidth = 9, kHeight = 4;
    const rhi::SampledTextureHandle texture = device->CreateRenderTexture(kWidth, kHeight);
    const u32 pitch = rhi::ReadbackRowPitch(kWidth, 8);
    const rhi::BufferHandle readback = device->CreateReadbackBuffer(static_cast<u64>(pitch) * kHeight);
    if (!texture.IsValid() || !readback.IsValid()) return;
    std::vector<u16> pixels[2];
    for (const bool sm6 : {false, true}) {
        rhi::PipelineDesc desc;
        desc.hlsl_source = kGradientShader;
        desc.render_texture_target = true;
        desc.shader_model_6 = sm6;
        rhi::PipelineHandle pipeline;
        try {
            pipeline = device->CreatePipeline(desc, *swap);
        } catch (const std::exception&) {
        }
        AETHER_CHECK(pipeline.IsValid());
        if (!pipeline.IsValid()) return;
        std::unique_ptr<rhi::ICommandList> cmd = device->CreateCommandList();
        cmd->Reset();
        AETHER_CHECK(cmd->BeginTexturePass(texture, nullptr));
        cmd->BindPipeline(pipeline);
        cmd->Draw(3);
        cmd->EndRenderPass();
        AETHER_CHECK(cmd->CopyRenderTextureToReadback(texture, readback));
        cmd->Close();
        device->WaitForFence(device->Submit(*cmd, nullptr));
        std::vector<u16>& out = pixels[sm6 ? 1 : 0];
        out.resize(static_cast<usize>(pitch) / 2 * kHeight);
        AETHER_CHECK(device->ReadReadbackBuffer(readback, out.data(), out.size() * 2));
        device->DestroyPipeline(pipeline);
    }
    AETHER_CHECK(pixels[0] == pixels[1]);
    AETHER_CHECK(assets::HalfToFloat(pixels[1][(pitch / 2) * 2 + 4 * 4 + 0]) == 4.0f); // x of the pixel at (4, 2)
    device->DestroyTexture(texture);
    device->DestroyBuffer(readback);
}

AETHER_TEST(ShaderModel6_DrawsWhatFxcDraws_D3D12) { ShaderModel6_DrawsWhatFxcDraws(rhi::Backend::D3D12); }
AETHER_TEST(ShaderModel6_DrawsWhatFxcDraws_Vulkan) { ShaderModel6_DrawsWhatFxcDraws(rhi::Backend::Vulkan); }

#if defined(AETHER_PLATFORM_WINDOWS)
// What FXC cannot compile at all: an inline ray query (shader model 6.5). The DXIL comes back signed and non-empty; a shader that is not
// valid HLSL still throws.
AETHER_TEST(ShaderModel6_ARayQueryCompilesToDXIL) {
    const char* source = R"(
RaytracingAccelerationStructure g_Scene : register(t0, space5);
float4 PSMain(float4 position : SV_Position) : SV_Target {
    RayDesc ray;
    ray.Origin = float3(position.xy, 0.0);
    ray.Direction = float3(0.0, 0.0, 1.0);
    ray.TMin = 0.001;
    ray.TMax = 100.0;
    RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(g_Scene, 0, 0xFF, ray);
    query.Proceed();
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? float4(0, 0, 0, 1) : float4(1, 1, 1, 1);
}
)";
    gfx::ShaderBytecode dxil;
    try {
        dxil = gfx::CompileHLSLToDXIL(source, "PSMain", "ps_6_5", "ray_query_test");
    } catch (const std::exception&) {
    }
    AETHER_CHECK(dxil.Size() > 4);
    AETHER_CHECK(dxil.Size() > 4 && dxil.data[0] == 'D' && dxil.data[1] == 'X' && dxil.data[2] == 'B' && dxil.data[3] == 'C'); // a DXIL container
    bool threw = false;
    try {
        gfx::CompileHLSLToDXIL("float4 PSMain() : SV_Target { return nope; }", "PSMain", "ps_6_5", "broken");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    AETHER_CHECK(threw);
}
#endif
