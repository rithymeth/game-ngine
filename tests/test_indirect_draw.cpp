#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"
#include "test_framework.h"

#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace aether;
namespace rhi = aether::gfx::rhi;

// ICommandList::DrawIndexedIndirect (virtual geometry stage 3): the GPU reads the draw's arguments from a storage buffer. The same triangle is drawn
// with 3 indices in the arguments (it appears) and with 0 (nothing appears), so the count really comes from the buffer, not the CPU.

namespace {

constexpr char kShader[] = R"(
struct VSInput { float3 position : POSITION; float2 uv : TEXCOORD0; };
float4 VSMain(VSInput input) : SV_Position { return float4(input.position, 1.0); }
float4 PSMain() : SV_Target { return float4(1.0, 0.0, 0.0, 1.0); }
)";
constexpr u32 kSize = 32;
const f32 kTriangle[15] = {-1.0f, -1.0f, 0.5f, 0.0f, 0.0f, 3.0f, -1.0f, 0.5f, 1.0f, 0.0f, -1.0f, 3.0f, 0.5f, 0.0f, 1.0f};
const u32 kIndices[3] = {0, 1, 2};

bool CenterIsRed(rhi::ISwapChain& target) {
    std::vector<u8> pixels;
    if (!target.ReadBack(pixels) || pixels.size() != static_cast<usize>(kSize) * kSize * 4) return false;
    const usize at = (static_cast<usize>(kSize / 2) * kSize + kSize / 2) * 4;
    return pixels[at] > 200 && pixels[at + 1] < 40 && pixels[at + 2] < 40;
}

} // namespace

static void IndirectDraw_TheArgumentsComeFromTheBuffer(rhi::Backend backend) {
    if (!rhi::IsBackendAvailable(backend)) return;
    std::unique_ptr<rhi::IDevice> device;
    try {
        device = rhi::CreateDevice(backend, false);
    } catch (const std::exception&) {
    }
    if (!device) return;
    if (!device->SupportsIndirectDraw()) return; // D3D12 has no indirect draw yet
    std::unique_ptr<rhi::ISwapChain> swap = device->CreateSwapChain(nullptr, kSize, kSize, 2);
    rhi::PipelineDesc desc;
    desc.hlsl_source = kShader;
    desc.use_vertex_buffer = true;
    desc.vertex_layout = rhi::VertexLayout::PositionUv;
    const rhi::PipelineHandle pipeline = device->CreatePipeline(desc, *swap);
    const rhi::BufferHandle vertices = device->CreateVertexBuffer(kTriangle, sizeof(kTriangle));
    const rhi::BufferHandle indices = device->CreateIndexBuffer(kIndices, sizeof(kIndices), rhi::IndexFormat::UInt32);
    // {index_count, instance_count, first_index, vertex_offset, first_instance} twice: a real draw at offset 0 and an empty one at offset 20.
    const u32 args[10] = {3, 1, 0, 0, 0, 0, 1, 0, 0, 0};
    const rhi::BufferHandle args_buffer = device->CreateStorageBuffer(sizeof(args), 0, args);
    AETHER_CHECK(pipeline.IsValid() && vertices.IsValid() && indices.IsValid() && args_buffer.IsValid());
    if (!pipeline.IsValid() || !vertices.IsValid() || !indices.IsValid() || !args_buffer.IsValid()) return;

    const auto frame = [&](u64 offset, bool expect_drawn) {
        std::unique_ptr<rhi::ICommandList> cmd = device->CreateCommandList();
        swap->AcquireNextImage();
        cmd->Reset();
        cmd->BeginRenderPass(*swap, {0.0f, 0.0f, 1.0f, 1.0f});
        cmd->BindPipeline(pipeline);
        cmd->BindVertexBuffer(vertices, 20);
        cmd->BindIndexBuffer(indices, rhi::IndexFormat::UInt32);
        AETHER_CHECK(cmd->DrawIndexedIndirect(args_buffer, offset));
        cmd->EndRenderPass();
        cmd->Close();
        device->WaitForFence(device->Submit(*cmd, swap.get()));
        AETHER_CHECK(CenterIsRed(*swap) == expect_drawn);
    };
    frame(0, true);
    frame(20, false);

    // Refusals record nothing: outside a pass, a buffer that is not a storage buffer, a misaligned or out-of-range offset, a dead handle.
    std::unique_ptr<rhi::ICommandList> cmd = device->CreateCommandList();
    swap->AcquireNextImage();
    cmd->Reset();
    AETHER_CHECK(!cmd->DrawIndexedIndirect(args_buffer, 0));
    cmd->BeginRenderPass(*swap, {0.0f, 0.0f, 1.0f, 1.0f});
    cmd->BindPipeline(pipeline);
    cmd->BindVertexBuffer(vertices, 20);
    cmd->BindIndexBuffer(indices, rhi::IndexFormat::UInt32);
    AETHER_CHECK(!cmd->DrawIndexedIndirect(vertices, 0));
    AETHER_CHECK(!cmd->DrawIndexedIndirect(args_buffer, 2));
    AETHER_CHECK(!cmd->DrawIndexedIndirect(args_buffer, 24));
    AETHER_CHECK(!cmd->DrawIndexedIndirect({}, 0));
    cmd->EndRenderPass();
    cmd->Close();
    device->WaitForFence(device->Submit(*cmd, swap.get()));

    device->DestroyBuffer(args_buffer);
    device->DestroyBuffer(indices);
    device->DestroyBuffer(vertices);
    device->DestroyPipeline(pipeline);
}

AETHER_TEST(IndirectDraw_TheArgumentsComeFromTheBuffer_D3D12) { IndirectDraw_TheArgumentsComeFromTheBuffer(rhi::Backend::D3D12); }
AETHER_TEST(IndirectDraw_TheArgumentsComeFromTheBuffer_Vulkan) { IndirectDraw_TheArgumentsComeFromTheBuffer(rhi::Backend::Vulkan); }

// A compute shader's result must reach the CPU on a real device: the descriptor sets are bound after the buffers are written into them.
AETHER_TEST(ComputeReadback_AShaderWriteReachesTheCpu_Vulkan) {
    if (!rhi::IsBackendAvailable(rhi::Backend::Vulkan)) return;
    std::unique_ptr<rhi::IDevice> device;
    try {
        device = rhi::CreateDevice(rhi::Backend::Vulkan, false);
    } catch (const std::exception&) {
    }
    if (!device || !device->SupportsStorageBufferRead()) return;
    rhi::ComputePipelineDesc desc;
    desc.hlsl_source = "RWByteAddressBuffer Out : register(u0);\n[numthreads(64,1,1)] void CSMain(uint3 id : SV_DispatchThreadID) { Out.InterlockedAdd(0, 1u); }\n";
    desc.storage_buffer_count = 1;
    const rhi::ComputePipelineHandle pipeline = device->CreateComputePipeline(desc);
    const u32 zeros[4] = {};
    const rhi::BufferHandle storage = device->CreateStorageBuffer(16, 0, zeros);
    AETHER_CHECK(pipeline.IsValid() && storage.IsValid());
    if (!pipeline.IsValid() || !storage.IsValid()) return;
    std::unique_ptr<rhi::ICommandList> cmd = device->CreateComputeCommandList();
    cmd->Reset();
    cmd->BindComputePipeline(pipeline);
    cmd->BindComputeStorageBuffer(0, storage);
    cmd->Dispatch(3); // 192 threads
    cmd->HostReadBarrier(storage);
    cmd->Close();
    device->WaitForComputeFence(device->SubmitCompute(*cmd));
    u32 out[4] = {};
    AETHER_CHECK(device->ReadStorageBuffer(storage, out, sizeof(out)));
    AETHER_CHECK(out[0] == 192u);
    device->DestroyBuffer(storage);
    device->DestroyComputePipeline(pipeline);
}
