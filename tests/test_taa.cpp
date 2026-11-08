#include "../player/src/post_pass.h"
#include "../player/src/taa_pass.h"
#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"
#include "test_framework.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace aether;
using namespace aether::player;
namespace rhi = aether::gfx::rhi;

// Render roadmap step 6: temporal anti-aliasing. Sub-pixel jitter on the projection (player/src/taa_pass.h), the resolve that blends
// each frame into a reprojected, clipped history, and the post pass reading the result.

namespace {

constexpr f32 kTaaTestPi = 3.14159265358979f;

Vec3 Project(const Mat4& m, const Vec3& p) {
    const Vec4 clip = m * Vec4(p.x, p.y, p.z, 1.0f);
    return Vec3(clip.x / clip.w, clip.y / clip.w, clip.z / clip.w);
}

bool Close(f32 a, f32 b, f32 e = 1e-4f) { return std::fabs(a - b) <= e; }

} // namespace

AETHER_TEST(Taa_JitterWalksTheHaltonSequenceInsideThePixel) {
    f32 sum_x = 0.0f, sum_y = 0.0f;
    std::vector<std::pair<f32, f32>> seen;
    for (u32 frame = 0; frame < kTaaSequenceLength; ++frame) {
        const Vec3 jitter = TaaJitter(frame, kTaaSequenceLength);
        const f32 x = jitter.x, y = jitter.y;
        AETHER_CHECK(x > -0.5f && x < 0.5f && y > -0.5f && y < 0.5f);
        for (const auto& [px, py] : seen) AETHER_CHECK(!(px == x && py == y)); // eight different points
        seen.push_back({x, y});
        sum_x += x, sum_y += y;
    }
    AETHER_CHECK(std::fabs(sum_x / kTaaSequenceLength) < 0.1f && std::fabs(sum_y / kTaaSequenceLength) < 0.1f); // centred on the pixel
    const Vec3 first = TaaJitter(0, kTaaSequenceLength), again = TaaJitter(kTaaSequenceLength, kTaaSequenceLength);
    const f32 x0 = first.x, y0 = first.y, x8 = again.x, y8 = again.y;
    AETHER_CHECK(x0 == x8 && y0 == y8); // and round again
    AETHER_CHECK(Close(x0, 0.0f) && Close(y0, 1.0f / 3.0f - 0.5f)); // Halton(1): 1/2 and 1/3
}

AETHER_TEST(Taa_JitterMovesEveryDepthByTheSameFractionOfAPixel) {
    const Mat4 perspective = Mat4::PerspectiveRH(kTaaTestPi / 3.0f, 16.0f / 9.0f, 0.1f, 100.0f);
    const Mat4 jittered = JitterProjection(perspective, 0.25f, -0.4f, 1600, 900);
    for (const Vec3& p : {Vec3(0.3f, -0.2f, -1.0f), Vec3(-4.0f, 2.0f, -50.0f)}) {
        const Vec3 a = Project(perspective, p), b = Project(jittered, p);
        AETHER_CHECK(Close(b.x - a.x, 2.0f * 0.25f / 1600.0f, 1e-6f) && Close(b.y - a.y, 2.0f * -0.4f / 900.0f, 1e-6f));
        AETHER_CHECK(Close(b.z, a.z, 1e-6f)); // depth untouched
    }
    Mat4 orthographic = Mat4::Identity();
    orthographic.cols[0].x = 0.1f, orthographic.cols[1].y = 0.2f, orthographic.cols[2].z = -0.01f;
    const Vec3 a = Project(orthographic, Vec3(3, 1, -5)), b = Project(JitterProjection(orthographic, 0.5f, 0.5f, 100, 50), Vec3(3, 1, -5));
    AETHER_CHECK(Close(b.x - a.x, 0.01f, 1e-6f) && Close(b.y - a.y, 0.02f, 1e-6f));
}

AETHER_TEST(Taa_ReprojectionFindsWhereAPointWasLastFrame) {
    const Mat4 projection = Mat4::PerspectiveRH(kTaaTestPi / 2.0f, 1.0f, 0.1f, 100.0f);
    const Mat4 now = projection * Mat4::LookAtRH(Vec3(0, 0, 0), Vec3(0, 0, -1), Vec3(0, 1, 0));
    Mat4 same;
    AETHER_CHECK(TaaReprojection(now, now, same));
    const Vec3 still = Project(same, Vec3(0.3f, -0.6f, 0.9f));
    AETHER_CHECK(Close(still.x, 0.3f) && Close(still.y, -0.6f)); // a camera that hasn't moved: the same place
    // Last frame the camera stood a metre to the left: a point now in the middle of the screen was right of it.
    const Mat4 before = projection * Mat4::LookAtRH(Vec3(-1, 0, 0), Vec3(-1, 0, -1), Vec3(0, 1, 0));
    Mat4 reprojection;
    AETHER_CHECK(TaaReprojection(before, now, reprojection));
    const Vec3 world(0, 0, -4);
    const Vec3 ndc_now = Project(now, world), ndc_before = Project(before, world);
    const Vec3 found = Project(reprojection, ndc_now);
    AETHER_CHECK(Close(found.x, ndc_before.x, 1e-4f) && Close(found.y, ndc_before.y, 1e-4f) && ndc_before.x > 0.2f);
    Mat4 singular;
    std::memset(&singular, 0, sizeof(singular));
    AETHER_CHECK(!TaaReprojection(now, singular, reprojection));
}

AETHER_TEST(Taa_ShaderClipsTheHistoryAndWeighsByLuma) {
    const std::string source = TaaShaderSource();
    AETHER_CHECK(source.find("float3 HistoryCatmullRom(float2 uv)") != std::string::npos);
    AETHER_CHECK(source.find("ClipToBox(ToYCoCg(history), mean, 1.25 * sigma)") != std::string::npos);
    AETHER_CHECK(source.find("depth = min(depth, Depth(p + int2(x, y)))") != std::string::npos); // the nearest depth of the neighbourhood
    AETHER_CHECK(source.find("g_PC.g_Blend / (1.0 + Luma(current))") != std::string::npos);
    AETHER_CHECK(kTaaBlend > 0.0f && kTaaBlend < 0.5f && sizeof(TaaConstants) <= 256);
}

AETHER_TEST(Taa_AHardEdgeConvergesToItsCoverageAndACutStartsOver) {
    if (!rhi::IsBackendAvailable(rhi::Backend::D3D12)) return;
    std::unique_ptr<rhi::IDevice> device;
    try {
        device = rhi::CreateDevice(rhi::Backend::D3D12, false);
    } catch (const std::exception&) {
    }
    if (!device) return;
    constexpr u32 kSize = 32;
    std::unique_ptr<rhi::ISwapChain> target = device->CreateSwapChain(nullptr, kSize, kSize, 2);
    TaaPass taa(*device, *target);
    AETHER_CHECK(taa.Valid());
    if (!taa.Valid()) return;

    // The "scene": white left of x = 10.3 pixels, black right of it, sampled where this frame's jitter puts the pixel's centre.
    rhi::PipelineDesc scene;
    scene.hlsl_source = R"(
struct PushConstants { float jitter_x; float edge; float pad0; float pad1; };
ConstantBuffer<PushConstants> g_PC : register(b0);
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.5, 1.0);
}
float4 PSMain(float4 position : SV_Position) : SV_Target {
    return position.x - g_PC.jitter_x < g_PC.edge ? float4(1, 1, 1, 1) : float4(0, 0, 0, 1);
}
)";
    scene.push_constant_size_bytes = 16;
    scene.scene_color_target = true;
    const rhi::PipelineHandle scene_pipeline = device->CreatePipeline(scene, *target);
    // The resolved image, copied into the back buffer to be read.
    rhi::PipelineDesc show;
    show.hlsl_source = R"(
struct PushConstants { uint index; uint pad0; uint pad1; uint pad2; };
ConstantBuffer<PushConstants> g_PC : register(b0);
Texture2D g_Textures[128] : register(t0, space0);
SamplerState g_Sampler : register(s0, space1);
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float4 PSMain(float4 position : SV_Position) : SV_Target {
    return float4(saturate(g_Textures[g_PC.index].Load(int3(int2(position.xy), 0)).rgb), 1.0);
}
)";
    show.push_constant_size_bytes = 16;
    show.enable_bindless_textures = true;
    const rhi::PipelineHandle show_pipeline = device->CreatePipeline(show, *target);
    AETHER_CHECK(scene_pipeline.IsValid() && show_pipeline.IsValid());
    if (!scene_pipeline.IsValid() || !show_pipeline.IsValid()) return;

    const Mat4 view_projection = Mat4::Identity();
    // One frame: the scene with this frame's jitter, the resolve, and the result read back (the red channel of row 16).
    const auto frame = [&](u64 camera, std::vector<u8>& row) {
        f32 jx = 0.0f, jy = 0.0f;
        taa.NextJitter(jx, jy);
        std::unique_ptr<rhi::ICommandList> cmd = device->CreateCommandList();
        target->AcquireNextImage();
        cmd->Reset();
        if (!cmd->BeginSceneColorPass(*target, {0, 0, 0, 1})) return false;
        cmd->BindPipeline(scene_pipeline);
        const f32 push[4] = {jx, 10.3f, 0, 0};
        cmd->SetPushConstants(push, sizeof(push));
        cmd->Draw(3);
        if (!cmd->CopySceneDepth()) return false;
        cmd->EndRenderPass();
        const u32 resolved = taa.Resolve(*cmd, view_projection, camera);
        if (resolved == 0) return false;
        cmd->BeginRenderPass(*target, {1, 0, 1, 1});
        cmd->BindPipeline(show_pipeline);
        cmd->BindBindlessTextures();
        const u32 index[4] = {resolved, 0, 0, 0};
        cmd->SetPushConstants(index, sizeof(index));
        cmd->Draw(3);
        cmd->EndRenderPass();
        cmd->Close();
        device->WaitForFence(device->Submit(*cmd, target.get()));
        std::vector<u8> pixels;
        const bool read = target->ReadBack(pixels) && pixels.size() == kSize * kSize * 4;
        target->Present(false);
        if (!read) return false;
        row.resize(kSize);
        for (u32 x = 0; x < kSize; ++x) row[x] = pixels[(16 * kSize + x) * 4];
        return true;
    };

    std::vector<u8> row;
    AETHER_CHECK(frame(1, row));
    // The first frame has no history: every pixel is one sample, all white or all black.
    for (const u8 v : row) AETHER_CHECK(v < 3 || v > 252);
    for (int i = 0; i < 24; ++i) AETHER_CHECK(frame(1, row));
    // Converged: the pixel the edge crosses (x 10 .. 11, 30% white) is between, its neighbours away from the edge are not.
    if (row.size() == kSize) {
        std::printf("    edge row: %u %u [%u] %u %u\n", row[8], row[9], row[10], row[11], row[12]);
        AETHER_CHECK(row[10] > 20 && row[10] < 235);
        AETHER_CHECK(row[2] > 250 && row[25] < 5);
    }
    // Another camera: a cut, the history dropped, one sample again.
    AETHER_CHECK(frame(2, row));
    if (row.size() == kSize) AETHER_CHECK(row[10] < 3 || row[10] > 252);

    device->DestroyPipeline(scene_pipeline);
    device->DestroyPipeline(show_pipeline);
}

AETHER_TEST(Taa_ThePostPassReadsTheResolveWhenGivenIt) {
    // PostPass::SetSource: Source() is the given index, or the scene colour target when 0.
    if (!rhi::IsBackendAvailable(rhi::Backend::D3D12)) return;
    std::unique_ptr<rhi::IDevice> device;
    try {
        device = rhi::CreateDevice(rhi::Backend::D3D12, false);
    } catch (const std::exception&) {
    }
    if (!device) return;
    std::unique_ptr<rhi::ISwapChain> target = device->CreateSwapChain(nullptr, 16, 16, 2);
    PostPass post(*device, *target);
    if (!post.Valid()) return;
    AETHER_CHECK(post.Source() == post.SceneColorIndex());
    post.SetSource(77);
    AETHER_CHECK(post.Source() == 77);
    post.SetSource(0);
    AETHER_CHECK(post.Source() == post.SceneColorIndex());
}
