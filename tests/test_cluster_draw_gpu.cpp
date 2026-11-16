#include "../player/src/cluster_cull_pass.h"
#include "test_framework.h"

#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

// Virtual geometry stage 3: the cluster cull pass's list drives DrawIndexedIndirectCount with nothing read back. At a threshold of 0 the cut is the leaf
// level, which is the whole mesh, so the picture must equal drawing the mesh's indices directly, pixel for pixel; a coarser cut draws fewer triangles and
// still covers the middle; a frustum that sees nothing draws nothing.

using namespace aether;
using namespace aether::cook;
using namespace aether::player;
using namespace aether::gfx::rhi;

namespace {

constexpr u32 kSize = 64;
constexpr char kShader[] = R"(
struct VSInput { float3 position : POSITION; float2 uv : TEXCOORD0; };
float4 VSMain(VSInput input) : SV_Position { return float4(input.position.xy * 0.45, input.position.z * 0.1 + 0.5, 1.0); }
float4 PSMain() : SV_Target { return float4(1.0, 0.0, 0.0, 1.0); }
)";

void MakeSphere(u32 n, std::vector<Vec3>& positions, std::vector<u32>& indices) {
    positions.clear();
    indices.clear();
    const u32 rings = n, segments = 2 * n;
    for (u32 r = 0; r <= rings; ++r) {
        const f32 phi = 3.14159265f * static_cast<f32>(r) / static_cast<f32>(rings);
        for (u32 s = 0; s <= segments; ++s) {
            const f32 theta = 2.0f * 3.14159265f * static_cast<f32>(s) / static_cast<f32>(segments);
            const f32 lump = 1.0f + 0.08f * std::sin(5.0f * theta) * std::sin(4.0f * phi) + 0.03f * std::sin(13.0f * theta + 2.0f * phi);
            positions.push_back(Vec3(lump * std::sin(phi) * std::cos(theta), lump * std::cos(phi), lump * std::sin(phi) * std::sin(theta)));
        }
    }
    for (u32 r = 0; r < rings; ++r) {
        for (u32 s = 0; s < segments; ++s) {
            const u32 a = r * (segments + 1) + s, b = a + 1, c = a + (segments + 1), d = c + 1;
            for (const u32 i : {a, c, b, b, c, d}) indices.push_back(i);
        }
    }
}

std::unique_ptr<IDevice> MakeDevice() {
    if (!IsBackendAvailable(Backend::Vulkan)) return nullptr;
    try {
        std::unique_ptr<IDevice> device = CreateDevice(Backend::Vulkan, false);
        if (device && device->SupportsIndirectCount()) return device;
    } catch (const std::exception&) {
    }
    return nullptr;
}

} // namespace

AETHER_TEST(ClusterDrawGpu_TheCullListDrivesTheDrawWithNothingReadBack) {
    std::unique_ptr<IDevice> device = MakeDevice();
    if (!device) {
        std::printf("    (no Vulkan device with indirect count)\n");
        return;
    }
    std::vector<Vec3> positions;
    std::vector<u32> indices;
    MakeSphere(40, positions, indices);
    ClusterHierarchy h;
    AETHER_CHECK(BuildClusterHierarchy(positions, indices, {}, h));
    ClusterCullPass pass;
    AETHER_CHECK(pass.Initialize(*device, h));
    if (!pass.Valid()) return;

    std::vector<f32> vertices;
    for (const Vec3& p : positions) {
        const f32 v[5] = {p.x, p.y, p.z, 0.0f, 0.0f};
        vertices.insert(vertices.end(), v, v + 5);
    }
    std::unique_ptr<ISwapChain> swap = device->CreateSwapChain(nullptr, kSize, kSize, 2);
    PipelineDesc desc;
    desc.hlsl_source = kShader;
    desc.use_vertex_buffer = true;
    desc.vertex_layout = VertexLayout::PositionUv;
    const PipelineHandle pipeline = device->CreatePipeline(desc, *swap);
    const BufferHandle vertex_buffer = device->CreateVertexBuffer(vertices.data(), vertices.size() * sizeof(f32));
    const BufferHandle cluster_indices = device->CreateIndexBuffer(h.indices.data(), h.indices.size() * sizeof(u32), IndexFormat::UInt32);
    const BufferHandle mesh_indices = device->CreateIndexBuffer(indices.data(), indices.size() * sizeof(u32), IndexFormat::UInt32);
    AETHER_CHECK(pipeline.IsValid() && vertex_buffer.IsValid() && cluster_indices.IsValid() && mesh_indices.IsValid());
    if (!pipeline.IsValid() || !vertex_buffer.IsValid() || !cluster_indices.IsValid() || !mesh_indices.IsValid()) return;

    const auto pixels = [&](bool direct, const ClusterView& view, f32 threshold, u64& red) {
        std::vector<u8> out;
        if (!direct) {
            std::unique_ptr<ICommandList> compute = device->CreateComputeCommandList();
            compute->Reset();
            AETHER_CHECK(pass.Record(*compute, view, threshold));
            compute->Close();
            device->WaitForComputeFence(device->SubmitCompute(*compute));
        }
        std::unique_ptr<ICommandList> cmd = device->CreateCommandList();
        swap->AcquireNextImage();
        cmd->Reset();
        cmd->BeginRenderPass(*swap, {0.0f, 0.0f, 1.0f, 1.0f});
        cmd->BindPipeline(pipeline);
        cmd->BindVertexBuffer(vertex_buffer, 20);
        if (direct) {
            cmd->BindIndexBuffer(mesh_indices, IndexFormat::UInt32);
            cmd->DrawIndexed(static_cast<u32>(indices.size()));
        } else {
            cmd->BindIndexBuffer(cluster_indices, IndexFormat::UInt32);
            AETHER_CHECK(cmd->DrawIndexedIndirectCount(pass.DrawArguments(), pass.DrawCount(), pass.Capacity()));
        }
        cmd->EndRenderPass();
        cmd->Close();
        device->WaitForFence(device->Submit(*cmd, swap.get()));
        AETHER_CHECK(swap->ReadBack(out) && out.size() == static_cast<usize>(kSize) * kSize * 4);
        red = 0;
        for (usize i = 0; i + 3 < out.size(); i += 4) red += out[i] > 200 && out[i + 1] < 40 && out[i + 2] < 40 ? 1u : 0u;
        return out;
    };

    ClusterView view;
    view.position = Vec3(0, 0, 10);
    view.vertical_fov = 1.0f;
    view.viewport_height = 1080.0f;
    u64 red_direct = 0, red_clusters = 0, red_coarse = 0, red_none = 0;
    const std::vector<u8> direct = pixels(true, view, 0.0f, red_direct);
    const std::vector<u8> leaves = pixels(false, view, 0.0f, red_clusters);
    AETHER_CHECK(red_direct > 400); // the sphere is on screen
    AETHER_CHECK(direct == leaves); // the leaf cut is the whole mesh: the same picture, pixel for pixel

    // A coarse cut (a loose pixel budget) draws a much smaller index count and still fills most of the silhouette.
    ClusterSelection coarse_cpu, fine_cpu;
    AETHER_CHECK(SelectClusters(h, view, 40.0f, coarse_cpu) && SelectClusters(h, view, 0.0f, fine_cpu));
    AETHER_CHECK(coarse_cpu.triangles < fine_cpu.triangles / 2);
    pixels(false, view, 40.0f, red_coarse);
    AETHER_CHECK(red_coarse > red_direct / 2 && red_coarse < red_direct * 2);

    // A frustum that sees nothing: the count is zero and nothing is drawn.
    ClusterView away = view;
    away.has_frustum = true;
    away.frustum = {ClusterPlane{Vec3(0, 0, 1), -500.0f}, ClusterPlane{Vec3(0, 0, 1), 0.0f}, ClusterPlane{Vec3(0, 0, 1), 0.0f},
                    ClusterPlane{Vec3(0, 0, 1), 0.0f}, ClusterPlane{Vec3(0, 0, 1), 0.0f}, ClusterPlane{Vec3(0, 0, 1), 0.0f}};
    pixels(false, away, 0.0f, red_none);
    AETHER_CHECK(red_none == 0);

    // Refusals record nothing: no draws allowed, a buffer that is not a storage buffer, more draws than the arguments hold, a misaligned offset, a dead handle.
    {
        std::unique_ptr<ICommandList> cmd = device->CreateCommandList();
        swap->AcquireNextImage();
        cmd->Reset();
        cmd->BeginRenderPass(*swap, {0.0f, 0.0f, 1.0f, 1.0f});
        cmd->BindPipeline(pipeline);
        cmd->BindVertexBuffer(vertex_buffer, 20);
        cmd->BindIndexBuffer(cluster_indices, IndexFormat::UInt32);
        AETHER_CHECK(!cmd->DrawIndexedIndirectCount(pass.DrawArguments(), pass.DrawCount(), 0));
        AETHER_CHECK(!cmd->DrawIndexedIndirectCount(vertex_buffer, pass.DrawCount(), 1));
        AETHER_CHECK(!cmd->DrawIndexedIndirectCount(pass.DrawArguments(), vertex_buffer, 1));
        AETHER_CHECK(!cmd->DrawIndexedIndirectCount(pass.DrawArguments(), pass.DrawCount(), pass.Capacity() + 1));
        AETHER_CHECK(!cmd->DrawIndexedIndirectCount(pass.DrawArguments(), pass.DrawCount(), 1, 2));
        AETHER_CHECK(!cmd->DrawIndexedIndirectCount({}, pass.DrawCount(), 1));
        cmd->EndRenderPass();
        cmd->Close();
        device->WaitForFence(device->Submit(*cmd, swap.get()));
    }

    device->DestroyBuffer(mesh_indices);
    device->DestroyBuffer(cluster_indices);
    device->DestroyBuffer(vertex_buffer);
    device->DestroyPipeline(pipeline);
}
