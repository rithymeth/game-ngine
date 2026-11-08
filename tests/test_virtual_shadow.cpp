#include "../player/src/virtual_shadow.h"
#include "aether/assets/environment.h"
#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"
#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>

using namespace aether;
using namespace aether::player;
namespace rhi = aether::gfx::rhi;

// Virtual shadow maps (player/src/virtual_shadow.h): the shared page geometry, and the feedback pass that finds the pages a frame looks at.

namespace {

bool Near(f32 a, f32 b, f32 e = 1e-4f) { return std::fabs(a - b) <= e; }

} // namespace

AETHER_TEST(Vsm_LevelsHaveTexelsThatDoubleAndPagesOfFixedSize) {
    AETHER_CHECK(Near(VsmTexelSize(0), kVsmBaseTexel) && Near(VsmTexelSize(3), kVsmBaseTexel * 8.0f));
    for (u32 level = 0; level < kVsmLevels; ++level) AETHER_CHECK(Near(VsmPageSize(level), VsmTexelSize(level) * 128.0f, 1e-5f));
    // The level of a footprint: the smallest texel not smaller than it, clamped to the levels there are.
    AETHER_CHECK(VsmLevelForFootprint(0.0f) == 0 && VsmLevelForFootprint(0.0005f) == 0 && VsmLevelForFootprint(kVsmBaseTexel) == 0);
    AETHER_CHECK(VsmLevelForFootprint(kVsmBaseTexel * 1.01f) == 1 && VsmLevelForFootprint(kVsmBaseTexel * 2.0f) == 1);
    AETHER_CHECK(VsmLevelForFootprint(kVsmBaseTexel * 8.0f) == 3 && VsmLevelForFootprint(kVsmBaseTexel * 8.1f) == 4);
    AETHER_CHECK(VsmLevelForFootprint(1000.0f) == kVsmLevels - 1);
    u32 previous = 0;
    for (f32 footprint = 0.0001f; footprint < 1.0f; footprint *= 1.3f) {
        const u32 level = VsmLevelForFootprint(footprint);
        AETHER_CHECK(level >= previous); // coarser surfaces never get a finer level
        AETHER_CHECK(VsmTexelSize(level) >= footprint || level == kVsmLevels - 1);
        previous = level;
    }
}

AETHER_TEST(Vsm_TheLightBasisIsOrthonormalAndStable) {
    for (const Vec3& direction : {Vec3(0.3f, -1.0f, 0.2f), Vec3(0, -1, 0), Vec3(0, 1, 0), Vec3(1, 0, 0), Vec3(-0.5f, -0.2f, 0.7f)}) {
        const VsmLight light = VsmLightBasis(direction);
        AETHER_CHECK(Near(light.right.Length(), 1.0f) && Near(light.up.Length(), 1.0f) && Near(light.direction.Length(), 1.0f));
        AETHER_CHECK(Near(light.right.Dot(light.up), 0.0f) && Near(light.right.Dot(light.direction), 0.0f) && Near(light.up.Dot(light.direction), 0.0f));
        const VsmLight again = VsmLightBasis(direction * 3.0f); // only the direction matters
        AETHER_CHECK(Near(again.right.x, light.right.x) && Near(again.up.y, light.up.y));
    }
    // A point moved along the light keeps its (u, v): the pages do not depend on how high a caster is.
    const VsmLight light = VsmLightBasis(Vec3(0.3f, -1.0f, 0.2f));
    f32 u0 = 0, v0 = 0, d0 = 0, u1 = 0, v1 = 0, d1 = 0;
    VsmLightSpace(light, Vec3(3, 1, -2), u0, v0, d0);
    VsmLightSpace(light, Vec3(3, 1, -2) + light.direction * 10.0f, u1, v1, d1);
    AETHER_CHECK(Near(u0, u1, 1e-4f) && Near(v0, v1, 1e-4f) && Near(d1 - d0, 10.0f, 1e-3f));
}

AETHER_TEST(Vsm_PagesAreAnchoredInLightSpaceAndTheWindowWrapsBack) {
    const VsmLight light = VsmLightBasis(Vec3(0.3f, -1.0f, 0.2f));
    const f32 size = VsmPageSize(2);
    // A point and the same point a page along u are neighbours; negative coordinates floor.
    const Vec3 p = light.right * (size * 5.5f) + light.up * (size * -2.5f);
    const VsmPage page = VsmPageOf(light, 2, p);
    AETHER_CHECK(page.level == 2 && page.x == 5 && page.y == -3);
    AETHER_CHECK(VsmPageOf(light, 2, p + light.right * size).x == 6);
    // The window around page 40 holds 24 .. 55; wrapped coordinates come back as the page, for every page in it, negative and positive.
    for (const i32 camera : {40, 0, -7, -1000, 123456}) {
        const i32 origin = VsmWindowOrigin(camera);
        AETHER_CHECK(origin == camera - 16);
        for (i32 page_x = origin; page_x < origin + static_cast<i32>(kVsmWindowPages); ++page_x) {
            AETHER_CHECK(VsmRecoverPage(origin, static_cast<u32>(page_x) & 31u) == page_x);
        }
    }
    // The window origins follow the camera and move one page when it crosses one.
    const auto before = VsmWindowOrigins(light, light.right * (VsmPageSize(0) * 10.5f));
    const auto after = VsmWindowOrigins(light, light.right * (VsmPageSize(0) * 11.5f));
    AETHER_CHECK(after[0][0] == before[0][0] + 1);
}

AETHER_TEST(Vsm_FeedbackImagesDecodeToRequestsMostLookedAtFirst) {
    // A 4 x 2 image: three texels look at (level 1, page 40, -2), one at (level 0, 5, 5), the rest nothing. The window origins make 40 & 31 = 8
    // and -2 & 31 = 30 mean those pages.
    std::array<std::array<i32, 2>, kVsmLevels> origins{};
    for (auto& origin : origins) origin = {24, -18};
    const u32 pitch = rhi::ReadbackRowPitch(4, 8);
    std::vector<u8> rows(static_cast<usize>(pitch) * 2, 0);
    const auto put = [&](u32 x, u32 y, f32 level_marker, u32 wrapped_x, u32 wrapped_y) {
        const u16 texel[4] = {assets::FloatToHalf(level_marker), assets::FloatToHalf(static_cast<f32>(wrapped_x)),
                              assets::FloatToHalf(static_cast<f32>(wrapped_y)), assets::FloatToHalf(1.0f)};
        std::memcpy(&rows[static_cast<usize>(y) * pitch + static_cast<usize>(x) * 8], texel, sizeof(texel));
    };
    put(0, 0, 2.0f, 40u & 31u, static_cast<u32>(-2) & 31u);
    put(1, 0, 2.0f, 40u & 31u, static_cast<u32>(-2) & 31u);
    put(3, 1, 2.0f, 40u & 31u, static_cast<u32>(-2) & 31u);
    put(2, 1, 1.0f, 5u, 5u);
    const std::vector<VsmRequest> requests = VsmDecodeFeedback(rows.data(), 4, 2, pitch, origins);
    AETHER_CHECK(requests.size() == 2);
    if (requests.size() == 2) {
        const VsmPage busy{1, 40, -2};
        AETHER_CHECK(requests[0].page == busy && requests[0].hits == 3);
        AETHER_CHECK(requests[1].hits == 1 && requests[1].page.level == 0);
    }
    AETHER_CHECK(VsmDecodeFeedback(std::vector<u8>(pitch * 2, 0).data(), 4, 2, pitch, origins).empty()); // all zero: nothing drawn
}

// A ground plane seen by a camera, its depth copied, and the pages the GPU asks for compared with the same rays worked out on the CPU.
static void Vsm_TheFeedbackPassFindsTheGroundPagesTheCameraSees(rhi::Backend backend) {
    if (!rhi::IsBackendAvailable(backend)) return;
    std::unique_ptr<rhi::IDevice> device;
    try {
        device = rhi::CreateDevice(backend, false);
    } catch (const std::exception&) {
    }
    if (!device || !device->SupportsReadback()) return;
    constexpr u32 kWidth = 320, kHeight = 180;
    std::unique_ptr<rhi::ISwapChain> target = device->CreateSwapChain(nullptr, kWidth, kHeight, 2);
    const rhi::SampledTextureHandle depth_texture = (target->SceneColorTexture(), target->SceneDepthTexture());
    AETHER_CHECK(depth_texture.IsValid());
    if (!depth_texture.IsValid()) return;
    VsmFeedback feedback(*device, *target);
    AETHER_CHECK(feedback.Valid());
    if (!feedback.Valid()) return;

    // The ground: y = 0, 600 m on a side, from procedural vertices; its depth is written.
    rhi::PipelineDesc ground;
    ground.hlsl_source = R"(
struct PushConstants { float4x4 view_projection; };
#ifdef __spirv__
[[vk::push_constant]]
#endif
ConstantBuffer<PushConstants> g_PC : register(b0);
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    const float2 corners[6] = {float2(-300, -300), float2(300, -300), float2(300, 300), float2(-300, -300), float2(300, 300), float2(-300, 300)};
    return mul(g_PC.view_projection, float4(corners[id].x, 0.0, corners[id].y, 1.0));
}
float4 PSMain(float4 position : SV_Position) : SV_Target { return float4(1, 1, 1, 1); }
)";
    ground.push_constant_size_bytes = 64;
    ground.scene_color_target = true;
    ground.depth_test = true;
    ground.depth_write = true;
    ground.cull_back_face = false;
    const rhi::PipelineHandle ground_pipeline = device->CreatePipeline(ground, *target);
    AETHER_CHECK(ground_pipeline.IsValid());
    if (!ground_pipeline.IsValid()) return;

    const Vec3 eye(0.0f, 1.6f, 0.0f);
    const f32 fov = 60.0f * 3.14159265f / 180.0f, aspect = static_cast<f32>(kWidth) / kHeight;
    const Mat4 view = Mat4::LookAtRH(eye, Vec3(0.0f, 1.6f - 0.4f, -1.0f), Vec3(0, 1, 0));
    const Mat4 view_projection = Mat4::PerspectiveRH(fov, aspect, 0.1f, 400.0f) * view;
    Mat4 inverse;
    AETHER_CHECK(view_projection.TryInverse(inverse));

    VsmFeedbackParams params;
    params.inverse_view_projection = inverse;
    params.camera_position = eye;
    params.footprint_scale = 2.0f * std::tan(fov * 0.5f) / static_cast<f32>(kHeight);
    params.footprint_constant = 0.0f;
    params.light = VsmLightBasis(Vec3(0.3f, -1.0f, 0.2f));
    params.depth_texture = depth_texture.index;
    params.screen_width = kWidth;
    params.screen_height = kHeight;
    params.frame = 0; // the block's top-left pixel

    std::unique_ptr<rhi::ICommandList> cmd = device->CreateCommandList();
    target->AcquireNextImage();
    cmd->Reset();
    AETHER_CHECK(cmd->BeginSceneColorPass(*target, {0, 0, 0, 1}));
    cmd->BindPipeline(ground_pipeline);
    cmd->SetPushConstants(&view_projection.cols[0], 64);
    cmd->Draw(6);
    AETHER_CHECK(cmd->CopySceneDepth());
    cmd->EndRenderPass();
    AETHER_CHECK(feedback.Record(*cmd, params, 3));
    cmd->Close();
    device->WaitForFence(device->Submit(*cmd, target.get()));
    target->Present(false);
    const std::vector<VsmRequest> requests = feedback.Collect(3);
    AETHER_CHECK(feedback.Collect(3).empty()); // read once
    AETHER_CHECK(!requests.empty());

    // The same rays on the CPU: each feedback texel's pixel through the ground plane, its level and page. A pixel at a level boundary can round
    // either way on the GPU, so the GPU's pages must lie within a page and a level of the expected ones, and nearly all expected ones be found.
    std::set<std::tuple<u32, i32, i32>> expected, allowed;
    u32 ground_texels = 0;
    for (u32 fy = 0; fy < (kHeight + 7) / 8; ++fy) {
        for (u32 fx = 0; fx < (kWidth + 7) / 8; ++fx) {
            const u32 px = fx * 8, py = fy * 8;
            if (px >= kWidth || py >= kHeight) continue;
            const f32 ndc_x = (static_cast<f32>(px) + 0.5f) / kWidth * 2.0f - 1.0f, ndc_y = 1.0f - (static_cast<f32>(py) + 0.5f) / kHeight * 2.0f;
            const Vec4 near_point = inverse * Vec4(ndc_x, ndc_y, 0.0f, 1.0f), far_point = inverse * Vec4(ndc_x, ndc_y, 1.0f, 1.0f);
            const Vec3 a(near_point.x / near_point.w, near_point.y / near_point.w, near_point.z / near_point.w);
            const Vec3 b(far_point.x / far_point.w, far_point.y / far_point.w, far_point.z / far_point.w);
            if (!(a.y > 0.0f && b.y < 0.0f)) continue; // the ray never reaches y = 0 inside the depth range: sky or beyond the far plane
            const f32 t = a.y / (a.y - b.y);
            const Vec3 p = a + (b - a) * t;
            if (std::fabs(p.x) > 299.0f || std::fabs(p.z) > 299.0f) continue;
            ++ground_texels;
            const f32 footprint = params.footprint_scale * (p - eye).Length();
            const VsmPage page = VsmPageOf(params.light, VsmLevelForFootprint(footprint), p);
            expected.insert({page.level, page.x, page.y});
            for (const f32 scale : {0.95f, 1.0f, 1.05f}) {
                const u32 level = VsmLevelForFootprint(footprint * scale);
                const VsmPage neighbour = VsmPageOf(params.light, level, p);
                for (i32 dx = -1; dx <= 1; ++dx) {
                    for (i32 dy = -1; dy <= 1; ++dy) allowed.insert({level, neighbour.x + dx, neighbour.y + dy});
                }
            }
        }
    }
    u32 total_hits = 0, outside = 0, found = 0;
    std::set<u32> levels;
    std::set<std::tuple<u32, i32, i32>> seen;
    for (const VsmRequest& request : requests) {
        total_hits += request.hits;
        levels.insert(request.page.level);
        const auto key = std::make_tuple(request.page.level, request.page.x, request.page.y);
        seen.insert(key);
        if (!allowed.count(key)) ++outside;
    }
    for (const auto& key : expected) found += seen.count(key) ? 1u : 0u;
    std::printf("    %zu pages asked for (%zu expected, %u found), %u ground texels, %zu levels\n", requests.size(), expected.size(), found,
                ground_texels, levels.size());
    AETHER_CHECK(ground_texels > 200);
    AETHER_CHECK(total_hits + 3 >= ground_texels && total_hits <= ground_texels + 3); // every ground texel asked for exactly one page
    AETHER_CHECK(outside == 0);
    AETHER_CHECK(found * 10 >= static_cast<u32>(expected.size()) * 9);
    AETHER_CHECK(levels.size() >= 3); // a ground from the camera's feet to the horizon spans levels
    AETHER_CHECK(std::is_sorted(requests.begin(), requests.end(), [](const VsmRequest& l, const VsmRequest& r) { return l.hits > r.hits; }));
    device->DestroyPipeline(ground_pipeline);
}

AETHER_TEST(Vsm_TheFeedbackPassFindsTheGroundPagesTheCameraSees_D3D12) { Vsm_TheFeedbackPassFindsTheGroundPagesTheCameraSees(rhi::Backend::D3D12); }
AETHER_TEST(Vsm_TheFeedbackPassFindsTheGroundPagesTheCameraSees_Vulkan) { Vsm_TheFeedbackPassFindsTheGroundPagesTheCameraSees(rhi::Backend::Vulkan); }
