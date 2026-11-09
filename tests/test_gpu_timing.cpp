#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"
#include "test_framework.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>

using namespace aether;
namespace rhi = aether::gfx::rhi;

// GPU timers (IDevice::CreateGpuTimer, ICommandList::WriteGpuTimestamp / ResolveGpuTimer): the GPU's own time between points of a recording.
// A heavy pass and then a trivial one are timed; the heavy one must read longer, the whole must fit in the wall time the CPU spent waiting for
// it, and the handles must behave (read once, refuse after a destroy, refuse bad counts).

namespace {

constexpr char kHeavyShader[] = R"(
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float4 PSMain(float4 position : SV_Position) : SV_Target {
    float x = position.x * 0.001;
    [loop] for (int i = 0; i < 3000; ++i) x = sin(x) * 1.0001 + 0.0001 * i;
    return float4(x, 0, 0, 1);
}
)";

constexpr char kLightShader[] = R"(
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float4 PSMain(float4 position : SV_Position) : SV_Target { return float4(1, 0, 0, 1); }
)";

} // namespace

static void GpuTiming_ReadsTheGpusOwnTime(rhi::Backend backend, const char* name) {
    if (!rhi::IsBackendAvailable(backend)) return;
    std::unique_ptr<rhi::IDevice> device;
    try {
        device = rhi::CreateDevice(backend, false);
    } catch (const std::exception&) {
    }
    if (!device) return;
    if (!device->SupportsGpuTiming()) {
        AETHER_CHECK(!device->CreateGpuTimer(3).IsValid()); // a device without timing says so with an invalid handle
        std::printf("    %s: no GPU timing here, skipped\n", name);
        return;
    }
    // Bad counts, and a handle that never was.
    bool threw = false;
    try {
        device->CreateGpuTimer(0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    AETHER_CHECK(threw);
    threw = false;
    try {
        device->CreateGpuTimer(65);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    AETHER_CHECK(threw);
    f64 scratch[1] = {};
    AETHER_CHECK(!device->ReadGpuTimer({}, scratch));
    AETHER_CHECK(!device->DestroyGpuTimer({}));

    std::unique_ptr<rhi::ISwapChain> swap = device->CreateSwapChain(nullptr, 16, 16, 2);
    const rhi::SampledTextureHandle target = device->CreateRenderTexture(1024, 1024);
    rhi::PipelineDesc heavy_desc;
    heavy_desc.hlsl_source = kHeavyShader;
    heavy_desc.render_texture_target = true;
    rhi::PipelineDesc light_desc = heavy_desc;
    light_desc.hlsl_source = kLightShader;
    const rhi::PipelineHandle heavy = device->CreatePipeline(heavy_desc, *swap);
    const rhi::PipelineHandle light = device->CreatePipeline(light_desc, *swap);
    const rhi::GpuTimerHandle timer = device->CreateGpuTimer(3);
    AETHER_CHECK(target.IsValid() && heavy.IsValid() && light.IsValid() && timer.IsValid());
    if (!target.IsValid() || !heavy.IsValid() || !light.IsValid() || !timer.IsValid()) return;

    f64 times[3] = {};
    AETHER_CHECK(!device->ReadGpuTimer(timer, times)); // nothing resolved and submitted yet

    std::unique_ptr<rhi::ICommandList> cmd = device->CreateCommandList();
    f64 best_heavy = 0.0, best_light = 1e9, wall_ms = 0.0;
    for (int round = 0; round < 3; ++round) { // the same timer, used again each round
        const auto start = std::chrono::steady_clock::now();
        cmd->Reset();
        AETHER_CHECK(cmd->WriteGpuTimestamp(timer, 0));
        AETHER_CHECK(cmd->BeginTexturePass(target, nullptr));
        cmd->BindPipeline(heavy);
        cmd->Draw(3);
        cmd->EndRenderPass();
        AETHER_CHECK(cmd->WriteGpuTimestamp(timer, 1));
        AETHER_CHECK(cmd->BeginTexturePass(target, nullptr));
        cmd->BindPipeline(light);
        cmd->Draw(3);
        cmd->EndRenderPass();
        AETHER_CHECK(cmd->WriteGpuTimestamp(timer, 2));
        AETHER_CHECK(!cmd->WriteGpuTimestamp(timer, 3)); // out of range
        AETHER_CHECK(!cmd->WriteGpuTimestamp({}, 0));
        AETHER_CHECK(cmd->ResolveGpuTimer(timer));
        cmd->Close();
        device->WaitForFence(device->Submit(*cmd, nullptr));
        wall_ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
        AETHER_CHECK(device->ReadGpuTimer(timer, times));
        AETHER_CHECK(!device->ReadGpuTimer(timer, times)); // read once per use
        AETHER_CHECK(times[0] == 0.0 && times[1] >= times[0] && times[2] >= times[1]);
        best_heavy = std::max(best_heavy, times[1] - times[0]);
        best_light = std::min(best_light, times[2] - times[1]);
        AETHER_CHECK(times[2] <= wall_ms + 0.5); // the GPU cannot have worked longer than the CPU waited
    }
    std::printf("    %s: the heavy pass %.3f ms, the trivial one %.3f ms, the whole recording took %.3f ms of wall time\n", name, best_heavy,
                best_light, wall_ms);
    AETHER_CHECK(best_heavy > 0.05);
    AETHER_CHECK(best_heavy > best_light * 2.0);

    // Fewer slots than the timer holds can be read; more than it holds cannot.
    cmd->Reset();
    AETHER_CHECK(cmd->WriteGpuTimestamp(timer, 0));
    AETHER_CHECK(cmd->WriteGpuTimestamp(timer, 1));
    AETHER_CHECK(cmd->WriteGpuTimestamp(timer, 2));
    AETHER_CHECK(cmd->ResolveGpuTimer(timer));
    cmd->Close();
    device->WaitForFence(device->Submit(*cmd, nullptr));
    f64 four[4] = {};
    AETHER_CHECK(!device->ReadGpuTimer(timer, four));
    f64 two[2] = {};
    AETHER_CHECK(device->ReadGpuTimer(timer, two) && two[0] == 0.0);

    AETHER_CHECK(device->DestroyGpuTimer(timer));
    AETHER_CHECK(!device->DestroyGpuTimer(timer));
    AETHER_CHECK(!device->ReadGpuTimer(timer, times));
    device->DestroyPipeline(heavy);
    device->DestroyPipeline(light);
    device->DestroyTexture(target);
}

AETHER_TEST(GpuTiming_ReadsTheGpusOwnTime_D3D12) { GpuTiming_ReadsTheGpusOwnTime(rhi::Backend::D3D12, "d3d12"); }
AETHER_TEST(GpuTiming_ReadsTheGpusOwnTime_Vulkan) { GpuTiming_ReadsTheGpusOwnTime(rhi::Backend::Vulkan, "vulkan"); }
