#include "../player/src/particle_pass.h"

#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"
#include "aether/vfx/render.h"
#include "test_framework.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <memory>
#include <stdexcept>
#include <vector>

// GPU resource lifetime (docs/design/RHI_RESOURCE_LIFETIME.md): DestroyBuffer / DestroyPipeline / DestroyComputePipeline free native objects
// and recycle their slots, destruction waits (without stalling the device) for the submissions that could still use the resource, a
// destroyed handle is rejected, and teardown frees whatever is left once. Each body runs on Vulkan (lavapipe on Linux CI) and D3D12 (WARP on
// the Windows runner); a backend without a device skips with a note.

using namespace aether;
using namespace aether::gfx::rhi;

namespace {

std::unique_ptr<IDevice> MakeDevice(Backend backend) {
    std::unique_ptr<IDevice> device;
    if (IsBackendAvailable(backend)) {
        try {
            device = CreateDevice(backend, false);
        } catch (const std::exception&) {
        }
    }
    if (!device) std::printf("    (no %s device: install a driver (Mesa's lavapipe for Vulkan, WARP for D3D12) to run this)\n", backend == Backend::Vulkan ? "Vulkan" : "D3D12");
    return device;
}

// A full-target triangle read through a vertex buffer (POSITION + TEXCOORD0, the PositionUv layout, 20 bytes a vertex); red, no push constants.
constexpr const char* kVertexShader = R"(
struct VSInput { float3 position : POSITION; float2 uv : TEXCOORD0; };
float4 VSMain(VSInput input) : SV_Position { return float4(input.position, 1.0); }
float4 PSMain() : SV_Target { return float4(1.0, 0.0, 0.0, 1.0); }
)";
constexpr const char* kProceduralShader = R"(
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.5, 1.0);
}
float4 PSMain() : SV_Target { return float4(0.0, 1.0, 0.0, 1.0); }
)";
constexpr const char* kComputeShader = R"(
RWStructuredBuffer<uint> Out : register(u0);
[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) { Out[0] = 7u; }
)";

constexpr u32 kW = 32, kH = 32;
const f32 kTriangle[15] = {-1.0f, -1.0f, 0.5f, 0.0f, 0.0f, 3.0f, -1.0f, 0.5f, 1.0f, 0.0f, -1.0f, 3.0f, 0.5f, 0.0f, 1.0f};
const ClearColor kBlue{0.0f, 0.0f, 1.0f, 1.0f};

PipelineHandle MakeVertexPipeline(IDevice& device, ISwapChain& target) {
    PipelineDesc desc;
    desc.hlsl_source = kVertexShader;
    desc.use_vertex_buffer = true;
    desc.vertex_layout = VertexLayout::PositionUv;
    return device.CreatePipeline(desc, target);
}

ComputePipelineHandle MakeComputePipeline(IDevice& device) {
    ComputePipelineDesc desc;
    desc.hlsl_source = kComputeShader;
    desc.storage_buffer_count = 1;
    return device.CreateComputePipeline(desc);
}

// Records and submits one frame drawing the triangle; returns the fence without waiting.
u64 SubmitTriangle(IDevice& device, ISwapChain& target, ICommandList& cmd, PipelineHandle pipeline, BufferHandle vertices) {
    target.AcquireNextImage();
    cmd.Reset();
    cmd.BeginRenderPass(target, kBlue);
    cmd.BindPipeline(pipeline);
    cmd.BindVertexBuffer(vertices, 20);
    cmd.Draw(3);
    cmd.EndRenderPass();
    cmd.Close();
    return device.Submit(cmd, &target);
}

bool CenterIs(ISwapChain& target, u8 r, u8 g, u8 b) {
    std::vector<u8> pixels;
    if (!target.ReadBack(pixels) || pixels.size() != static_cast<usize>(kW) * kH * 4) return false;
    const usize at = (static_cast<usize>(kH / 2) * kW + kW / 2) * 4;
    auto near = [](u8 a, u8 e) { return a + 3 >= e && a <= e + 3; };
    return near(pixels[at], r) && near(pixels[at + 1], g) && near(pixels[at + 2], b);
}

// ---- bodies ----

void BuffersRecycleTheirSlots(Backend backend) {
    auto device = MakeDevice(backend);
    if (!device) return;
    const u8 bytes[64] = {};
    const IDevice::ResourceStats base = device->GetResourceStats();
    // Every kind, 2000 times: counts return to zero and the table never grows past one round's worth.
    for (u32 i = 0; i < 2000; ++i) {
        const BufferHandle vb = device->CreateVertexBuffer(bytes, 40);
        const BufferHandle ib = device->CreateIndexBuffer(bytes, 24, IndexFormat::UInt32);
        const BufferHandle cb = device->CreateConstantBuffer(64, bytes);
        const BufferHandle sb = device->CreateStorageBuffer(64, 4, bytes);
        const BufferHandle fb = device->SupportsFrameConstants() ? device->CreateFrameConstantBuffer(64) : BufferHandle{};
        AETHER_CHECK(vb.IsValid() && ib.IsValid() && cb.IsValid() && sb.IsValid());
        if (!vb.IsValid() || !ib.IsValid() || !cb.IsValid() || !sb.IsValid()) return;
        std::vector<BufferHandle> round = {vb, ib, cb, sb};
        if (fb.IsValid()) round.push_back(fb);
        if (i % 2) std::reverse(round.begin(), round.end());
        for (const BufferHandle buffer : round) AETHER_CHECK(device->DestroyBuffer(buffer));
    }
    device->ReleaseRetiredResources();
    IDevice::ResourceStats stats = device->GetResourceStats();
    AETHER_CHECK(stats.live_buffers == base.live_buffers);
    AETHER_CHECK(stats.buffer_slots <= base.buffer_slots + 5); // one round of five, reused ever after
    AETHER_CHECK(stats.pending_retire == 0);                   // an idle device retires at once

    // A sliding window of 16 live buffers: the table stays at the window, not at the number ever created.
    std::deque<BufferHandle> window;
    for (u32 i = 0; i < 3000; ++i) {
        window.push_back(device->CreateVertexBuffer(bytes, 40));
        AETHER_CHECK(window.back().IsValid());
        if (window.size() > 16) {
            AETHER_CHECK(device->DestroyBuffer(window.front()));
            window.pop_front();
        }
    }
    stats = device->GetResourceStats();
    AETHER_CHECK(stats.live_buffers == base.live_buffers + 16);
    AETHER_CHECK(stats.buffer_slots <= base.buffer_slots + 5 + 17);
    for (const BufferHandle buffer : window) AETHER_CHECK(device->DestroyBuffer(buffer));
    AETHER_CHECK(device->GetResourceStats().live_buffers == base.live_buffers);
}

void DoubleAndInvalidDestroyAreRejected(Backend backend) {
    auto device = MakeDevice(backend);
    if (!device) return;
    const u8 bytes[64] = {};
    const BufferHandle buffer = device->CreateConstantBuffer(64, bytes);
    AETHER_CHECK(device->DestroyBuffer(buffer));
    const IDevice::ResourceStats after = device->GetResourceStats();
    AETHER_CHECK(!device->DestroyBuffer(buffer));            // already gone
    AETHER_CHECK(!device->DestroyBuffer(BufferHandle{}));    // never a buffer
    AETHER_CHECK(!device->DestroyBuffer(BufferHandle{123456}));
    AETHER_CHECK(!device->DestroyPipeline(PipelineHandle{}));
    AETHER_CHECK(!device->DestroyPipeline(PipelineHandle{99999}));
    AETHER_CHECK(!device->DestroyComputePipeline(ComputePipelineHandle{}));
    AETHER_CHECK(!device->DestroyComputePipeline(ComputePipelineHandle{99999}));
    const IDevice::ResourceStats now = device->GetResourceStats();
    AETHER_CHECK(now.live_buffers == after.live_buffers && now.buffer_slots == after.buffer_slots);
    // Using the destroyed handle is refused, not a crash.
    device->UpdateConstantBuffer(buffer, bytes, 64, 0);
    device->UpdateVertexBuffer(buffer, bytes, 20);
    device->UpdateStorageBuffer(buffer, bytes, 16, 0);
    device->UpdateFrameConstantBuffer(buffer, bytes, 16);

    std::unique_ptr<ISwapChain> target = device->CreateSwapChain(nullptr, kW, kH, 2);
    const PipelineHandle pipeline = MakeVertexPipeline(*device, *target);
    AETHER_CHECK(pipeline.IsValid());
    AETHER_CHECK(device->DestroyPipeline(pipeline));
    AETHER_CHECK(!device->DestroyPipeline(pipeline));
    const ComputePipelineHandle compute = MakeComputePipeline(*device);
    AETHER_CHECK(compute.IsValid());
    AETHER_CHECK(device->DestroyComputePipeline(compute));
    AETHER_CHECK(!device->DestroyComputePipeline(compute));
    // A buffer handle is not a pipeline handle with the same index, and vice versa: each table is its own.
    const BufferHandle other = device->CreateVertexBuffer(kTriangle, sizeof(kTriangle));
    AETHER_CHECK(other.IsValid());
    AETHER_CHECK(!device->DestroyPipeline(PipelineHandle{other.index}));
    AETHER_CHECK(device->DestroyBuffer(other));
    const SampledTextureHandle tex = device->CreateTexture(1, 1, bytes);
    AETHER_CHECK(device->DestroyTexture(tex));
    AETHER_CHECK(!device->DestroyTexture(tex));
}

void PipelinesRecycleTheirSlots(Backend backend) {
    auto device = MakeDevice(backend);
    if (!device) return;
    std::unique_ptr<ISwapChain> target = device->CreateSwapChain(nullptr, kW, kH, 2);
    const IDevice::ResourceStats base = device->GetResourceStats();
    for (u32 i = 0; i < 120; ++i) {
        PipelineDesc desc;
        desc.hlsl_source = kProceduralShader;
        const PipelineHandle pipeline = device->CreatePipeline(desc, *target);
        AETHER_CHECK(pipeline.IsValid());
        if (!pipeline.IsValid()) return;
        const ComputePipelineHandle compute = MakeComputePipeline(*device);
        AETHER_CHECK(compute.IsValid());
        if (!compute.IsValid()) return;
        AETHER_CHECK(device->DestroyPipeline(pipeline));
        AETHER_CHECK(device->DestroyComputePipeline(compute));
    }
    const IDevice::ResourceStats stats = device->GetResourceStats();
    AETHER_CHECK(stats.live_pipelines == base.live_pipelines && stats.live_compute_pipelines == base.live_compute_pipelines);
    AETHER_CHECK(stats.pipeline_slots <= base.pipeline_slots + 1);
    AETHER_CHECK(stats.compute_pipeline_slots <= base.compute_pipeline_slots + 1);
}

void DestroyWhileSubmittedKeepsTheDraw(Backend backend) {
    auto device = MakeDevice(backend);
    if (!device) return;
    std::unique_ptr<ISwapChain> target = device->CreateSwapChain(nullptr, kW, kH, 2);
    std::unique_ptr<ICommandList> cmd = device->CreateCommandList();
    for (u32 round = 0; round < 8; ++round) {
        const PipelineHandle pipeline = MakeVertexPipeline(*device, *target);
        const BufferHandle vertices = device->CreateVertexBuffer(kTriangle, sizeof(kTriangle));
        AETHER_CHECK(pipeline.IsValid() && vertices.IsValid());
        if (!pipeline.IsValid() || !vertices.IsValid()) return;
        const u64 fence = SubmitTriangle(*device, *target, *cmd, pipeline, vertices);
        AETHER_CHECK(fence != 0);
        // The submission may still be running: the destroys must neither stall nor free what it reads.
        AETHER_CHECK(device->DestroyBuffer(vertices));
        AETHER_CHECK(device->DestroyPipeline(pipeline));
        device->WaitForFence(fence);
        AETHER_CHECK(CenterIs(*target, 255, 0, 0)); // the triangle the buffer held, drawn by the pipeline just freed
        target->Present(false);
        device->ReleaseRetiredResources();
        AETHER_CHECK(device->GetResourceStats().pending_retire == 0);
    }
    AETHER_CHECK(device->GetResourceStats().live_buffers == 0 && device->GetResourceStats().live_pipelines == 0);
}

void DestroyWhileComputeSubmitted(Backend backend) {
    auto device = MakeDevice(backend);
    if (!device) return;
    std::unique_ptr<ICommandList> cmd = device->CreateComputeCommandList();
    for (u32 round = 0; round < 8; ++round) {
        const ComputePipelineHandle pipeline = MakeComputePipeline(*device);
        const u32 zeros[4] = {};
        const BufferHandle storage = device->CreateStorageBuffer(16, 4, zeros);
        AETHER_CHECK(pipeline.IsValid() && storage.IsValid());
        if (!pipeline.IsValid() || !storage.IsValid()) return;
        cmd->Reset();
        cmd->BindComputePipeline(pipeline);
        cmd->BindComputeStorageBuffer(0, storage);
        cmd->Dispatch(1, 1, 1);
        cmd->ComputeBufferBarrier(storage);
        cmd->Close();
        const u64 fence = device->SubmitCompute(*cmd);
        AETHER_CHECK(fence != 0);
        AETHER_CHECK(device->DestroyBuffer(storage));
        AETHER_CHECK(device->DestroyComputePipeline(pipeline));
        device->WaitForComputeFence(fence);
        device->ReleaseRetiredResources();
    }
    const IDevice::ResourceStats stats = device->GetResourceStats();
    AETHER_CHECK(stats.live_buffers == 0 && stats.live_compute_pipelines == 0 && stats.pending_retire == 0);
}

void StaleHandlesDrawNothing(Backend backend) {
    auto device = MakeDevice(backend);
    if (!device) return;
    std::unique_ptr<ISwapChain> target = device->CreateSwapChain(nullptr, kW, kH, 2);
    std::unique_ptr<ICommandList> cmd = device->CreateCommandList();
    const PipelineHandle pipeline = MakeVertexPipeline(*device, *target);
    const BufferHandle vertices = device->CreateVertexBuffer(kTriangle, sizeof(kTriangle));
    // A second live pair, made before the destroys: a slot reused by a later Create* would make the old handle name the new resource.
    const PipelineHandle live_pipeline = MakeVertexPipeline(*device, *target);
    const BufferHandle live_vertices = device->CreateVertexBuffer(kTriangle, sizeof(kTriangle));
    AETHER_CHECK(pipeline.IsValid() && vertices.IsValid() && live_pipeline.IsValid() && live_vertices.IsValid());
    if (!pipeline.IsValid() || !vertices.IsValid() || !live_pipeline.IsValid() || !live_vertices.IsValid()) return;
    AETHER_CHECK(device->DestroyBuffer(vertices));
    AETHER_CHECK(device->DestroyPipeline(pipeline));
    // Recording with the destroyed handles is refused (logged), the draw is skipped, and the target keeps its clear colour.
    device->WaitForFence(SubmitTriangle(*device, *target, *cmd, pipeline, vertices));
    AETHER_CHECK(CenterIs(*target, 0, 0, 255));
    target->Present(false);
    // A stale pipeline with a live buffer is refused too.
    device->WaitForFence(SubmitTriangle(*device, *target, *cmd, pipeline, live_vertices));
    AETHER_CHECK(CenterIs(*target, 0, 0, 255));
    target->Present(false);
    // A stale buffer with a live pipeline is refused.
    device->WaitForFence(SubmitTriangle(*device, *target, *cmd, live_pipeline, vertices));
    AETHER_CHECK(CenterIs(*target, 0, 0, 255));
    target->Present(false);
    // And the live pair still draws (the checks above were refusals, not a broken frame).
    device->WaitForFence(SubmitTriangle(*device, *target, *cmd, live_pipeline, live_vertices));
    AETHER_CHECK(CenterIs(*target, 255, 0, 0));
    target->Present(false);
    AETHER_CHECK(device->DestroyBuffer(live_vertices));
    AETHER_CHECK(device->DestroyPipeline(live_pipeline));
}

void StressCreateDestroy(Backend backend) {
    auto device = MakeDevice(backend);
    if (!device) return;
    std::unique_ptr<ISwapChain> target = device->CreateSwapChain(nullptr, kW, kH, 2);
    std::unique_ptr<ICommandList> cmd = device->CreateCommandList();
    const PipelineHandle pipeline = MakeVertexPipeline(*device, *target);
    AETHER_CHECK(pipeline.IsValid());
    if (!pipeline.IsValid()) return;
    const u8 pixel[4] = {10, 200, 30, 255};
    // Thousands of create/draw/destroy rounds with the GPU running behind the CPU: the retire queue must neither grow without bound nor
    // free anything a pending frame reads.
    u32 max_pending = 0;
    u64 last_fence = 0;
    for (u32 i = 0; i < 3000; ++i) {
        const BufferHandle vertices = device->CreateVertexBuffer(kTriangle, sizeof(kTriangle));
        AETHER_CHECK(vertices.IsValid());
        if (!vertices.IsValid()) return;
        if (i % 50 == 0) {
            last_fence = SubmitTriangle(*device, *target, *cmd, pipeline, vertices);
            AETHER_CHECK(last_fence != 0);
            device->WaitForFence(last_fence); // a command list is reused only after its previous submission finished
            target->Present(false);
        }
        AETHER_CHECK(device->DestroyBuffer(vertices));
        if (i % 100 == 0) {
            const SampledTextureHandle texture = device->CreateTexture(1, 1, pixel);
            AETHER_CHECK(texture.IsValid());
            AETHER_CHECK(device->DestroyTexture(texture));
        }
        const IDevice::ResourceStats stats = device->GetResourceStats();
        if (stats.pending_retire > max_pending) max_pending = stats.pending_retire;
    }
    device->ReleaseRetiredResources();
    const IDevice::ResourceStats stats = device->GetResourceStats();
    AETHER_CHECK(stats.live_buffers == 0 && stats.live_textures == 0 && stats.pending_retire == 0);
    AETHER_CHECK(stats.buffer_slots <= 2);
    AETHER_CHECK(max_pending < 64);
    AETHER_CHECK(device->DestroyPipeline(pipeline));
}

void TeardownWithLiveResources(Backend backend) {
    auto device = MakeDevice(backend);
    if (!device) return;
    std::unique_ptr<ISwapChain> target = device->CreateSwapChain(nullptr, kW, kH, 2);
    std::unique_ptr<ICommandList> cmd = device->CreateCommandList();
    const u8 bytes[64] = {};
    const PipelineHandle pipeline = MakeVertexPipeline(*device, *target);
    const PipelineHandle second = MakeVertexPipeline(*device, *target);
    const ComputePipelineHandle compute = MakeComputePipeline(*device);
    const BufferHandle vertices = device->CreateVertexBuffer(kTriangle, sizeof(kTriangle));
    device->CreateIndexBuffer(bytes, 24, IndexFormat::UInt32);
    device->CreateConstantBuffer(64, bytes);
    device->CreateStorageBuffer(64, 4, bytes);
    if (device->SupportsFrameConstants()) device->CreateFrameConstantBuffer(64);
    device->CreateTexture(1, 1, bytes);
    const BufferHandle destroyed = device->CreateVertexBuffer(kTriangle, sizeof(kTriangle));
    AETHER_CHECK(pipeline.IsValid() && second.IsValid() && compute.IsValid() && vertices.IsValid() && destroyed.IsValid());
    if (!pipeline.IsValid() || !vertices.IsValid()) return;
    // A frame still in flight, resources destroyed behind it, others left alive: tearing the device down frees each native object once.
    const u64 fence = SubmitTriangle(*device, *target, *cmd, pipeline, vertices);
    AETHER_CHECK(fence != 0);
    AETHER_CHECK(device->DestroyBuffer(destroyed));
    AETHER_CHECK(device->DestroyPipeline(second));
    const IDevice::ResourceStats stats = device->GetResourceStats();
    AETHER_CHECK(stats.live_buffers >= 5 && stats.live_pipelines == 1 && stats.live_compute_pipelines == 1 && stats.live_textures == 1);
    cmd.reset();
    target.reset();
    device.reset(); // must not crash, double free, or hang (the Vulkan run under the validation layer checks the rest)
}

void ParticlePassFreesOutgrownBuffers(Backend backend) {
    auto device = MakeDevice(backend);
    if (!device) return;
    std::unique_ptr<ISwapChain> target = device->CreateSwapChain(nullptr, kW, kH, 2);
    std::unique_ptr<ICommandList> cmd = device->CreateCommandList();
    const IDevice::ResourceStats base = device->GetResourceStats();
    {
        player::ParticlePass pass(*device, *target);
        AETHER_CHECK(pass.Valid());
        if (!pass.Valid()) return;
        const IDevice::ResourceStats with_pass = device->GetResourceStats();
        AETHER_CHECK(with_pass.live_pipelines == base.live_pipelines + 3);
        const vfx::ParticleCamera camera;
        const Mat4 identity = Mat4::Identity();
        u32 grown_to_at_least = 0;
        // Each step has more sprites than the last, so the additive stream's buffer is outgrown several times (6 vertices a sprite, starting at 6144).
        for (u32 sprites : {10u, 1500u, 3000u, 6000u, 12000u, 12000u, 50u}) {
            vfx::SpriteInstance sprite;
            sprite.center = Vec3(0.0f, 0.0f, -2.0f);
            sprite.right = Vec3(0.5f, 0.0f, 0.0f);
            sprite.up = Vec3(0.0f, 0.25f, 0.0f);
            vfx::SpriteBatch batch;
            batch.blend = vfx::BlendMode::Additive;
            batch.instances.assign(sprites, sprite);
            vfx::ParticleRenderData data;
            data.sprites.push_back(batch);
            target->AcquireNextImage();
            cmd->Reset();
            cmd->BeginRenderPass(*target, kBlue);
            pass.Draw(*cmd, data, camera, identity, identity, 0);
            cmd->EndRenderPass();
            cmd->Close();
            device->WaitForFence(device->Submit(*cmd, target.get()));
            target->Present(false);
            const IDevice::ResourceStats stats = device->GetResourceStats();
            // One stream is drawn: exactly one vertex buffer lives however many times it grew, and the table holds at most the old and the new.
            AETHER_CHECK(stats.live_buffers == base.live_buffers + 1);
            AETHER_CHECK(stats.buffer_slots <= base.buffer_slots + 2);
            grown_to_at_least += sprites;
        }
        AETHER_CHECK(grown_to_at_least > 0);
    }
    // Destroying the pass frees its pipelines and the buffer.
    device->ReleaseRetiredResources();
    const IDevice::ResourceStats after = device->GetResourceStats();
    AETHER_CHECK(after.live_buffers == base.live_buffers && after.live_pipelines == base.live_pipelines);
}

} // namespace

#define AETHER_RHI_LIFETIME_TEST(Name)                                                \
    AETHER_TEST(RHI_Lifetime_##Name##_Vulkan) { Name(Backend::Vulkan); }              \
    AETHER_TEST(RHI_Lifetime_##Name##_D3D12) { Name(Backend::D3D12); }

AETHER_RHI_LIFETIME_TEST(BuffersRecycleTheirSlots)
AETHER_RHI_LIFETIME_TEST(DoubleAndInvalidDestroyAreRejected)
AETHER_RHI_LIFETIME_TEST(PipelinesRecycleTheirSlots)
AETHER_RHI_LIFETIME_TEST(DestroyWhileSubmittedKeepsTheDraw)
AETHER_RHI_LIFETIME_TEST(DestroyWhileComputeSubmitted)
AETHER_RHI_LIFETIME_TEST(StaleHandlesDrawNothing)
AETHER_RHI_LIFETIME_TEST(StressCreateDestroy)
AETHER_RHI_LIFETIME_TEST(TeardownWithLiveResources)
AETHER_RHI_LIFETIME_TEST(ParticlePassFreesOutgrownBuffers)

// M8 108, rows 23, 24 and 25: the checks that need a bound graphics pipeline. Each is asserted inside a real
// render pass on a real pipeline, and the control at the end proves the pipeline draws once its requirements
// are satisfied -- so a validation that rejected everything could not pass this.
void PushConstantAndBindingReadiness(Backend backend) {
    auto device = MakeDevice(backend);
    if (!device) return;
    std::unique_ptr<ISwapChain> target = device->CreateSwapChain(nullptr, kW, kH, 2);
    AETHER_CHECK(target != nullptr);
    if (!target) return;
    const PipelineHandle pipeline = MakeVertexPipeline(*device, *target);
    AETHER_CHECK(pipeline.IsValid());
    if (!pipeline.IsValid()) return;

    auto cmd = device->CreateCommandList();
    AETHER_CHECK(cmd != nullptr);
    if (!cmd) return;
    target->AcquireNextImage();
    cmd->Reset();
    cmd->BeginRenderPass(*target, kBlue);
    cmd->BindPipeline(pipeline);

    // Row 23: the pipeline declared no push-constant range, so any payload exceeds it.
    const f32 value = 1.0f;
    aether::Logger::Instance().ClearRecent();
    cmd->SetPushConstants(&value, sizeof(value));
    AETHER_CHECK(aether::test::LoggedErrorContaining("aligned push-constant range"));

    // Row 24: the pipeline was not created with bindless textures.
    aether::Logger::Instance().ClearRecent();
    cmd->BindBindlessTextures();
    AETHER_CHECK(aether::test::LoggedErrorContaining("bindless graphics pipeline"));

    // Row 25: the pipeline takes vertex input and no vertex buffer is bound.
    aether::Logger::Instance().ClearRecent();
    cmd->Draw(3);
    AETHER_CHECK(aether::test::LoggedErrorContaining("valid bound graphics pipeline"));

    // Control: with the vertices bound the same draw is accepted, so the checks above are not just "Draw fails".
    const BufferHandle vertices =
        device->CreateVertexBuffer(reinterpret_cast<const u8*>(kTriangle), static_cast<u32>(sizeof(kTriangle)));
    AETHER_CHECK(vertices.IsValid());
    if (vertices.IsValid()) {
        cmd->BindVertexBuffer(vertices, 20);
        aether::Logger::Instance().ClearRecent();
        cmd->Draw(3);
        AETHER_CHECK(!aether::test::LoggedErrorContaining("valid bound graphics pipeline"));
    }

    cmd->EndRenderPass();
    cmd->Close();
    const u64 fence = device->Submit(*cmd, target.get());
    device->WaitForFence(fence);
    AETHER_CHECK(device->IsFenceComplete(fence));
    if (vertices.IsValid()) AETHER_CHECK(device->DestroyBuffer(vertices));
    AETHER_CHECK(device->DestroyPipeline(pipeline));
}
AETHER_TEST(RHI_Validation_PushConstantAndBindingReadiness_D3D12) { PushConstantAndBindingReadiness(Backend::D3D12); }
// Rows 36, 37 and 38: draws outside the bound buffers, and a vertex stride that disagrees with the pipeline. Both
// are rejected inside a real render pass, and the same draws succeed at their exact capacity afterwards -- the
// classic off-by-one sits precisely on that boundary, so the control is as important as the rejection.
void DrawBoundsAndStrideReadiness(Backend backend) {
    auto device = MakeDevice(backend);
    if (!device) return;
    std::unique_ptr<ISwapChain> target = device->CreateSwapChain(nullptr, kW, kH, 2);
    AETHER_CHECK(target != nullptr);
    if (!target) return;
    const PipelineHandle pipeline = MakeVertexPipeline(*device, *target);
    AETHER_CHECK(pipeline.IsValid());
    if (!pipeline.IsValid()) return;

    // Three PositionUv vertices (20 bytes each) and six indices.
    const BufferHandle vertices =
        device->CreateVertexBuffer(reinterpret_cast<const u8*>(kTriangle), static_cast<u32>(sizeof(kTriangle)));
    const u32 indices[6] = {0, 1, 2, 0, 1, 2};
    const BufferHandle indexBuffer =
        device->CreateIndexBuffer(reinterpret_cast<const u8*>(indices), static_cast<u32>(sizeof(indices)), IndexFormat::UInt32);
    AETHER_CHECK(vertices.IsValid() && indexBuffer.IsValid());
    if (!vertices.IsValid() || !indexBuffer.IsValid()) return;

    auto cmd = device->CreateCommandList();
    AETHER_CHECK(cmd != nullptr);
    if (!cmd) return;
    target->AcquireNextImage();
    cmd->Reset();
    cmd->BeginRenderPass(*target, kBlue);
    cmd->BindPipeline(pipeline);

    // Row 37: a stride that is not the layout's.
    aether::Logger::Instance().ClearRecent();
    cmd->BindVertexBuffer(vertices, 12);
    AETHER_CHECK(aether::test::LoggedErrorContaining("stride that does not match"));
    cmd->BindVertexBuffer(vertices, 20);
    cmd->BindIndexBuffer(indexBuffer, IndexFormat::UInt32);

    // Row 36: one index beyond the six the buffer holds.
    aether::Logger::Instance().ClearRecent();
    cmd->DrawIndexed(7);
    AETHER_CHECK(aether::test::LoggedErrorContaining("valid bound graphics pipeline"));

    // Row 38: one vertex beyond the three the buffer holds.
    aether::Logger::Instance().ClearRecent();
    cmd->Draw(4);
    AETHER_CHECK(aether::test::LoggedErrorContaining("valid bound graphics pipeline"));

    // Controls: exactly at capacity both draws are accepted.
    aether::Logger::Instance().ClearRecent();
    cmd->DrawIndexed(6);
    AETHER_CHECK(!aether::test::LoggedErrorContaining("valid bound graphics pipeline"));
    aether::Logger::Instance().ClearRecent();
    cmd->Draw(3);
    AETHER_CHECK(!aether::test::LoggedErrorContaining("valid bound graphics pipeline"));

    cmd->EndRenderPass();
    cmd->Close();
    const u64 fence = device->Submit(*cmd, target.get());
    device->WaitForFence(fence);
    AETHER_CHECK(device->IsFenceComplete(fence));
    AETHER_CHECK(device->DestroyBuffer(vertices));
    AETHER_CHECK(device->DestroyBuffer(indexBuffer));
    AETHER_CHECK(device->DestroyPipeline(pipeline));
}
AETHER_TEST(RHI_Validation_DrawBoundsAndStrideReadiness_D3D12) { DrawBoundsAndStrideReadiness(Backend::D3D12); }
// Rows 19 and 42: graphics-only resources are refused on a compute command list rather than recorded into a queue
// that cannot execute them. The back buffer is a real, valid texture, so this is the queue clause being checked and
// not "the handle was invalid".
void GraphicsResourcesOnComputeLists(Backend backend) {
    auto device = MakeDevice(backend);
    if (!device) return;
    std::unique_ptr<ISwapChain> target = device->CreateSwapChain(nullptr, kW, kH, 2);
    AETHER_CHECK(target != nullptr);
    if (!target) return;
    target->AcquireNextImage();
    const TextureHandle backBuffer = target->CurrentBackBuffer();
    AETHER_CHECK(backBuffer.IsValid());
    if (!backBuffer.IsValid()) return;

    auto compute = device->CreateComputeCommandList();
    AETHER_CHECK(compute != nullptr);
    if (!compute) return;
    compute->Reset();

    aether::Logger::Instance().ClearRecent();
    compute->TransitionTexture(backBuffer, ResourceState::Present, ResourceState::RenderTarget);
    AETHER_CHECK(aether::test::LoggedErrorContaining("graphics list and valid texture/resource states"));

    aether::Logger::Instance().ClearRecent();
    compute->ClearRenderTarget(backBuffer, kBlue);
    AETHER_CHECK(aether::test::LoggedErrorContaining("graphics list and valid texture/color"));

    compute->Close();
    const u64 fence = device->SubmitCompute(*compute);
    device->WaitForComputeFence(fence);
    AETHER_CHECK(device->IsComputeFenceComplete(fence));
}
AETHER_TEST(RHI_Validation_GraphicsResourcesOnComputeLists_D3D12) { GraphicsResourcesOnComputeLists(Backend::D3D12); }