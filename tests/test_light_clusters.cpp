#include "../player/src/frame_lights.h"
#include "../player/src/light_clusters.h"
#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"
#include "aether/player/scene_renderer.h"
#include "aether/renderer/components.h"
#include "aether/scene/components.h"
#include "aether/scene/hierarchy.h"
#include "test_framework.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace aether;
using namespace aether::player;
namespace rhi = aether::gfx::rhi;

// Render roadmap step 5: clustered forward lighting. The CPU bins every light in view into a 16 x 9 x 24 grid of the view frustum
// (player/src/light_clusters.h); the scene shader finds its pixel's cell through the RHI's frame read buffer and shades those lights.

namespace {

constexpr f32 kTestPi = 3.14159265358979f;

// A camera at the origin looking down -Z: 90 degrees vertically, 16:9, 0.1 to 1000.
ClusterView TestView() {
    ClusterView view;
    view.view = Mat4::Identity();
    view.projection = Mat4::PerspectiveRH(kTestPi / 2.0f, 16.0f / 9.0f, 0.1f, 1000.0f);
    view.near_plane = 0.1f;
    view.far_plane = 1000.0f;
    return view;
}

u32 Cell(u32 x, u32 y, u32 s) { return (s * kClusterTilesY + y) * kClusterTilesX + x; }

// The lights (positions in the kept list) the built buffer gives a cell.
std::vector<u32> LightsIn(const LightClusters& clusters, u32 cell) {
    u32 run[2] = {0, 0};
    std::memcpy(run, clusters.bytes.data() + clusters.cells_offset + cell * 8, sizeof(run));
    std::vector<u32> out(run[1]);
    for (u32 i = 0; i < run[1]; ++i) {
        u16 index = 0;
        std::memcpy(&index, clusters.bytes.data() + clusters.indices_offset + (run[0] + i) * 2, sizeof(index));
        out[i] = index;
    }
    return out;
}

f32 KeptX(const LightClusters& clusters, u32 k) {
    f32 x = 0.0f;
    std::memcpy(&x, clusters.bytes.data() + k * sizeof(ClusterLight), sizeof(x));
    return x;
}

std::unique_ptr<rhi::IDevice> MakeDevice(rhi::Backend backend) {
    std::unique_ptr<rhi::IDevice> device;
    if (rhi::IsBackendAvailable(backend)) {
        try {
            device = rhi::CreateDevice(backend, false);
        } catch (const std::exception&) {
        }
    }
    if (!device) std::printf("    (no %s device: skipping)\n", backend == rhi::Backend::D3D12 ? "D3D12" : "Vulkan");
    return device;
}

} // namespace

AETHER_TEST(Clusters_FrameDataCarriesTheGrid) {
    static_assert(sizeof(FrameData) == 1216 && offsetof(FrameData, cluster_forward) == 1024 && offsetof(FrameData, cluster_info) == 1056 &&
                  offsetof(FrameData, vsm_light_right) == 1072 && offsetof(FrameData, vsm_window) == 1136 && offsetof(FrameData, vsm_info) == 1184);
    const std::string hlsl = FrameDataHlsl();
    AETHER_CHECK(hlsl.find("ByteAddressBuffer g_FrameLights : register(t0, space3)") != std::string::npos);
    AETHER_CHECK(hlsl.find("uint4 cluster_info;") != std::string::npos && hlsl.find("uint2 ClusterRange(") != std::string::npos);
    const std::string scene = SceneShaderSource(true, true);
    AETHER_CHECK(scene.find("if (g_Frame.cluster_info.w != 0)") != std::string::npos); // else the nearest 8 + 4, as before
    AETHER_CHECK(kLightClusterBufferBytes == 1024u * 48u + 3456u * 8u + 262144u * 2u);
}

AETHER_TEST(Clusters_SlicesSpanNearToFarExponentially) {
    LightClusters clusters;
    BuildLightClusters({}, TestView(), clusters);
    AETHER_CHECK(ClusterSlice(0.1f, clusters.depth_scale, clusters.depth_bias) == 0);
    AETHER_CHECK(ClusterSlice(0.01f, clusters.depth_scale, clusters.depth_bias) == 0);      // nearer than the near plane: the first
    AETHER_CHECK(ClusterSlice(5000.0f, clusters.depth_scale, clusters.depth_bias) == kClusterSlices - 1); // past the far one: the last
    // Four decades over 24 slices: six to a decade.
    AETHER_CHECK(ClusterSlice(1.05f, clusters.depth_scale, clusters.depth_bias) == 6);
    AETHER_CHECK(ClusterSlice(11.0f, clusters.depth_scale, clusters.depth_bias) == 12);
    u32 last = 0;
    for (f32 d = 0.1f; d < 1000.0f; d *= 1.1f) {
        const u32 s = ClusterSlice(d, clusters.depth_scale, clusters.depth_bias);
        AETHER_CHECK(s >= last);
        last = s;
    }
    // No lights: every cell empty, and the buffer is the cell table alone.
    AETHER_CHECK(clusters.light_count == 0 && clusters.index_count == 0 && clusters.cells_offset == 0);
    AETHER_CHECK(clusters.bytes.size() == kClusterCells * 8);
}

AETHER_TEST(Clusters_APointLightLandsInTheCellsAroundIt) {
    const ClusterView view = TestView();
    // Straight ahead at 10 m, 1 m of reach: the middle of the screen (half the visible height at 10 m is 10 m, so it spans a tenth).
    const ClusterLight light = MakePointClusterLight(Vec3(0, 0, -10), 1.0f, Vec3(1, 1, 1));
    LightClusters clusters;
    BuildLightClusters(std::span<const ClusterLight>(&light, 1), view, clusters);
    AETHER_CHECK(clusters.light_count == 1 && clusters.dropped_lights == 0);
    const u32 slice = ClusterSlice(10.0f, clusters.depth_scale, clusters.depth_bias);
    for (u32 x : {7u, 8u}) AETHER_CHECK(LightsIn(clusters, Cell(x, 4, slice)).size() == 1);
    AETHER_CHECK(LightsIn(clusters, Cell(0, 0, slice)).empty() && LightsIn(clusters, Cell(15, 8, slice)).empty());
    AETHER_CHECK(LightsIn(clusters, Cell(8, 4, ClusterSlice(30.0f, clusters.depth_scale, clusters.depth_bias))).empty()); // well behind it
    AETHER_CHECK(LightsIn(clusters, Cell(8, 4, ClusterSlice(2.0f, clusters.depth_scale, clusters.depth_bias))).empty());  // well before it
    // Its cells: 2 or so across, 1 down, 2 or 3 slices deep (9 m to 11 m) - far fewer than the grid's 3456.
    AETHER_CHECK(clusters.index_count >= 2 && clusters.index_count <= 12);
    AETHER_CHECK(clusters.bytes.size() == clusters.indices_offset + (clusters.index_count * 2u + 3u) / 4u * 4u);
    AETHER_CHECK(clusters.cells_offset == sizeof(ClusterLight) && clusters.indices_offset == clusters.cells_offset + kClusterCells * 8);
    ClusterLight stored;
    std::memcpy(&stored, clusters.bytes.data(), sizeof(stored));
    AETHER_CHECK(stored.position_range[2] == -10.0f && stored.position_range[3] == 1.0f && stored.direction_outer[3] < -1.0f);

    // Up and to the left lands up and to the left: pixel rows run down the screen.
    const ClusterLight upper_left = MakePointClusterLight(Vec3(-12, 6, -10), 1.0f, Vec3(1, 1, 1));
    BuildLightClusters(std::span<const ClusterLight>(&upper_left, 1), view, clusters);
    AETHER_CHECK(clusters.light_count == 1);
    bool upper_left_found = false, elsewhere = false;
    for (u32 y = 0; y < kClusterTilesY; ++y)
        for (u32 x = 0; x < kClusterTilesX; ++x) {
            if (LightsIn(clusters, Cell(x, y, slice)).empty()) continue;
            if (x < 6 && y < 3) upper_left_found = true;
            else elsewhere = true;
        }
    AETHER_CHECK(upper_left_found && !elsewhere);
}

AETHER_TEST(Clusters_LightsOutOfViewAreLeftOutAndOnesRoundTheCameraFillTheScreen) {
    const ClusterView view = TestView();
    const std::vector<ClusterLight> outside = {
        MakePointClusterLight(Vec3(0, 0, 10), 2.0f, Vec3(1, 1, 1)),     // behind the camera
        MakePointClusterLight(Vec3(0, 0, -2000), 5.0f, Vec3(1, 1, 1)),  // past the far plane
        MakePointClusterLight(Vec3(60, 0, -10), 2.0f, Vec3(1, 1, 1)),   // off to the right
        MakePointClusterLight(Vec3(0, -40, -10), 2.0f, Vec3(1, 1, 1)),  // below
        MakePointClusterLight(Vec3(0, 0, -10), 0.0f, Vec3(1, 1, 1)),    // no range
    };
    LightClusters clusters;
    BuildLightClusters(outside, view, clusters);
    AETHER_CHECK(clusters.light_count == 0 && clusters.dropped_lights == 0 && clusters.index_count == 0);
    // The camera inside its reach: every tile of the nearest slices (it straddles the near plane).
    const ClusterLight around = MakePointClusterLight(Vec3(0.2f, 0, -0.5f), 3.0f, Vec3(1, 1, 1));
    BuildLightClusters(std::span<const ClusterLight>(&around, 1), view, clusters);
    AETHER_CHECK(clusters.light_count == 1);
    for (u32 y = 0; y < kClusterTilesY; ++y)
        for (u32 x = 0; x < kClusterTilesX; ++x) AETHER_CHECK(LightsIn(clusters, Cell(x, y, 0)).size() == 1);
    AETHER_CHECK(LightsIn(clusters, Cell(8, 4, ClusterSlice(10.0f, clusters.depth_scale, clusters.depth_bias))).empty());
}

AETHER_TEST(Clusters_ANarrowSpotIsBoundByItsConeNotItsRange) {
    const ClusterView view = TestView();
    // At 20 m, pointing right, 10 m of reach, a 10 degree cone: its light stays right of where it stands.
    const ClusterLight spot = MakeSpotClusterLight(Vec3(0, 0, -20), 10.0f, Vec3(1, 1, 1), Vec3(1, 0, 0), 10.0f, 5.0f);
    AETHER_CHECK(std::fabs(spot.direction_outer[3] - std::cos(10.0f * kTestPi / 180.0f)) < 1e-5f && spot.color_inner[3] > spot.direction_outer[3]);
    LightClusters clusters;
    BuildLightClusters(std::span<const ClusterLight>(&spot, 1), view, clusters);
    const ClusterLight point = MakePointClusterLight(Vec3(0, 0, -20), 10.0f, Vec3(1, 1, 1));
    LightClusters point_clusters;
    BuildLightClusters(std::span<const ClusterLight>(&point, 1), view, point_clusters);
    AETHER_CHECK(clusters.light_count == 1 && clusters.index_count * 2 < point_clusters.index_count);
    const u32 slice = ClusterSlice(20.0f, clusters.depth_scale, clusters.depth_bias);
    AETHER_CHECK(!LightsIn(point_clusters, Cell(6, 4, slice)).empty() && LightsIn(clusters, Cell(6, 4, slice)).empty()); // left of it
    AETHER_CHECK(!LightsIn(clusters, Cell(9, 4, slice)).empty());
    // A wide cone (60 degrees) still holds its whole sector; one of 90 degrees or more is bound by the range's ball.
    const ClusterLight wide = MakeSpotClusterLight(Vec3(0, 0, -20), 10.0f, Vec3(1, 1, 1), Vec3(1, 0, 0), 60.0f, 50.0f);
    BuildLightClusters(std::span<const ClusterLight>(&wide, 1), view, clusters);
    AETHER_CHECK(!LightsIn(clusters, Cell(9, 4, slice)).empty() && clusters.index_count <= point_clusters.index_count);
}

AETHER_TEST(Clusters_TheNearestLightsStayWhenThereAreTooMany) {
    const ClusterView view = TestView();
    // Over the light limit: small lights in a column straight ahead, a metre apart; the farthest are dropped.
    std::vector<ClusterLight> many;
    for (u32 i = 0; i < kMaxClusterLights + 76; ++i) {
        many.push_back(MakePointClusterLight(Vec3(static_cast<f32>(i % 7) - 3.0f, 0, -2.0f - static_cast<f32>(i) * 0.5f), 0.2f,
                                             Vec3(1, 1, 1)));
    }
    LightClusters clusters;
    BuildLightClusters(many, view, clusters);
    AETHER_CHECK(clusters.light_count == kMaxClusterLights && clusters.dropped_lights == 76);
    f32 farthest_kept = 0.0f;
    for (u32 k = 0; k < clusters.light_count; ++k) {
        f32 z = 0.0f;
        std::memcpy(&z, clusters.bytes.data() + k * sizeof(ClusterLight) + 8, sizeof(z));
        farthest_kept = std::max(farthest_kept, -z);
    }
    AETHER_CHECK(farthest_kept <= 2.0f + static_cast<f32>(kMaxClusterLights - 1) * 0.5f + 1e-3f);
    // Over the index budget: lights round the camera reaching the far plane fill every cell, so only so many fit.
    std::vector<ClusterLight> huge;
    for (u32 i = 0; i < 90; ++i) huge.push_back(MakePointClusterLight(Vec3(static_cast<f32>(i) * 0.01f, 0, 0), 2000.0f, Vec3(1, 1, 1)));
    BuildLightClusters(huge, view, clusters);
    AETHER_CHECK(clusters.light_count == kMaxClusterIndices / kClusterCells && clusters.dropped_lights == 90 - clusters.light_count);
    AETHER_CHECK(clusters.index_count == clusters.light_count * kClusterCells);
    // Within a cell the nearest comes first.
    const std::vector<ClusterLight> pair = {MakePointClusterLight(Vec3(0.5f, 0, -12), 1.0f, Vec3(1, 1, 1)),
                                            MakePointClusterLight(Vec3(0.1f, 0, -10), 3.0f, Vec3(1, 1, 1))};
    BuildLightClusters(pair, view, clusters);
    const std::vector<u32> both = LightsIn(clusters, Cell(8, 4, ClusterSlice(11.5f, clusters.depth_scale, clusters.depth_bias)));
    AETHER_CHECK(both.size() == 2 && KeptX(clusters, both[0]) == 0.1f && KeptX(clusters, both[1]) == 0.5f);
}

AETHER_TEST(Clusters_EveryLightInTheWorldAndTheParticlesAreGathered) {
    RegisterRenderComponents();
    World world;
    GuidIndex guids;
    for (int i = 0; i < 20; ++i) {
        Transform t;
        t.position = Vec3(static_cast<f32>(i), 0, 0);
        PointLight light;
        light.range = 3.0f;
        light.intensity = 2.0f;
        light.color = Vec3(1.0f, 0.5f, 0.25f);
        world.CreateEntity(t, light);
    }
    PointLight unlit;
    unlit.range = 0.0f;
    world.CreateEntity(Transform{}, unlit);
    Transform spot_at;
    spot_at.position = Vec3(0, 5, 0);
    spot_at.rotation = Quaternion::FromAxisAngle(Vec3(1, 0, 0), -1.5707963f); // pitched down: shines along -Y
    SpotLight spot;
    spot.range = 8.0f;
    spot.outer_angle_degrees = 30.0f;
    spot.inner_angle_degrees = 20.0f;
    world.CreateEntity(spot_at, spot);
    vfx::ParticleLight spark;
    spark.position = Vec3(1, 1, 1);
    spark.radius = 0.5f;
    spark.intensity = 4.0f;
    vfx::ParticleLight dead = spark;
    dead.intensity = 0.0f;
    const std::vector<vfx::ParticleLight> particles = {spark, dead};
    const std::vector<ClusterLight> lights = GatherClusterLights(world, guids, particles);
    AETHER_CHECK(lights.size() == 22); // 20 point lights (more than the frame constants' 8), the spot and the live spark
    if (lights.size() != 22) return;
    AETHER_CHECK(std::fabs(lights[0].color_inner[0] - 2.0f) < 1e-5f && std::fabs(lights[0].color_inner[2] - 0.5f) < 1e-5f);
    const ClusterLight& s = lights[20];
    AETHER_CHECK(std::fabs(s.direction_outer[1] + 1.0f) < 1e-4f && std::fabs(s.position_range[1] - 5.0f) < 1e-5f);
    AETHER_CHECK(std::fabs(s.direction_outer[3] - std::cos(30.0f * kTestPi / 180.0f)) < 1e-5f);
    AETHER_CHECK(lights[21].position_range[3] == 0.5f && std::fabs(lights[21].color_inner[0] - 4.0f) < 1e-5f);

    FrameData data;
    LightClusters clusters;
    BuildLightClusters(lights, TestView(), clusters);
    FillClusterData(data, clusters, TestView(), 1600, 900);
    AETHER_CHECK(data.cluster_info[3] == 1 && data.cluster_info[0] == clusters.cells_offset && data.cluster_info[1] == clusters.indices_offset);
    AETHER_CHECK(std::fabs(data.cluster_forward[2] + 1.0f) < 1e-6f && std::fabs(data.cluster_scale[0] - 0.01f) < 1e-7f &&
                 std::fabs(data.cluster_scale[1] - 0.01f) < 1e-7f);
    AETHER_CHECK(FrameData{}.cluster_info[3] == 0); // off unless filled: the nearest lights above, as before
}

// On both backends: the frame read buffer is bound where the shader looks for it (Vulkan once had no set 3 and lost the device).
void TheShaderFindsTheCellsTheCpuFilled(rhi::Backend backend) {
    std::unique_ptr<rhi::IDevice> device = MakeDevice(backend);
    if (!device || !device->SupportsFrameConstants()) return;
    // Three lights at 9 m: middle, upper left, lower right (a spot). Each pixel writes which of them its cell lists.
    const ClusterView view = TestView();
    const std::vector<ClusterLight> lights = {
        MakePointClusterLight(Vec3(0, 0, -9), 1.5f, Vec3(1, 1, 1)),
        MakePointClusterLight(Vec3(-11, 4, -9.5f), 2.0f, Vec3(1, 1, 1)),
        MakeSpotClusterLight(Vec3(8, -3, -8), 4.0f, Vec3(1, 1, 1), Vec3(0, 0, -1), 20.0f, 10.0f),
    };
    LightClusters clusters;
    BuildLightClusters(lights, view, clusters);
    AETHER_CHECK(clusters.light_count == 3);
    constexpr u32 kWidth = 160, kHeight = 90;
    FrameData frame;
    FillClusterData(frame, clusters, view, kWidth, kHeight);

    std::unique_ptr<rhi::ISwapChain> target = device->CreateSwapChain(nullptr, kWidth, kHeight, 2);
    rhi::PipelineDesc desc;
    desc.hlsl_source = std::string(R"(
struct PushConstants { float depth; uint pad0; uint pad1; uint pad2; };
#ifdef __spirv__
[[vk::push_constant]]
#endif
ConstantBuffer<PushConstants> g_PC : register(b0);
Texture2D g_Textures[128] : register(t0, space0);
SamplerState g_Sampler : register(s0, space1);
)") + FrameDataHlsl() + R"(
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float4 PSMain(float4 position : SV_Position) : SV_Target {
    uint2 range = ClusterRange(position.xy, g_PC.depth);
    float3 bits = float3(0, 0, 0);
    for (uint i = 0; i < range.y; ++i) {
        uint light = ClusterLightIndex(range.x + i);
        if (light == 0) bits.x = 1.0;
        if (light == 1) bits.y = 1.0;
        if (light == 2) bits.z = 1.0;
    }
    return float4(bits, 1.0);
}
)";
    desc.push_constant_size_bytes = 16;
    desc.frame_constants_size_bytes = sizeof(FrameData);
    desc.frame_read_buffer = true;
    desc.enable_bindless_textures = true; // FrameDataHlsl's sky helpers name the texture table (unused here)
    const rhi::PipelineHandle pipeline = device->CreatePipeline(desc, *target);
    AETHER_CHECK(pipeline.IsValid());
    if (!pipeline.IsValid()) return;
    const rhi::BufferHandle frame_buffer = device->CreateFrameConstantBuffer(sizeof(FrameData));
    const rhi::BufferHandle light_buffer = device->CreateFrameConstantBuffer(kLightClusterBufferBytes);
    device->UpdateFrameConstantBuffer(frame_buffer, &frame, sizeof(frame));
    device->UpdateFrameConstantBuffer(light_buffer, clusters.bytes.data(), clusters.bytes.size());

    for (const f32 depth : {9.0f, 40.0f}) {
        std::unique_ptr<rhi::ICommandList> cmd = device->CreateCommandList();
        target->AcquireNextImage();
        cmd->Reset();
        cmd->BeginRenderPass(*target, {0, 0, 0, 1});
        cmd->BindPipeline(pipeline);
        cmd->BindFrameConstants(frame_buffer);
        cmd->BindFrameReadBuffer(light_buffer);
        cmd->BindBindlessTextures();
        const f32 push[4] = {depth, 0, 0, 0};
        cmd->SetPushConstants(push, sizeof(push));
        cmd->Draw(3);
        cmd->EndRenderPass();
        cmd->Close();
        device->WaitForFence(device->Submit(*cmd, target.get()));
        std::vector<u8> pixels;
        const bool read = target->ReadBack(pixels) && pixels.size() == kWidth * kHeight * 4;
        target->Present(false);
        AETHER_CHECK(read);
        if (!read) break;
        // Every tile's centre against the CPU's list for that cell.
        const u32 slice = ClusterSlice(depth, clusters.depth_scale, clusters.depth_bias);
        u32 mismatches = 0, lit_tiles = 0;
        for (u32 y = 0; y < kClusterTilesY; ++y)
            for (u32 x = 0; x < kClusterTilesX; ++x) {
                bool expected[3] = {false, false, false};
                for (const u32 k : LightsIn(clusters, Cell(x, y, slice))) expected[k] = true;
                const u8* p = &pixels[(static_cast<usize>(y * 10 + 5) * kWidth + x * 10 + 5) * 4];
                for (int c = 0; c < 3; ++c) {
                    if ((p[c] > 128) != expected[c]) ++mismatches;
                    if (expected[c]) ++lit_tiles;
                }
            }
        if (mismatches != 0) std::printf("    depth %.0f: %u tile mismatches\n", depth, mismatches);
        AETHER_CHECK(mismatches == 0);
        AETHER_CHECK(depth == 9.0f ? lit_tiles >= 3 : lit_tiles == 0); // all three at 9 m; nothing reaches 40 m
    }
    device->DestroyPipeline(pipeline);
    device->DestroyBuffer(frame_buffer);
    device->DestroyBuffer(light_buffer);
}
AETHER_TEST(Clusters_TheShaderFindsTheCellsTheCpuFilled_D3D12) { TheShaderFindsTheCellsTheCpuFilled(rhi::Backend::D3D12); }
AETHER_TEST(Clusters_TheShaderFindsTheCellsTheCpuFilled_Vulkan) { TheShaderFindsTheCellsTheCpuFilled(rhi::Backend::Vulkan); }
