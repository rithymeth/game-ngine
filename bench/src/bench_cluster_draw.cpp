// Cluster draw-path benchmark (virtual geometry stage 3, docs/design/VIRTUAL_GEOMETRY.md and the benchmark gate in CLUSTER_GEOMETRY_LOD.md): one frame, end to end
// (record, submit, wait for the GPU), of a lumpy sphere at 1920x1080 seen from 8 m, drawn three ways. The hardware and numbers are in VIRTUAL_GEOMETRY.md.
//
//   draw/direct_<tris>       the existing path: the whole mesh in one DrawIndexed, no cluster LOD, no culling
//   draw/cpu_cull_<tris>     cook::SelectClusters on the CPU, its draw records written by the CPU, one DrawIndexedIndirectCount
//   draw/gpu_cull_<tris>     the cull on the GPU (compute queue, the graphics queue waits on its fence), one DrawIndexedIndirectCount: no CPU work or readback
//
// Each entry's setup also prints, on stderr, the GPU time of the graphics submission alone (timestamps around the pass: median of 31 frames), the draw count and the
// triangles submitted, which the wall-clock figure cannot say. The cut is a 1-pixel error budget with back-face culling, as in the cull benchmark. Needs a Vulkan
// device with indirect count; without one the entries do nothing and say so on stderr.
#include "../../player/src/cluster_cull_pass.h"
#include "aether/bench/bench.h"
#include "aether/cook/cluster_hierarchy.h"
#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace aether;
using namespace aether::cook;

namespace {

constexpr u32 kWidth = 1920, kHeight = 1080;
constexpr char kShader[] = R"(
struct VSInput { float3 position : POSITION; float2 uv : TEXCOORD0; };
float4 VSMain(VSInput input) : SV_Position {
    float3 p = input.position - float3(0.0, 0.0, 8.0); // the camera: 8 m out on +z, looking down -z, 1 radian vertical field of view
    float f = 1.8304877;
    float depth = -p.z;
    return float4(p.x * f * (1080.0 / 1920.0), p.y * f, (depth - 0.1) * 0.01 * depth, depth);
}
float4 PSMain() : SV_Target { return float4(0.9, 0.8, 0.7, 1.0); }
)";

enum class Path { Direct, CpuCull, GpuCull };

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

ClusterView View() {
    ClusterView v;
    v.position = Vec3(0, 0, 8.0f);
    v.vertical_fov = 1.0f;
    v.viewport_height = static_cast<f32>(kHeight);
    v.cull_backfaces = true;
    return v;
}

struct Scene {
    u32 sphere_n = 0;
    std::unique_ptr<gfx::rhi::IDevice> device; // first: destroyed last
    std::vector<u32> mesh_indices;
    ClusterHierarchy hierarchy;
    player::ClusterCullPass pass;
    std::unique_ptr<gfx::rhi::ISwapChain> swap;
    gfx::rhi::PipelineHandle pipeline;
    gfx::rhi::BufferHandle vertex_buffer, mesh_index_buffer, cluster_index_buffer, cpu_draws, cpu_count;
    gfx::rhi::GpuTimerHandle timer;
    ClusterSelection selection; // the CPU cut, also what the GPU's should equal
    bool tried = false, ok = false;

    bool Prepare() {
        if (tried) return ok;
        tried = true;
        if (!gfx::rhi::IsBackendAvailable(gfx::rhi::Backend::Vulkan)) {
            std::fprintf(stderr, "draw bench: no Vulkan device: the entries do nothing\n");
            return false;
        }
        try {
            device = gfx::rhi::CreateDevice(gfx::rhi::Backend::Vulkan, false);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "draw bench: Vulkan failed (%s): the entries do nothing\n", e.what());
            return false;
        }
        if (!device || !device->SupportsIndirectCount()) {
            std::fprintf(stderr, "draw bench: the device has no indirect count: the entries do nothing\n");
            return false;
        }
        std::vector<Vec3> positions;
        MakeSphere(sphere_n, positions, mesh_indices);
        if (!BuildClusterHierarchy(positions, mesh_indices, {}, hierarchy)) return false;
        std::string error;
        if (!pass.Initialize(*device, hierarchy, 0, &error)) {
            std::fprintf(stderr, "draw bench: the cull pass did not start (%s)\n", error.c_str());
            return false;
        }
        SelectClusters(hierarchy, View(), 1.0f, selection);
        std::vector<f32> vertices;
        for (const Vec3& p : positions) {
            const f32 v[5] = {p.x, p.y, p.z, 0.0f, 0.0f};
            vertices.insert(vertices.end(), v, v + 5);
        }
        swap = device->CreateSwapChain(nullptr, kWidth, kHeight, 2);
        gfx::rhi::PipelineDesc desc;
        desc.hlsl_source = kShader;
        desc.use_vertex_buffer = true;
        desc.vertex_layout = gfx::rhi::VertexLayout::PositionUv;
        pipeline = device->CreatePipeline(desc, *swap);
        vertex_buffer = device->CreateVertexBuffer(vertices.data(), vertices.size() * sizeof(f32));
        mesh_index_buffer = device->CreateIndexBuffer(mesh_indices.data(), mesh_indices.size() * sizeof(u32), gfx::rhi::IndexFormat::UInt32);
        cluster_index_buffer = device->CreateIndexBuffer(hierarchy.indices.data(), hierarchy.indices.size() * sizeof(u32), gfx::rhi::IndexFormat::UInt32);
        cpu_draws = device->CreateStorageBuffer(static_cast<u64>(hierarchy.clusters.size()) * 20);
        cpu_count = device->CreateStorageBuffer(16);
        timer = device->SupportsGpuTiming() ? device->CreateGpuTimer(2) : gfx::rhi::GpuTimerHandle{};
        ok = pipeline.IsValid() && vertex_buffer.IsValid() && mesh_index_buffer.IsValid() && cluster_index_buffer.IsValid() && cpu_draws.IsValid() &&
             cpu_count.IsValid();
        std::fprintf(stderr, "draw bench: %zu triangles, %zu clusters; the 1-pixel cut at 8 m keeps %zu clusters / %zu triangles\n", mesh_indices.size() / 3,
                     hierarchy.clusters.size(), selection.clusters.size(), selection.triangles);
        return ok;
    }

    // One frame; returns the graphics submission's GPU time in ms (0 without timers).
    double Frame(Path path) {
        using namespace gfx::rhi;
        u64 compute_fence = 0;
        if (path == Path::GpuCull) {
            std::unique_ptr<ICommandList> compute = device->CreateComputeCommandList();
            compute->Reset();
            pass.Record(*compute, View(), 1.0f);
            compute->Close();
            compute_fence = device->SubmitCompute(*compute);
        } else if (path == Path::CpuCull) {
            ClusterSelection cut;
            SelectClusters(hierarchy, View(), 1.0f, cut);
            std::vector<u32> records(cut.clusters.size() * 5);
            for (usize i = 0; i < cut.clusters.size(); ++i) {
                const Cluster& c = hierarchy.clusters[cut.clusters[i]];
                const u32 r[5] = {c.triangle_count * 3, 1, c.index_offset, 0, 0};
                std::memcpy(&records[i * 5], r, sizeof(r));
            }
            if (!records.empty()) device->UpdateStorageBuffer(cpu_draws, records.data(), records.size() * sizeof(u32));
            const u32 count[4] = {static_cast<u32>(cut.clusters.size()), 0, 0, 0};
            device->UpdateStorageBuffer(cpu_count, count, sizeof(count));
        }
        std::unique_ptr<ICommandList> cmd = device->CreateCommandList();
        swap->AcquireNextImage();
        cmd->Reset();
        if (timer.IsValid()) cmd->WriteGpuTimestamp(timer, 0);
        cmd->BeginRenderPass(*swap, {0.0f, 0.0f, 0.0f, 1.0f});
        cmd->BindPipeline(pipeline);
        cmd->BindVertexBuffer(vertex_buffer, 20);
        if (path == Path::Direct) {
            cmd->BindIndexBuffer(mesh_index_buffer, IndexFormat::UInt32);
            cmd->DrawIndexed(static_cast<u32>(mesh_indices.size()));
        } else {
            cmd->BindIndexBuffer(cluster_index_buffer, IndexFormat::UInt32);
            if (path == Path::GpuCull) cmd->DrawIndexedIndirectCount(pass.DrawArguments(), pass.DrawCount(), pass.Capacity());
            else cmd->DrawIndexedIndirectCount(cpu_draws, cpu_count, static_cast<u32>(hierarchy.clusters.size()));
        }
        cmd->EndRenderPass();
        if (timer.IsValid()) {
            cmd->WriteGpuTimestamp(timer, 1);
            cmd->ResolveGpuTimer(timer);
        }
        cmd->Close();
        if (compute_fence != 0) device->GraphicsQueueWaitOnCompute(compute_fence);
        device->WaitForFence(device->Submit(*cmd, swap.get()));
        double ms[2] = {0.0, 0.0};
        if (timer.IsValid() && device->ReadGpuTimer(timer, ms)) return ms[1];
        return 0.0;
    }

    void Report(Path path, const char* name) {
        if (!Prepare()) return;
        std::vector<double> gpu;
        for (u32 i = 0; i < 31; ++i) gpu.push_back(Frame(path));
        std::sort(gpu.begin(), gpu.end());
        const usize triangles = path == Path::Direct ? mesh_indices.size() / 3 : selection.triangles;
        const usize draws = path == Path::Direct ? 1 : selection.clusters.size();
        std::fprintf(stderr, "draw bench: %-9s %zu draws, %zu triangles submitted, graphics GPU time median %.3f ms (min %.3f, max %.3f)\n", name, draws, triangles,
                     gpu[gpu.size() / 2], gpu.front(), gpu.back());
    }
};

Scene& SceneFor(u32 sphere_n) {
    static std::vector<std::unique_ptr<Scene>> scenes;
    for (auto& s : scenes) if (s->sphere_n == sphere_n) return *s;
    scenes.push_back(std::make_unique<Scene>());
    scenes.back()->sphere_n = sphere_n;
    return *scenes.back();
}

void Register(u32 sphere_n, const char* tag, Path path, const char* name) {
    bench::Registry().push_back({std::string("draw/") + name + "_" + tag, [sphere_n, path, name] { SceneFor(sphere_n).Report(path, name); },
                                 [sphere_n, path] {
                                     Scene& s = SceneFor(sphere_n);
                                     if (s.ok) s.Frame(path);
                                 },
                                 1});
}

const bool kRegistered = [] {
    for (const auto& [n, tag] : {std::pair<u32, const char*>{130, "68k"}, {230, "212k"}}) {
        Register(n, tag, Path::Direct, "direct");
        Register(n, tag, Path::CpuCull, "cpu_cull");
        Register(n, tag, Path::GpuCull, "gpu_cull");
    }
    return true;
}();

} // namespace
