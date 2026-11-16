// Cluster culling benchmark (virtual geometry stage 2, docs/design/VIRTUAL_GEOMETRY.md): the cluster cut and frustum/cone culling on the CPU
// (cook::SelectClusters) against the same work on the GPU (player/src/cluster_cull_pass.h), on lumpy spheres of growing density. The GPU runs on Vulkan
// (it needs storage-buffer readback only for checking, not for timing, but Vulkan is the backend with the pass's host-side buffer updates tested).
//
//   cluster/cpu_select_<tris>     one SelectClusters over the whole hierarchy
//   cluster/gpu_cull_<tris>       one cull: constants and counters written, one dispatch recorded, submitted, waited for: the latency of asking once
//   cluster/gpu_cull_x16_<tris>   16 dispatches in one submission, divide by 16: what a cull costs when many are batched
//
// The question it answers is only whether culling clusters on the GPU is cheaper than on the CPU at this scale. It is NOT the comparison the design doc's
// benchmark gate asks for (the existing draw path against cluster rendering): that needs the draw stage. On a machine with no Vulkan device the GPU entries
// do nothing and say so on stderr. The 1M-triangle size is only registered with AETHER_BENCH_CLUSTER_LARGE=1 (its hierarchy takes tens of seconds to build).
#include "../../player/src/cluster_cull_pass.h"
#include "aether/bench/bench.h"
#include "aether/cook/cluster_hierarchy.h"
#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace aether;
using namespace aether::cook;

namespace {

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
    v.viewport_height = 1080.0f;
    v.cull_backfaces = true;
    return v;
}

// One density: its hierarchy, and (once a GPU benchmark asks) a device and the pass on it. The device is declared first so it is destroyed last.
struct Scene {
    u32 sphere_n = 0;
    std::unique_ptr<gfx::rhi::IDevice> device;
    ClusterHierarchy hierarchy;
    player::ClusterCullPass pass;
    bool built = false;
    bool gpu_tried = false;

    void Build() {
        if (built) return;
        built = true;
        std::vector<Vec3> positions;
        std::vector<u32> indices;
        MakeSphere(sphere_n, positions, indices);
        if (!BuildClusterHierarchy(positions, indices, {}, hierarchy)) std::fprintf(stderr, "cluster bench: the hierarchy did not build\n");
        ClusterSelection sel;
        SelectClusters(hierarchy, View(), 1.0f, sel);
        std::fprintf(stderr, "cluster bench: %zu triangles, %zu clusters, the cut at 8 m keeps %zu clusters / %zu triangles\n", indices.size() / 3,
                     hierarchy.clusters.size(), sel.clusters.size(), sel.triangles);
    }
    bool Gpu() {
        Build();
        if (gpu_tried) return pass.Valid();
        gpu_tried = true;
        if (!gfx::rhi::IsBackendAvailable(gfx::rhi::Backend::Vulkan)) {
            std::fprintf(stderr, "cluster bench: no Vulkan device: the GPU entries do nothing\n");
            return false;
        }
        try {
            device = gfx::rhi::CreateDevice(gfx::rhi::Backend::Vulkan, false);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "cluster bench: Vulkan failed (%s): the GPU entries do nothing\n", e.what());
            return false;
        }
        std::string error;
        if (!device || !pass.Initialize(*device, hierarchy, 0, &error)) {
            std::fprintf(stderr, "cluster bench: the GPU pass did not start (%s): the GPU entries do nothing\n", error.c_str());
            return false;
        }
        return true;
    }
    // `culls` dispatches in one submission, waited for.
    void CullOnGpu(u32 culls) {
        std::unique_ptr<gfx::rhi::ICommandList> cmd = device->CreateComputeCommandList();
        cmd->Reset();
        for (u32 i = 0; i < culls; ++i) pass.Record(*cmd, View(), 1.0f); // every record rewrites the same constants: only the timing matters here
        cmd->Close();
        device->WaitForComputeFence(device->SubmitCompute(*cmd));
    }
};

Scene& SceneFor(u32 sphere_n) {
    static std::vector<std::unique_ptr<Scene>> scenes;
    for (auto& s : scenes) if (s->sphere_n == sphere_n) return *s;
    scenes.push_back(std::make_unique<Scene>());
    scenes.back()->sphere_n = sphere_n;
    return *scenes.back();
}

void RegisterDensity(u32 sphere_n, const char* label) {
    const std::string tag = label;
    bench::Registry().push_back({"cluster/cpu_select_" + tag, [sphere_n] { SceneFor(sphere_n).Build(); },
                                 [sphere_n] {
                                     ClusterSelection sel;
                                     SelectClusters(SceneFor(sphere_n).hierarchy, View(), 1.0f, sel);
                                 },
                                 1});
    bench::Registry().push_back({"cluster/gpu_cull_" + tag, [sphere_n] { SceneFor(sphere_n).Gpu(); },
                                 [sphere_n] {
                                     Scene& s = SceneFor(sphere_n);
                                     if (s.pass.Valid()) s.CullOnGpu(1);
                                 },
                                 1});
    bench::Registry().push_back({"cluster/gpu_cull_x16_" + tag, [sphere_n] { SceneFor(sphere_n).Gpu(); },
                                 [sphere_n] {
                                     Scene& s = SceneFor(sphere_n);
                                     if (s.pass.Valid()) s.CullOnGpu(16);
                                 },
                                 1});
}

// A sphere of n rings has 4 n^2 triangles: 130 is about 68k, 230 about 212k, 500 about 1M.
const bool kRegistered = [] {
    RegisterDensity(130, "68k");
    RegisterDensity(230, "212k");
    const char* large = std::getenv("AETHER_BENCH_CLUSTER_LARGE");
    if (large && *large && *large != '0') RegisterDensity(500, "1M");
    return true;
}();

} // namespace
