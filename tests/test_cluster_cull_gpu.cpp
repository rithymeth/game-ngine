#include "../player/src/cluster_cull_pass.h"
#include "test_framework.h"

#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <memory>
#include <set>

// Stage 2 of virtual geometry: the GPU cull (player/src/cluster_cull_pass.h) must pick the same clusters as the CPU selector (cook::SelectClusters), for
// every distance, threshold and culling mode. Runs on a device that can read storage buffers back (Vulkan: lavapipe on Linux); skips with a note on others.

using namespace aether;
using namespace aether::cook;
using namespace aether::player;
using namespace aether::gfx::rhi;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

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

ClusterView ViewAt(f32 distance) {
    ClusterView v;
    v.position = Vec3(0, 0, distance);
    v.vertical_fov = 1.0f;
    v.viewport_height = 1080.0f;
    return v;
}

std::unique_ptr<IDevice> MakeDevice() {
    if (!IsBackendAvailable(Backend::Vulkan)) return nullptr;
    try {
        std::unique_ptr<IDevice> device = CreateDevice(Backend::Vulkan, false);
        if (device && device->SupportsStorageBufferRead()) return device;
    } catch (const std::exception&) {
    }
    return nullptr;
}

// Runs one cull on the GPU and reads it back; false if anything about the submission failed.
bool RunGpu(IDevice& device, ClusterCullPass& pass, const ClusterView& view, f32 threshold, GpuClusterSelection& out) {
    std::unique_ptr<ICommandList> cmd = device.CreateComputeCommandList();
    cmd->Reset();
    const bool recorded = pass.Record(*cmd, view, threshold);
    cmd->Close();
    if (!recorded) return false;
    device.WaitForComputeFence(device.SubmitCompute(*cmd));
    return pass.Read(out);
}

// The clusters whose test is so close to the threshold that a different rounding on the GPU could go either way: a mismatch there is not a bug.
std::set<u32> Borderline(const ClusterHierarchy& h, const ClusterView& view, f32 threshold) {
    std::set<u32> out;
    for (u32 i = 0; i < h.clusters.size(); ++i) {
        const Cluster& c = h.clusters[i];
        const f32 own = ProjectedError(c.center, c.radius, c.error, view);
        const f32 parent = ProjectedError(c.parent_center, c.parent_radius, c.parent_error, view);
        const auto close = [&](f32 v) { return std::isfinite(v) && std::fabs(v - threshold) <= 1e-3f * std::max(threshold, 1.0f); };
        if (close(own) || close(parent)) out.insert(i);
    }
    return out;
}

} // namespace

AETHER_TEST(ClusterCullGpu_PicksTheSameClustersAsTheCpuSelector) {
    std::unique_ptr<IDevice> device = MakeDevice();
    if (!device) {
        std::printf("    (no Vulkan device that can read storage buffers: install Mesa's lavapipe to run this)\n");
        return;
    }
    std::vector<Vec3> positions;
    std::vector<u32> indices;
    MakeSphere(40, positions, indices);
    ClusterHierarchy h;
    CHECK(BuildClusterHierarchy(positions, indices, {}, h));
    ClusterCullPass pass;
    std::string error;
    CHECK(pass.Initialize(*device, h, 0, &error));
    if (!pass.Valid()) return;
    CHECK(pass.ClusterCount() == h.clusters.size() && pass.Capacity() == h.clusters.size());

    // A frustum around a thin column through the sphere (x, y within 0.4, looking down -z), and the camera's cone culling, on and off.
    const std::array<ClusterPlane, 6> column = {ClusterPlane{Vec3(1, 0, 0), 0.4f}, ClusterPlane{Vec3(-1, 0, 0), 0.4f}, ClusterPlane{Vec3(0, 1, 0), 0.4f},
                                                ClusterPlane{Vec3(0, -1, 0), 0.4f}, ClusterPlane{Vec3(0, 0, -1), 1000.0f}, ClusterPlane{Vec3(0, 0, 1), 1000.0f}};
    usize compared = 0, mismatched_clusters = 0;
    for (const f32 distance : {1.5f, 3.0f, 8.0f, 25.0f, 80.0f, 300.0f, 2000.0f}) {
        for (const f32 threshold : {0.0f, 0.5f, 1.0f, 4.0f}) {
            for (int mode = 0; mode < 4; ++mode) {
                ClusterView view = ViewAt(distance);
                view.has_frustum = (mode & 1) != 0;
                view.cull_backfaces = (mode & 2) != 0;
                if (view.has_frustum) view.frustum = column;
                ClusterSelection cpu;
                GpuClusterSelection gpu;
                CHECK(SelectClusters(h, view, threshold, cpu));
                CHECK(RunGpu(*device, pass, view, threshold, gpu));
                CHECK(!gpu.overflow && gpu.needed == gpu.clusters.size());
                std::set<u32> a(cpu.clusters.begin(), cpu.clusters.end()), b(gpu.clusters.begin(), gpu.clusters.end());
                CHECK(b.size() == gpu.clusters.size()); // no cluster listed twice
                const std::set<u32> edge = Borderline(h, view, threshold);
                std::set<u32> differing;
                for (const u32 id : a) if (!b.count(id)) differing.insert(id);
                for (const u32 id : b) if (!a.count(id)) differing.insert(id);
                bool only_borderline = true;
                for (const u32 id : differing) only_borderline = only_borderline && edge.count(id) != 0;
                CHECK(only_borderline);
                if (differing.empty()) CHECK(gpu.triangles == cpu.triangles);
                mismatched_clusters += differing.size();
                ++compared;
            }
        }
    }
    CHECK(compared == 7 * 4 * 4);
    CHECK(mismatched_clusters == 0); // in practice they agree exactly: the borderline allowance is only for another driver's rounding
}

AETHER_TEST(ClusterCullGpu_CullingAndTheThresholdChangeTheListAsTheyShould) {
    std::unique_ptr<IDevice> device = MakeDevice();
    if (!device) return;
    std::vector<Vec3> positions;
    std::vector<u32> indices;
    MakeSphere(40, positions, indices);
    ClusterHierarchy h;
    CHECK(BuildClusterHierarchy(positions, indices, {}, h));
    ClusterCullPass pass;
    CHECK(pass.Initialize(*device, h));
    if (!pass.Valid()) return;
    GpuClusterSelection open, culled, loose, tight, away;
    ClusterView view = ViewAt(4.0f);
    CHECK(RunGpu(*device, pass, view, 1.0f, open));
    view.cull_backfaces = true;
    CHECK(RunGpu(*device, pass, view, 1.0f, culled));
    CHECK(!culled.clusters.empty() && culled.clusters.size() < open.clusters.size()); // the far side of the sphere faces away
    view.cull_backfaces = false;
    // A looser pixel budget keeps fewer triangles, a tighter one more (never the other way round); at 10 m the extremes differ for certain.
    view = ViewAt(10.0f);
    GpuClusterSelection mid;
    CHECK(RunGpu(*device, pass, view, 8.0f, loose) && RunGpu(*device, pass, view, 1.0f, mid) && RunGpu(*device, pass, view, 0.5f, tight));
    CHECK(loose.triangles <= mid.triangles && mid.triangles <= tight.triangles && loose.triangles < tight.triangles);
    view = ViewAt(4.0f);
    // A frustum that sees nothing: no clusters, and the pass is reusable afterwards (its counters start from zero each time).
    view.has_frustum = true;
    view.frustum = {ClusterPlane{Vec3(0, 0, 1), -500.0f}, ClusterPlane{Vec3(0, 0, 1), 0.0f}, ClusterPlane{Vec3(0, 0, 1), 0.0f},
                    ClusterPlane{Vec3(0, 0, 1), 0.0f}, ClusterPlane{Vec3(0, 0, 1), 0.0f}, ClusterPlane{Vec3(0, 0, 1), 0.0f}};
    CHECK(RunGpu(*device, pass, view, 1.0f, away) && away.clusters.empty() && away.triangles == 0 && !away.overflow);
    GpuClusterSelection again;
    CHECK(RunGpu(*device, pass, ViewAt(4.0f), 1.0f, again) && again.clusters.size() == open.clusters.size() && again.triangles == open.triangles);
}

AETHER_TEST(ClusterCullGpu_AFullOutputReportsHowManyWereNeeded) {
    std::unique_ptr<IDevice> device = MakeDevice();
    if (!device) return;
    std::vector<Vec3> positions;
    std::vector<u32> indices;
    MakeSphere(40, positions, indices);
    ClusterHierarchy h;
    CHECK(BuildClusterHierarchy(positions, indices, {}, h));
    ClusterSelection cpu;
    const ClusterView view = ViewAt(3.0f);
    CHECK(SelectClusters(h, view, 0.5f, cpu) && cpu.clusters.size() > 12);
    ClusterCullPass pass;
    CHECK(pass.Initialize(*device, h, 12));
    if (!pass.Valid()) return;
    GpuClusterSelection gpu;
    CHECK(RunGpu(*device, pass, view, 0.5f, gpu));
    CHECK(gpu.overflow && gpu.needed == cpu.clusters.size() && gpu.clusters.size() == 12 && gpu.triangles == cpu.triangles);
    const std::set<u32> allowed(cpu.clusters.begin(), cpu.clusters.end());
    for (const u32 id : gpu.clusters) CHECK(allowed.count(id) == 1); // what fit is a subset of the right answer
}

AETHER_TEST(ClusterCullGpu_RefusesWhatItCannotUse) {
    std::unique_ptr<IDevice> device = MakeDevice();
    if (!device) return;
    ClusterCullPass pass;
    std::string error;
    ClusterHierarchy empty;
    CHECK(!pass.Initialize(*device, empty, 0, &error) && !error.empty() && !pass.Valid());
    GpuClusterSelection out;
    CHECK(!pass.Read(out));
    std::vector<Vec3> positions;
    std::vector<u32> indices;
    MakeSphere(24, positions, indices);
    ClusterHierarchy h;
    CHECK(BuildClusterHierarchy(positions, indices, {}, h));
    CHECK(pass.Initialize(*device, h));
    if (!pass.Valid()) return;
    std::unique_ptr<ICommandList> cmd = device->CreateComputeCommandList();
    cmd->Reset();
    CHECK(!pass.Record(*cmd, ViewAt(5.0f), std::nanf("")));
    ClusterView bad = ViewAt(5.0f);
    bad.viewport_height = 0.0f;
    CHECK(!pass.Record(*cmd, bad, 1.0f));
    bad = ViewAt(5.0f);
    bad.position.x = INFINITY;
    CHECK(!pass.Record(*cmd, bad, 1.0f));
    bad = ViewAt(5.0f);
    bad.vertical_fov = 0.0f;
    CHECK(!pass.Record(*cmd, bad, 1.0f));
    cmd->Close();
    pass.Release();
    CHECK(!pass.Valid() && !pass.Read(out));
    pass.Release(); // twice is fine
}
