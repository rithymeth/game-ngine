#include "aether/ecs/world.h"
#include "aether/math/quaternion.h"
#include "aether/reflection/serialize.h"
#include "aether/renderer/components.h"
#include "aether/renderer/decal.h"
#include "test_framework.h"
#include "../player/src/decal_pass.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#if defined(AETHER_HAS_VULKAN) && !defined(_WIN32)
#include "aether/gfx/rhi/device.h"
#include <memory>
#include <stdexcept>
#endif

using namespace aether;
using namespace aether::mat;

// Phase 39.4: decals. The CPU side: the projection the shader uses, the screen rectangle, the draw order, and the component.

namespace {

constexpr f32 kPiF = 3.14159265f;

Mat4 Camera() { // at (0, 2, 6) looking at the origin
    const Mat4 view = Mat4::LookAtRH(Vec3(0, 2, 6), Vec3(0, 0, 0), Vec3(0, 1, 0));
    return Mat4::PerspectiveRH(60.0f * kPiF / 180.0f, 16.0f / 9.0f, 0.1f, 100.0f) * view;
}

bool Near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }

// What the depth buffer holds for a world point, and its ndc, the way the pass's pixel shader gets them.
void Project(const Mat4& view_projection, const Vec3& world, f32& ndc_x, f32& ndc_y, f32& depth) {
    const Vec4 clip = view_projection * Vec4(world.x, world.y, world.z, 1.0f);
    ndc_x = clip.x / clip.w, ndc_y = clip.y / clip.w, depth = clip.z / clip.w;
}

} // namespace

AETHER_TEST(Decal_ClipToUnitMapsAPixelBackToTheBox) {
    const Mat4 vp = Camera();
    // A decal 2 x 1 x 4 metres, turned 30 degrees about Y and moved off the origin.
    const Mat4 world = Mat4::Translation(Vec3(1.0f, 0.5f, -1.0f)) *
                       Quaternion::FromAxisAngle(Vec3(0, 1, 0), 30.0f * kPiF / 180.0f).ToMat4();
    DecalInput input;
    input.world = world, input.size = Vec3(2, 1, 4);
    std::vector<DecalDraw> draws;
    BuildDecalDraws({input}, vp, Vec3(0, 2, 6), draws);
    AETHER_CHECK(draws.size() == 1);
    if (draws.empty()) return;

    const Mat4 local_to_world = DecalLocalToWorld(world, input.size);
    // A point at unit coordinates (u, v, w) of the box is at local_to_world * (u, v, w, 1): seen through the camera and mapped back it must
    // return the same unit coordinates, inside or outside the box.
    for (const Vec3& unit : {Vec3(0, 0, 0), Vec3(0.4f, -0.3f, 0.2f), Vec3(-0.5f, 0.5f, -0.5f), Vec3(0.75f, 0.1f, 0.3f), Vec3(-0.2f, -0.9f, 0.9f)}) {
        const Vec4 w = local_to_world * Vec4(unit.x, unit.y, unit.z, 1.0f);
        f32 x, y, depth;
        Project(vp, Vec3(w.x, w.y, w.z), x, y, depth);
        const Vec3 back = DecalUnitPosition(draws[0], x, y, depth);
        AETHER_CHECK(Near(back.x, unit.x) && Near(back.y, unit.y) && Near(back.z, unit.z));
    }
}

AETHER_TEST(Decal_ScreenRectCoversTheProjectedBox) {
    const Mat4 vp = Camera();
    const Mat4 world = Mat4::Translation(Vec3(0.5f, 0.0f, 0.0f));
    f32 rect[4];
    const Mat4 local_to_world = DecalLocalToWorld(world, Vec3(2, 2, 2));
    AETHER_CHECK(DecalScreenRect(local_to_world, vp, rect));
    AETHER_CHECK(rect[0] >= -1.0f && rect[1] >= -1.0f && rect[2] <= 1.0f && rect[3] <= 1.0f && rect[0] < rect[2] && rect[1] < rect[3]);
    // Every corner of the box lands inside the rectangle.
    for (u32 c = 0; c < 8; ++c) {
        const Vec4 w = local_to_world * Vec4((c & 1) ? 0.5f : -0.5f, (c & 2) ? 0.5f : -0.5f, (c & 4) ? 0.5f : -0.5f, 1.0f);
        f32 x, y, d;
        Project(vp, Vec3(w.x, w.y, w.z), x, y, d);
        AETHER_CHECK(x >= rect[0] - 1e-4f && x <= rect[2] + 1e-4f && y >= rect[1] - 1e-4f && y <= rect[3] + 1e-4f);
    }
    // A bigger box covers more.
    f32 big[4];
    AETHER_CHECK(DecalScreenRect(DecalLocalToWorld(world, Vec3(4, 4, 4)), vp, big));
    AETHER_CHECK(big[0] <= rect[0] && big[1] <= rect[1] && big[2] >= rect[2] && big[3] >= rect[3]);
}

AETHER_TEST(Decal_OffscreenAndBehindBoxesAreSkippedAndTheCameraMayBeInside) {
    const Mat4 vp = Camera();
    f32 rect[4];
    // Far to the right and far to the left: off screen.
    AETHER_CHECK(!DecalScreenRect(DecalLocalToWorld(Mat4::Translation(Vec3(80, 0, 0)), Vec3(1, 1, 1)), vp, rect));
    AETHER_CHECK(!DecalScreenRect(DecalLocalToWorld(Mat4::Translation(Vec3(-80, 0, 0)), Vec3(1, 1, 1)), vp, rect));
    // Behind the camera (it looks toward -Z from z = 6).
    AETHER_CHECK(!DecalScreenRect(DecalLocalToWorld(Mat4::Translation(Vec3(0, 2, 20)), Vec3(1, 1, 1)), vp, rect));
    // The camera inside a big box: some corners are behind it, so the rectangle is the whole screen.
    AETHER_CHECK(DecalScreenRect(DecalLocalToWorld(Mat4::Translation(Vec3(0, 2, 6)), Vec3(20, 20, 20)), vp, rect));
    AETHER_CHECK(rect[0] == -1.0f && rect[1] == -1.0f && rect[2] == 1.0f && rect[3] == 1.0f);

    // A decal with no size can't be inverted, and isn't drawn.
    DecalInput flat;
    flat.world = Mat4::Identity(), flat.size = Vec3(1, 0, 1);
    std::vector<DecalDraw> draws;
    BuildDecalDraws({flat}, vp, Vec3(0, 2, 6), draws);
    AETHER_CHECK(draws.empty());
}

AETHER_TEST(Decal_DrawsSortByOrderThenFarToNear) {
    const Mat4 vp = Camera();
    const auto at = [](f32 z, i32 order, u32 id) {
        DecalInput d;
        d.world = Mat4::Translation(Vec3(0, 0, z)), d.size = Vec3(1, 1, 1), d.sort_order = order, d.id = id;
        return d;
    };
    // Camera at z = 6: z = -4 is the farthest, z = 3 the nearest.
    std::vector<DecalDraw> draws;
    BuildDecalDraws({at(3, 0, 10), at(-4, 0, 11), at(0, 0, 12), at(3, -1, 13), at(-4, 1, 14)}, vp, Vec3(0, 2, 6), draws);
    AETHER_CHECK(draws.size() == 5);
    if (draws.size() != 5) return;
    // Order -1 first; then order 0 far to near; then order 1.
    AETHER_CHECK(draws[0].id == 13 && draws[1].id == 11 && draws[2].id == 12 && draws[3].id == 10 && draws[4].id == 14);
    AETHER_CHECK(draws[1].distance > draws[2].distance && draws[2].distance > draws[3].distance);
}

AETHER_TEST(Decal_ComponentRoundTripsThroughReflection) {
    RegisterRenderComponents();
    Decal decal;
    decal.texture = "textures/scorch.png";
    decal.size = Vec3(2, 3, 0.5f);
    decal.color = Vec3(0.8f, 0.6f, 0.4f);
    decal.opacity = 0.7f;
    decal.angle_fade_start = 0.6f;
    decal.angle_fade_end = 0.2f;
    decal.sort_order = 3;
    decal.enabled = false;
    const reflect::Json json = reflect::ToJson(decal);
    Decal back;
    AETHER_CHECK(reflect::FromJson(back, json));
    AETHER_CHECK(back.texture == decal.texture && back.size.x == 2.0f && back.size.y == 3.0f && back.size.z == 0.5f);
    AETHER_CHECK(Near(back.color.y, 0.6f, 1e-6f) && Near(back.opacity, 0.7f, 1e-6f) && Near(back.angle_fade_start, 0.6f, 1e-6f) &&
                 Near(back.angle_fade_end, 0.2f, 1e-6f) && back.sort_order == 3 && !back.enabled);
    // A scene that has one loads it as a component.
    World world;
    const Entity e = world.CreateEntity();
    world.AddComponent<Decal>(e, decal);
    AETHER_CHECK(world.HasComponent<Decal>(e) && world.GetComponent<Decal>(e)->sort_order == 3);
}

// --- The pass ----------------------------------------------------------------------------------------------------------------

AETHER_TEST(DecalPass_ConstantsAreExactly128BytesAndCarryTheItem) {
    AETHER_CHECK(sizeof(player::DecalConstants) == 128);
    DecalInput input;
    input.world = Mat4::Translation(Vec3(0, 0, 0)), input.size = Vec3(2, 3, 4);
    std::vector<DecalDraw> draws;
    BuildDecalDraws({input}, Camera(), Vec3(0, 2, 6), draws);
    AETHER_CHECK(draws.size() == 1);
    if (draws.empty()) return;
    player::DecalPassItem item;
    item.draw = draws[0];
    item.texture = 7;
    item.color[0] = 0.1f, item.color[1] = 0.2f, item.color[2] = 0.3f, item.color[3] = 0.4f;
    item.fade_start = 0.6f, item.fade_end = 0.2f;
    const player::DecalConstants c = player::PackDecalConstants(item, 9);
    AETHER_CHECK(c.rect[0] == draws[0].rect[0] && c.rect[3] == draws[0].rect[3]);
    AETHER_CHECK(c.color[0] == 0.1f && c.color[3] == 0.4f && c.fade[0] == 0.6f && c.fade[1] == 0.2f);
    AETHER_CHECK(c.fade[2] == 2.0f && c.fade[3] == 3.0f && c.extra[0] == 4.0f && c.extra[1] == 7.0f && c.extra[2] == 9.0f);
    AETHER_CHECK(c.clip_to_unit.cols[1].y == draws[0].clip_to_unit.cols[1].y);
    // No texture by default.
    AETHER_CHECK(player::DecalPassItem{}.texture == player::kDecalNoTexture && player::kDecalNoTexture == 255);
}

AETHER_TEST(DecalPass_TintIsTheSrgbCurve) {
    AETHER_CHECK(player::DecalTint(0.0f) == 0.0f && player::DecalTint(1.0f) > 0.999f && player::DecalTint(1.0f) < 1.001f);
    AETHER_CHECK(Near(player::DecalTint(0.5f), 0.7354f, 1e-3f) && Near(player::DecalTint(0.0020f), 0.0258f, 1e-3f));
    AETHER_CHECK(player::DecalTint(-1.0f) == 0.0f && player::DecalTint(5.0f) > 0.999f); // clamped
}

AETHER_TEST(DecalPass_ShaderReadsDepthAndProjectsIntoTheBox) {
    const std::string hlsl = player::DecalShaderSource();
    for (const char* part : {"VSMain", "PSMain", "g_ClipToUnit", "GetDimensions", ".Load(int3(int2(position.xy), 0))", "ddx(metric)", "clip(0.5 -"}) {
        AETHER_CHECK(hlsl.find(part) != std::string::npos);
    }
}

AETHER_TEST(DecalPass_ShaderCompilesWithGlslang) {
    if (std::system("glslangValidator --version > /dev/null 2>&1") != 0) {
        std::printf("  (glslangValidator not found: skipping the HLSL compile check)\n");
        return;
    }
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_decal_shader";
    std::filesystem::create_directories(dir);
    const std::filesystem::path src = dir / "decal.hlsl", log = dir / "log.txt";
    {
        std::ofstream f(src);
        f << "#define __spirv__ 1\n" << player::DecalShaderSource();
    }
    for (const auto& stage : {std::pair<const char*, const char*>{"vert", "VSMain"}, {"frag", "PSMain"}}) {
        const std::string cmd = std::string("glslangValidator -D -V -S ") + stage.first + " -e " + stage.second + " \"" + src.string() + "\" -o \"" +
                                (dir / "out.spv").string() + "\" > \"" + log.string() + "\" 2>&1";
        const bool ok = std::system(cmd.c_str()) == 0;
        if (!ok) {
            std::ifstream l(log);
            const std::string text((std::istreambuf_iterator<char>(l)), std::istreambuf_iterator<char>());
            std::printf("%s\n", text.c_str());
        }
        AETHER_CHECK(ok);
    }
    std::filesystem::remove_all(dir);
}

#if defined(AETHER_HAS_VULKAN) && !defined(_WIN32)
// A real draw on Vulkan (lavapipe in CI): a flat "ground" writes depth 0.5 over the target, then decals are drawn. With an identity view
// projection, world coordinates are ndc and depth, so a box centred at z = 0.5 contains the ground and one centred at z = 0.9 does not.
AETHER_TEST(DecalPass_PaintsOnlyWhereTheSurfaceIsInsideTheBox) {
    std::unique_ptr<gfx::rhi::IDevice> device;
    try {
        device = gfx::rhi::CreateDevice(gfx::rhi::Backend::Vulkan, false);
    } catch (const std::exception&) {
    }
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    constexpr u32 kW = 96, kH = 64;
    std::unique_ptr<gfx::rhi::ISwapChain> target = device->CreateSwapChain(nullptr, kW, kH, 2);
    player::DecalPass pass(*device, *target);
    if (!pass.Valid()) {
        std::printf("    (this device has no scene depth texture: decals are not drawn)\n");
        return;
    }
    gfx::rhi::PipelineDesc ground_desc;
    ground_desc.hlsl_source = R"(
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.5, 1.0);
}
float4 PSMain(float4 pos : SV_Position) : SV_Target {
    return float4(0.5, 0.5, 0.5, 1.0);
}
)";
    ground_desc.depth_test = true;
    ground_desc.depth_write = true;
    const gfx::rhi::PipelineHandle ground = device->CreatePipeline(ground_desc, *target);
    AETHER_CHECK(ground.IsValid());

    const Mat4 identity = Mat4::Identity();
    const auto decal_at = [&](f32 x, f32 z, u32 id) {
        DecalInput d;
        d.world = Mat4::Translation(Vec3(x, 0.0f, z)), d.size = Vec3(0.5f, 0.5f, 0.2f), d.id = id;
        return d;
    };
    // id 0: red on the ground, on the left; id 1: green in front of nothing (z = 0.9 is behind the ground), on the right; id 2: half-transparent blue, lower right.
    std::vector<DecalDraw> draws;
    BuildDecalDraws({decal_at(-0.5f, 0.5f, 0), decal_at(0.5f, 0.9f, 1)}, identity, Vec3(0, 0, 0), draws);
    AETHER_CHECK(draws.size() == 2);
    std::vector<player::DecalPassItem> items;
    for (const DecalDraw& d : draws) {
        player::DecalPassItem item;
        item.draw = d;
        if (d.id == 0) item.color[0] = 1.0f, item.color[1] = 0.0f, item.color[2] = 0.0f, item.color[3] = 1.0f;
        else item.color[0] = 0.0f, item.color[1] = 1.0f, item.color[2] = 0.0f, item.color[3] = 1.0f;
        items.push_back(item);
    }

    std::unique_ptr<gfx::rhi::ICommandList> cmd = device->CreateCommandList();
    target->AcquireNextImage();
    cmd->Reset();
    cmd->BeginRenderPass(*target, {0.0f, 0.0f, 0.0f, 1.0f});
    cmd->BindPipeline(ground);
    cmd->Draw(3);
    const u32 drawn = pass.Draw(*cmd, items);
    cmd->EndRenderPass();
    cmd->Close();
    device->WaitForFence(device->Submit(*cmd, target.get()));
    std::vector<u8> pixels;
    AETHER_CHECK(target->ReadBack(pixels));
    target->Present(false);
    AETHER_CHECK(drawn == 2 && pixels.size() == kW * kH * 4);
    if (pixels.size() != kW * kH * 4) return;

    const auto pixel = [&](f32 ndc_x, f32 ndc_y) { // ndc to the pixel it covers (y up)
        const u32 x = static_cast<u32>((ndc_x * 0.5f + 0.5f) * kW), y = static_cast<u32>((0.5f - ndc_y * 0.5f) * kH);
        return &pixels[(static_cast<usize>(y) * kW + x) * 4];
    };
    const auto close = [](const u8* p, int r, int g, int b) { return std::abs(p[0] - r) <= 3 && std::abs(p[1] - g) <= 3 && std::abs(p[2] - b) <= 3; };
    AETHER_CHECK(close(pixel(-0.5f, 0.0f), 255, 0, 0));   // inside the first decal's box: red
    AETHER_CHECK(close(pixel(0.5f, 0.0f), 128, 128, 128)); // the second decal's box is behind the ground: untouched
    AETHER_CHECK(close(pixel(0.0f, 0.0f), 128, 128, 128)); // between them
    AETHER_CHECK(close(pixel(-0.5f, 0.8f), 128, 128, 128)); // above the first decal's rectangle
}
#endif
