#include "../player/src/frame_lights.h"
#include "../player/src/shadow_pass.h"
#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"
#include "aether/player/scene_renderer.h"
#include "test_framework.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace aether;
namespace rhi = aether::gfx::rhi;

// Render roadmap step 2: cascaded sun shadows. Depth textures in the RHI (IDevice::CreateDepthTexture, ICommandList::BeginDepthPass),
// the player's ShadowPass that fills a cascade atlas with them, and the frame data the scene shader reads it through.

namespace {

std::unique_ptr<rhi::IDevice> MakeDevice(rhi::Backend backend) {
    std::unique_ptr<rhi::IDevice> device;
    if (rhi::IsBackendAvailable(backend)) {
        try {
            device = rhi::CreateDevice(backend, false);
        } catch (const std::exception&) {
        }
    }
    if (!device) std::printf("    (no %s device: skipping)\n", backend == rhi::Backend::Vulkan ? "Vulkan" : "D3D12");
    return device;
}

View TestView() {
    Camera camera;
    camera.fov_degrees = 60.0f;
    camera.near_plane = 0.1f;
    camera.far_plane = 10.0f;
    // At (0, 2, 5), looking down the -Z axis tilted 20 degrees toward the ground.
    const f32 a = -20.0f * 3.14159265f / 180.0f;
    Mat4 world = Mat4::Identity();
    world.cols[1] = Vec4(0, std::cos(a), std::sin(a), 0);
    world.cols[2] = Vec4(0, -std::sin(a), std::cos(a), 0);
    world.cols[3] = Vec4(0, 2, 5, 1);
    return MakeView(camera, world, 1.0f);
}

// A flat quad, `half` metres each side of (cx, 0, cz), in the SceneVertex layout (position, normal, uv, tangent: 12 floats).
std::vector<f32> Quad(f32 cx, f32 cz, f32 half) {
    std::vector<f32> v;
    const f32 corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (const auto& c : corners) {
        const f32 u = c[0] * 0.5f + 0.5f, w = c[1] * 0.5f + 0.5f;
        for (const f32 x : {cx + c[0] * half, 0.0f, cz + c[1] * half, 0.0f, 1.0f, 0.0f, u, w, 1.0f, 0.0f, 0.0f, 1.0f}) v.push_back(x);
    }
    return v;
}

// The texel of the atlas a world point lands in, through cascade 0.
bool AtlasTexel(const ShadowCascade& cascade, u32 resolution, const Vec3& p, u32& x, u32& y) {
    const Vec4 clip = cascade.view_projection * Vec4(p.x, p.y, p.z, 1.0f);
    const f32 u = clip.x * 0.5f + 0.5f, v = 0.5f - clip.y * 0.5f;
    if (u < 0.0f || u >= 1.0f || v < 0.0f || v >= 1.0f) return false;
    x = static_cast<u32>(u * static_cast<f32>(resolution));
    y = static_cast<u32>(v * static_cast<f32>(resolution));
    return true;
}

// Writes the atlas, texel for pixel, to the back buffer: green where something was drawn (depth < 1), black where it is still clear.
constexpr char kShowDepthShader[] = R"(
struct PushConstants { uint atlas; uint pad0; uint pad1; uint pad2; };
#ifdef __spirv__
[[vk::push_constant]]
#endif
ConstantBuffer<PushConstants> g_PC : register(b0);
Texture2D g_Textures[128] : register(t0, space0);
SamplerState g_Sampler : register(s0, space1);
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float4 PSMain(float4 position : SV_Position) : SV_Target {
    float d = g_Textures[g_PC.atlas].Load(int3(int2(position.xy), 0)).r;
    return d < 0.999 ? float4(0, 1, 0, 1) : float4(0, 0, 0, 1);
}
)";

} // namespace

AETHER_TEST(Shadows_AtlasLayoutAndFrameData) {
    u32 w = 0, h = 0;
    player::ShadowAtlasSize(1024, 1, w, h);
    AETHER_CHECK(w == 1024 && h == 1024);
    player::ShadowAtlasSize(1024, 2, w, h);
    AETHER_CHECK(w == 2048 && h == 1024);
    player::ShadowAtlasSize(1024, 4, w, h);
    AETHER_CHECK(w == 2048 && h == 2048);
    u32 x = 0, y = 0;
    player::ShadowCascadeOrigin(1024, 3, x, y);
    AETHER_CHECK(x == 1024 && y == 1024);

    // The C++ frame data matches the HLSL struct: the shadow block follows the spot lights, 16-byte aligned.
    static_assert(sizeof(player::FrameData) >= 864);
    static_assert(offsetof(player::FrameData, shadow_view_projection) == 528 && offsetof(player::FrameData, shadow_info) == 848);

    const std::vector<ShadowCascade> cascades = player::FitShadowCascades(TestView(), Vec3(0, -1, 0), 512, 4, 20.0f);
    AETHER_CHECK(cascades.size() == 4);
    if (cascades.size() != 4) return;
    const player::ShadowFrameData data = player::PackShadowFrameData(cascades, 512, 9);
    AETHER_CHECK(data.info[0] == 9 && data.info[1] == 4 && data.info[2] == 512);
    AETHER_CHECK(data.cascade[1][0] == 512.0f && data.cascade[1][1] == 0.0f && data.cascade[2][1] == 512.0f);
    // Nearer cascades are finer; the depth scale is 1 / the orthographic far plane.
    AETHER_CHECK(data.cascade[0][2] > 0.0f && data.cascade[0][2] < data.cascade[3][2]);
    AETHER_CHECK(std::fabs(data.cascade[0][3] * (1.0f / -cascades[0].projection.cols[2].z) - 1.0f) < 1e-4f);
    AETHER_CHECK(data.view_projection[2][0] == cascades[2].view_projection.cols[0].x);
    // No atlas (a slot outside the table), or no cascades: shadows off.
    AETHER_CHECK(player::PackShadowFrameData(cascades, 512, 200).info[0] == 255 && player::PackShadowFrameData({}, 512, 9).info[1] == 0);
}

AETHER_TEST(Shadows_CasterCullingAgainstACascade) {
    const std::vector<ShadowCascade> cascades = player::FitShadowCascades(TestView(), Vec3(0, -1, 0), 512, 1, 10.0f);
    AETHER_CHECK(cascades.size() == 1);
    if (cascades.empty()) return;
    player::ShadowCaster caster;
    caster.bounds_radius = 0.5f;
    caster.bounds_center = Vec3(0, 0, 0);
    AETHER_CHECK(player::CasterTouchesCascade(caster, cascades[0]));
    caster.bounds_center = Vec3(500, 0, 0); // far to the side
    AETHER_CHECK(!player::CasterTouchesCascade(caster, cascades[0]));
    caster.bounds_center = Vec3(0, 400, 0); // high above, between the sun and the cascade: kept (its depth is clamped)
    AETHER_CHECK(player::CasterTouchesCascade(caster, cascades[0]));
    caster.bounds_center = Vec3(0, -400, 0); // far below: past the map's far plane
    AETHER_CHECK(!player::CasterTouchesCascade(caster, cascades[0]));
    caster.bounds_radius = -1.0f; // unbounded (skinned): always drawn
    AETHER_CHECK(player::CasterTouchesCascade(caster, cascades[0]));
}

AETHER_TEST(Shadows_SceneShaderReadsTheAtlas) {
    const std::string source = player::SceneShaderSource(true, true);
    AETHER_CHECK(source.find("float SunShadow(") != std::string::npos && source.find("shadow_view_projection[4]") != std::string::npos);
    AETHER_CHECK(source.find("* sun_radiance * sun_visibility") != std::string::npos);
}

static void Shadows_DepthTextures(rhi::Backend backend) {
    std::unique_ptr<rhi::IDevice> device = MakeDevice(backend);
    if (!device) return;
    const rhi::SampledTextureHandle depth = device->CreateDepthTexture(64, 32);
    const rhi::SampledTextureHandle color = device->CreateRenderTexture(64, 32);
    AETHER_CHECK(depth.IsValid() && color.IsValid());
    if (!depth.IsValid() || !color.IsValid()) return;
    bool threw = false;
    try {
        (void)device->CreateDepthTexture(0, 4);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    AETHER_CHECK(threw);
    std::unique_ptr<rhi::ICommandList> cmd = device->CreateCommandList();
    cmd->Reset();
    const rhi::ClearColor black{0, 0, 0, 1};
    AETHER_CHECK(!cmd->BeginTexturePass(depth, &black)); // a depth texture is no colour target
    AETHER_CHECK(!cmd->BeginDepthPass(color, true));     // nor the other way round
    AETHER_CHECK(cmd->BeginDepthPass(depth, true));
    AETHER_CHECK(!cmd->BeginDepthPass(depth, false)); // nested
    AETHER_CHECK(!cmd->CopySceneDepth());
    cmd->EndRenderPass();
    AETHER_CHECK(cmd->BeginDepthPass(depth, false)); // again, keeping its contents
    cmd->EndRenderPass();
    cmd->Close();
    device->WaitForFence(device->Submit(*cmd, nullptr));
    AETHER_CHECK(device->DestroyTexture(depth));
    cmd->Reset();
    AETHER_CHECK(!cmd->BeginDepthPass(depth, true)); // destroyed
    cmd->Close();
    device->WaitForFence(device->Submit(*cmd, nullptr));
    device->DestroyTexture(color);
}

static void Shadows_PassDrawsOpaqueAndAlphaTestedCasters(rhi::Backend backend) {
    std::unique_ptr<rhi::IDevice> device = MakeDevice(backend);
    if (!device) return;
    constexpr u32 kRes = 64;
    std::unique_ptr<rhi::ISwapChain> target = device->CreateSwapChain(nullptr, kRes, kRes, 2);
    player::ShadowPass shadows(*device, *target);
    AETHER_CHECK(shadows.Valid());
    if (!shadows.Valid()) return;
    shadows.Configure(kRes, 1, 10.0f);

    // An opaque quad at the origin, and an alpha-tested one beside it whose texture is fully transparent: it must leave no depth.
    const std::vector<f32> opaque_vertices = Quad(0.0f, 0.0f, 0.4f), masked_vertices = Quad(1.5f, 0.0f, 0.4f);
    const u32 quad_indices[6] = {0, 1, 2, 0, 2, 3};
    const rhi::BufferHandle opaque_vb = device->CreateVertexBuffer(opaque_vertices.data(), opaque_vertices.size() * sizeof(f32));
    const rhi::BufferHandle masked_vb = device->CreateVertexBuffer(masked_vertices.data(), masked_vertices.size() * sizeof(f32));
    const rhi::BufferHandle ib = device->CreateIndexBuffer(quad_indices, sizeof(quad_indices), rhi::IndexFormat::UInt32);
    const u8 clear_texel[4] = {255, 255, 255, 0};
    const rhi::SampledTextureHandle transparent = device->CreateTexture(1, 1, clear_texel);
    std::vector<player::ShadowCaster> casters(2);
    casters[0].vertices = opaque_vb, casters[0].indices = ib, casters[0].index_count = 6, casters[0].model = Mat4::Identity();
    casters[1] = casters[0];
    casters[1].vertices = masked_vb;
    casters[1].alpha_tested = true;
    casters[1].base_color_texture = transparent.index;

    const View view = TestView();
    std::unique_ptr<rhi::ICommandList> cmd = device->CreateCommandList();
    target->AcquireNextImage();
    cmd->Reset();
    const player::ShadowFrameData data = shadows.Render(*cmd, view, Vec3(0, -1, 0), casters);
    AETHER_CHECK(data.info[0] == shadows.Atlas().index && data.info[1] == 1 && shadows.LastDrawCount() == 2);

    rhi::PipelineDesc show;
    show.hlsl_source = kShowDepthShader;
    show.push_constant_size_bytes = 16;
    show.enable_bindless_textures = true;
    const rhi::PipelineHandle show_pipeline = device->CreatePipeline(show, *target);
    cmd->BeginRenderPass(*target, {1, 0, 1, 1});
    cmd->BindPipeline(show_pipeline);
    cmd->BindBindlessTextures();
    const u32 push[4] = {shadows.Atlas().index, 0, 0, 0};
    cmd->SetPushConstants(push, sizeof(push));
    cmd->Draw(3);
    cmd->EndRenderPass();
    cmd->Close();
    device->WaitForFence(device->Submit(*cmd, target.get()));
    std::vector<u8> pixels;
    AETHER_CHECK(target->ReadBack(pixels) && pixels.size() == kRes * kRes * 4);
    target->Present(false);
    if (pixels.size() != kRes * kRes * 4) return;
    const auto drawn = [&](const Vec3& p) {
        const std::vector<ShadowCascade> cascades = player::FitShadowCascades(view, Vec3(0, -1, 0), kRes, 1, 10.0f);
        u32 x = 0, y = 0;
        if (cascades.empty() || !AtlasTexel(cascades[0], kRes, p, x, y)) return -1;
        return pixels[(static_cast<usize>(y) * kRes + x) * 4 + 1] > 128 ? 1 : 0;
    };
    AETHER_CHECK(drawn(Vec3(0, 0, 0)) == 1);      // the opaque caster's centre
    AETHER_CHECK(drawn(Vec3(1.5f, 0, 0)) == 0);   // the transparent alpha-tested one: clipped
    AETHER_CHECK(drawn(Vec3(-1.5f, 0, 0)) == 0);  // bare ground: still the clear depth
    usize lit = 0;
    for (usize i = 0; i < kRes * kRes; ++i) lit += pixels[i * 4 + 1] > 128 ? 1 : 0;
    AETHER_CHECK(lit > 0 && lit < kRes * kRes / 2);

    // Configure(0, ...) turns shadows off and frees the atlas.
    shadows.Configure(0, 1);
    AETHER_CHECK(!shadows.Atlas().IsValid());
    cmd->Reset();
    AETHER_CHECK(shadows.Render(*cmd, view, Vec3(0, -1, 0), casters).info[0] == 255);
    cmd->Close();
    device->WaitForFence(device->Submit(*cmd, nullptr));
    device->DestroyPipeline(show_pipeline);
    device->DestroyTexture(transparent);
    device->DestroyBuffer(opaque_vb);
    device->DestroyBuffer(masked_vb);
    device->DestroyBuffer(ib);
}

// The rendering checks above, on both backends (Vulkan has render targets since the parity work).
AETHER_TEST(Shadows_DepthTextures_D3D12) { Shadows_DepthTextures(rhi::Backend::D3D12); }
AETHER_TEST(Shadows_DepthTextures_Vulkan) { Shadows_DepthTextures(rhi::Backend::Vulkan); }
AETHER_TEST(Shadows_PassDrawsOpaqueAndAlphaTestedCasters_D3D12) { Shadows_PassDrawsOpaqueAndAlphaTestedCasters(rhi::Backend::D3D12); }
AETHER_TEST(Shadows_PassDrawsOpaqueAndAlphaTestedCasters_Vulkan) { Shadows_PassDrawsOpaqueAndAlphaTestedCasters(rhi::Backend::Vulkan); }
