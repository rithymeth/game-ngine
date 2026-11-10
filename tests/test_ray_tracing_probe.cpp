#include "aether/assets/environment.h"
#include "aether/core/log.h"
#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"
#include "aether/gfx/shader_compiler.h"
#include "test_framework.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace aether;
namespace rhi = aether::gfx::rhi;

namespace {
// Vulkan enables its ray tracing features only when AETHER_VK_RAY_TRACING=1 is set as the device is made (see vulkan_backend.cpp); the variable
// is cleared again at once so that no other test's device gets them.
std::unique_ptr<rhi::IDevice> MakeRayTracingDevice(rhi::Backend backend) {
    const bool vulkan = backend == rhi::Backend::Vulkan;
#if defined(_WIN32)
    if (vulkan) _putenv_s("AETHER_VK_RAY_TRACING", "1");
#else
    if (vulkan) setenv("AETHER_VK_RAY_TRACING", "1", 1);
#endif
    std::unique_ptr<rhi::IDevice> device;
    try {
        device = rhi::CreateDevice(backend, false);
    } catch (const std::exception&) {
    }
#if defined(_WIN32)
    if (vulkan) _putenv_s("AETHER_VK_RAY_TRACING", "");
#else
    if (vulkan) unsetenv("AETHER_VK_RAY_TRACING");
#endif
    return device;
}
} // namespace

// IDevice::SupportsRayTracing is a probe: it must answer without throwing, and say the same thing every time. What the answer is depends on
// the GPU, so the test reports it rather than demanding it.

static void RayTracingProbe_AnswersConsistently(rhi::Backend backend, const char* name) {
    if (!rhi::IsBackendAvailable(backend)) return;
    std::unique_ptr<rhi::IDevice> device = MakeRayTracingDevice(backend);
    if (!device) return;
    const bool first = device->SupportsRayTracing();
    AETHER_CHECK(device->SupportsRayTracing() == first);
    std::fprintf(stderr, "[ray tracing] %s: %s\n", name, first ? "supported" : "not supported");
}

// A Vulkan device may say it supports ray tracing only where the shaders for it can be compiled. lavapipe has ray queries, and
// off Windows the HLSL front end does not: such a device said yes and could build no pipeline that used an acceleration structure.
AETHER_TEST(RayTracingProbe_VulkanSaysYesOnlyWhereRayQueryShadersCompile) {
    if (!rhi::IsBackendAvailable(rhi::Backend::Vulkan)) return;
    const bool can_compile = gfx::CanCompileRayQueriesToSPIRV();
    AETHER_CHECK(gfx::CanCompileRayQueriesToSPIRV() == can_compile);
    std::unique_ptr<rhi::IDevice> device = MakeRayTracingDevice(rhi::Backend::Vulkan);
    if (!device) return;
    std::fprintf(stderr, "[ray tracing] Vulkan: ray-query shaders %s, device says %s\n", can_compile ? "compile" : "do not compile",
                 device->SupportsRayTracing() ? "supported" : "not supported");
    AETHER_CHECK(can_compile || !device->SupportsRayTracing());
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

// Acceleration structures end to end: a quad covers the left half of the unit square in the plane z = 1; the pixel shader fires one ray per
// pixel along +z and paints the pixel red when the ray query hits. A bottom level, a top level placing it, a pipeline that sees the top level,
// the same picture on both backends; then the instance moved by half the square to show the transform is honoured.
static void AccelerationStructure_RaysHitTheQuad(rhi::Backend backend) {
    if (!rhi::IsBackendAvailable(backend)) return;
    std::unique_ptr<rhi::IDevice> device = MakeRayTracingDevice(backend);
    if (!device || !device->SupportsRayTracing()) return;
    constexpr char kRayShader[] = R"(
RaytracingAccelerationStructure g_Scene : register(t0, space4);
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float4 PSMain(float4 position : SV_Position) : SV_Target {
    RayDesc ray;
    ray.Origin = float3(position.x / 8.0, position.y / 8.0, 0.0);
    ray.Direction = float3(0.0, 0.0, 1.0);
    ray.TMin = 0.001;
    ray.TMax = 10.0;
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(g_Scene, RAY_FLAG_NONE, 0xFF, ray);
    query.Proceed();
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? float4(1, 0, 0, 1) : float4(0, 0, 0, 1);
}
)";
    std::unique_ptr<rhi::ISwapChain> swap = device->CreateSwapChain(nullptr, 16, 16, 2);
    // Four vertices of position + uv (20 bytes each) and two triangles.
    const f32 vertices[] = {0.0f, 0.0f, 1.0f, 0, 0, 0.5f, 0.0f, 1.0f, 0, 0, 0.5f, 1.0f, 1.0f, 0, 0, 0.0f, 1.0f, 1.0f, 0, 0};
    const u32 indices[] = {0, 1, 2, 0, 2, 3};
    const rhi::BufferHandle vertex_buffer = device->CreateVertexBuffer(vertices, sizeof(vertices));
    const rhi::BufferHandle index_buffer = device->CreateIndexBuffer(indices, sizeof(indices), rhi::IndexFormat::UInt32);
    rhi::AccelerationStructureHandle blas;
    try {
        blas = device->CreateBottomLevelAS(vertex_buffer, 4, 20, index_buffer, 6);
    } catch (const std::exception&) {
    }
    AETHER_CHECK(blas.IsValid());
    if (!blas.IsValid()) return;

    rhi::PipelineDesc desc;
    desc.hlsl_source = kRayShader;
    desc.render_texture_target = true;
    desc.shader_model_6 = true;
    desc.acceleration_structure = true;
    rhi::PipelineHandle pipeline;
    try {
        pipeline = device->CreatePipeline(desc, *swap);
    } catch (const std::exception&) {
    }
    AETHER_CHECK(pipeline.IsValid());
    constexpr u32 kSize = 8;
    const rhi::SampledTextureHandle texture = device->CreateRenderTexture(kSize, kSize);
    const u32 pitch = rhi::ReadbackRowPitch(kSize, 8);
    const rhi::BufferHandle readback = device->CreateReadbackBuffer(static_cast<u64>(pitch) * kSize);
    if (!pipeline.IsValid() || !texture.IsValid() || !readback.IsValid()) return;

    // Draws with `tlas` bound and returns the red channel of every pixel.
    auto trace = [&](rhi::AccelerationStructureHandle tlas) {
        std::unique_ptr<rhi::ICommandList> cmd = device->CreateCommandList();
        cmd->Reset();
        AETHER_CHECK(cmd->BeginTexturePass(texture, nullptr));
        cmd->BindPipeline(pipeline);
        cmd->BindAccelerationStructure(tlas);
        cmd->Draw(3);
        cmd->EndRenderPass();
        AETHER_CHECK(cmd->CopyRenderTextureToReadback(texture, readback));
        cmd->Close();
        device->WaitForFence(device->Submit(*cmd, nullptr));
        std::vector<u16> rows(static_cast<usize>(pitch) / 2 * kSize);
        AETHER_CHECK(device->ReadReadbackBuffer(readback, rows.data(), rows.size() * 2));
        std::vector<f32> red(kSize * kSize);
        for (u32 y = 0; y < kSize; ++y) {
            for (u32 x = 0; x < kSize; ++x) red[y * kSize + x] = assets::HalfToFloat(rows[static_cast<usize>(y) * (pitch / 2) + x * 4]);
        }
        return red;
    };
    auto expect_columns = [&](const std::vector<f32>& red, u32 first_hit, u32 last_hit) {
        u32 wrong = 0;
        for (u32 y = 0; y < kSize; ++y) {
            for (u32 x = 0; x < kSize; ++x) {
                const bool hit = x >= first_hit && x <= last_hit;
                if (red[y * kSize + x] != (hit ? 1.0f : 0.0f)) ++wrong;
            }
        }
        return wrong;
    };

    rhi::AccelerationInstance instance;
    instance.blas = blas;
    rhi::AccelerationStructureHandle left = device->CreateTopLevelAS(std::span<const rhi::AccelerationInstance>(&instance, 1));
    AETHER_CHECK(left.IsValid());
    instance.transform[3] = 0.5f; // moved half the square to the right
    rhi::AccelerationStructureHandle right = device->CreateTopLevelAS(std::span<const rhi::AccelerationInstance>(&instance, 1));
    AETHER_CHECK(right.IsValid());
    if (left.IsValid()) AETHER_CHECK(expect_columns(trace(left), 0, 3) == 0);
    if (right.IsValid()) AETHER_CHECK(expect_columns(trace(right), 4, 7) == 0);

    // Refusals: a top level is not an instance, a dead handle, buffers that are not those, a bottom level as the bound structure.
    rhi::AccelerationInstance bad;
    bad.blas = left;
    bool threw = false;
    try {
        device->CreateTopLevelAS(std::span<const rhi::AccelerationInstance>(&bad, 1));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    AETHER_CHECK(threw);
    threw = false;
    try {
        device->CreateBottomLevelAS(index_buffer, 4, 20, vertex_buffer, 6); // swapped
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    AETHER_CHECK(threw);
    AETHER_CHECK(device->DestroyAccelerationStructure(left));
    AETHER_CHECK(!device->DestroyAccelerationStructure(left));
    AETHER_CHECK(device->DestroyAccelerationStructure(right));
    AETHER_CHECK(device->DestroyAccelerationStructure(blas));
    device->DestroyPipeline(pipeline);
    device->DestroyTexture(texture);
    device->DestroyBuffer(readback);
    device->DestroyBuffer(vertex_buffer);
    device->DestroyBuffer(index_buffer);
}

AETHER_TEST(AccelerationStructure_RaysHitTheQuad_D3D12) { AccelerationStructure_RaysHitTheQuad(rhi::Backend::D3D12); }
AETHER_TEST(AccelerationStructure_RaysHitTheQuad_Vulkan) { AccelerationStructure_RaysHitTheQuad(rhi::Backend::Vulkan); }
