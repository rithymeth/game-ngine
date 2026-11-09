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

// ---- Receiver culling: a caster is skipped for a cascade only when its shadow cannot land on a pixel that reads that cascade. ----

namespace {

struct Lcg {
    u64 state;
    explicit Lcg(u64 seed) : state(seed) {}
    f32 Next() { // 0 .. 1
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<f32>((state >> 40) & 0xFFFFFF) / static_cast<f32>(0x1000000);
    }
    f32 Range(f32 lo, f32 hi) { return lo + (hi - lo) * Next(); }
};

View RandomView(Lcg& rng) {
    Camera camera;
    camera.fov_degrees = rng.Range(40.0f, 90.0f);
    camera.near_plane = 0.1f;
    camera.far_plane = 400.0f;
    const f32 yaw = rng.Range(0.0f, 6.2831853f), pitch = rng.Range(-1.0f, 0.35f);
    const Vec3 forward(std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch));
    const Vec3 right = forward.Cross(Vec3(0, 1, 0)).Normalized();
    const Vec3 up = right.Cross(forward);
    Mat4 world = Mat4::Identity();
    world.cols[0] = Vec4(right.x, right.y, right.z, 0);
    world.cols[1] = Vec4(up.x, up.y, up.z, 0);
    world.cols[2] = Vec4(-forward.x, -forward.y, -forward.z, 0);
    world.cols[3] = Vec4(rng.Range(-30.0f, 30.0f), rng.Range(0.5f, 12.0f), rng.Range(-30.0f, 30.0f), 1);
    return MakeView(camera, world, rng.Range(1.0f, 2.0f));
}

// The cascade the scene shader would read for a pixel at `p` with normal `n`: the first whose map holds it, two texels clear of the edge.
int CascadeReadBy(const std::vector<ShadowCascade>& cascades, u32 resolution, const Vec3& p, const Vec3& n, const Vec3& to_sun) {
    const f32 n_dot_l = std::clamp(n.Dot(to_sun), 0.0f, 1.0f);
    const f32 slope = std::sqrt(std::clamp(1.0f - n_dot_l * n_dot_l, 0.0f, 1.0f));
    const f32 margin = 2.0f / static_cast<f32>(resolution);
    for (usize i = 0; i < cascades.size(); ++i) {
        const Vec3 offset = p + n * (cascades[i].texel_size * (0.5f + 1.5f * slope));
        const Vec4 clip = cascades[i].view_projection * Vec4(offset.x, offset.y, offset.z, 1.0f);
        const f32 u = clip.x * 0.5f + 0.5f, v = 0.5f - clip.y * 0.5f;
        if (u < margin || v < margin || u > 1.0f - margin || v > 1.0f - margin || clip.z > 1.0f) continue;
        return static_cast<int>(i);
    }
    return -1;
}

} // namespace

AETHER_TEST(Shadows_ReceiverCullingNeverDropsACasterThatShadowsAPixel) {
    Lcg rng(0x5EEDF00Dull);
    constexpr u32 kResolution = 2048;
    u64 occluders = 0, violations = 0, sphere_violations = 0, pairs = 0, culled_pairs = 0, near_pairs = 0, near_culled = 0;
    for (int scenario = 0; scenario < 400; ++scenario) {
        const View view = RandomView(rng);
        // The way the light travels: always some way down, from any side.
        const f32 azimuth = rng.Range(0.0f, 6.2831853f), elevation = rng.Range(0.15f, 1.5f);
        const Vec3 travel(-std::cos(azimuth) * std::cos(elevation), -std::sin(elevation), -std::sin(azimuth) * std::cos(elevation));
        const Vec3 to_sun = travel * -1.0f;
        const std::vector<ShadowCascade> cascades = player::FitShadowCascades(view, travel, kResolution, 4, 100.0f);
        AETHER_CHECK(cascades.size() == 4);
        if (cascades.size() != 4) return;
        std::vector<player::CascadeReceivers> regions;
        for (usize i = 0; i < cascades.size(); ++i) regions.push_back(player::CascadeReceiverRegion(view, cascades, i));

        // Casters near the camera, against every cascade: how many would be skipped (a test of the cull doing something).
        for (int c = 0; c < 6; ++c) {
            player::ShadowCaster caster;
            caster.bounds_center = view.position + view.forward * rng.Range(1.0f, 5.0f) + Vec3(rng.Range(-1.5f, 1.5f), rng.Range(-1.0f, 0.5f), rng.Range(-1.5f, 1.5f));
            caster.bounds_radius = rng.Range(0.3f, 1.2f);
            for (usize i = 0; i < cascades.size(); ++i) {
                if (!player::CasterTouchesCascade(caster, cascades[i])) continue;
                ++near_pairs;
                if (!player::CasterReachesReceivers(caster, cascades[i], regions[i])) ++near_culled;
            }
        }

        const f32 half_tan = view.perspective ? std::tan(Radians(view.fov_degrees) * 0.5f) : 0.0f;
        for (int sample = 0; sample < 400; ++sample) {
            // A pixel anywhere in the view out to the shadow distance, with a normal facing roughly up.
            const f32 depth = view.near_plane + (std::min(view.far_plane, 100.0f) - view.near_plane) * rng.Next() * rng.Next();
            const f32 half_h = depth * half_tan, half_w = half_h * view.aspect;
            const Vec3 p = view.position + view.forward * depth + view.Right() * (rng.Range(-1.0f, 1.0f) * half_w) +
                           view.Up() * (rng.Range(-1.0f, 1.0f) * half_h);
            Vec3 n(rng.Range(-0.7f, 0.7f), 1.0f, rng.Range(-0.7f, 0.7f));
            n = n.Normalized();
            const int read_by = CascadeReadBy(cascades, kResolution, p, n, to_sun);
            if (read_by < 0) continue;
            // A sphere on the ray from the pixel to the sun: it shades the pixel, and so must be drawn into the cascade the pixel reads.
            const f32 radius = rng.Range(0.05f, 3.0f);
            const f32 t = rng.Range(0.3f, 60.0f);
            const Vec3 off(rng.Range(-1.0f, 1.0f), rng.Range(-1.0f, 1.0f), rng.Range(-1.0f, 1.0f));
            player::ShadowCaster caster;
            caster.bounds_center = p + to_sun * t + off * (0.9f * radius / std::max(off.Length(), 1.0f));
            caster.bounds_radius = radius;
            ++occluders;
            const usize k = static_cast<usize>(read_by);
            if (!player::CasterTouchesCascade(caster, cascades[k])) ++sphere_violations; // the test before this one (a reference)
            if (!player::CasterReachesReceivers(caster, cascades[k], regions[k])) ++violations;
            ++pairs;
            for (usize i = 0; i < cascades.size(); ++i) {
                if (player::CasterTouchesCascade(caster, cascades[i]) && !player::CasterReachesReceivers(caster, cascades[i], regions[i])) ++culled_pairs;
            }
        }
    }
    std::printf("    %llu occluders of pixels checked: %llu wrongly culled (the sphere test alone: %llu); casters near the camera culled from %.0f percent of the "
                "cascades their sphere touches\n",
                static_cast<unsigned long long>(occluders), static_cast<unsigned long long>(violations),
                static_cast<unsigned long long>(sphere_violations), near_pairs ? 100.0 * static_cast<double>(near_culled) / static_cast<double>(near_pairs) : 0.0);
    AETHER_CHECK(occluders > 50000);
    AETHER_CHECK(violations == 0);
    AETHER_CHECK(near_pairs > 1000 && near_culled * 4 > near_pairs); // it culls a good share of what a character near the camera would draw
    (void)pairs;
    (void)culled_pairs;
}

AETHER_TEST(Shadows_ReceiverRegionIsTheViewFromThePreviousSliceOn) {
    const View view = TestView();
    const std::vector<ShadowCascade> cascades = player::FitShadowCascades(view, Vec3(0.3f, -1.0f, 0.2f), 1024, 4, 100.0f);
    AETHER_CHECK(cascades.size() == 4);
    if (cascades.size() != 4) return;
    const player::CascadeReceivers first = player::CascadeReceiverRegion(view, cascades, 0), last = player::CascadeReceiverRegion(view, cascades, 3);
    AETHER_CHECK(first.valid && last.valid && first.hull_count >= 3 && last.hull_count >= 3);
    AETHER_CHECK(!player::CascadeReceiverRegion(view, cascades, 4).valid); // no such cascade
    // A caster with no bounding radius is always kept, whatever the region.
    player::ShadowCaster unbounded;
    AETHER_CHECK(player::CasterReachesReceivers(unbounded, cascades[3], last));
    // A caster far off to the side of the view, out of every outline, is dropped; one at the camera is kept for the first cascade.
    player::ShadowCaster far_away;
    far_away.bounds_center = Vec3(5000.0f, 0.0f, 5000.0f);
    far_away.bounds_radius = 1.0f;
    AETHER_CHECK(!player::CasterReachesReceivers(far_away, cascades[0], first));
    player::ShadowCaster at_camera;
    at_camera.bounds_center = view.position + view.forward * 2.0f;
    at_camera.bounds_radius = 0.5f;
    AETHER_CHECK(player::CasterReachesReceivers(at_camera, cascades[0], first));
}

// The pixels the random test almost never draws: those within two texels of their own slice's map edge, which the shader hands to the next
// cascade. A cascade's sphere is tight around its slice, so only a slice corner comes that close, and only for some sun directions; the test
// sweeps the sun's azimuth and elevation around each corner of each slice over several cameras, keeps the pixels the shader really does hand on,
// and checks their occluders.
AETHER_TEST(Shadows_ReceiverCullingKeepsCastersForPixelsThatFallThroughToTheNextCascade) {
    Lcg rng(0xFA11ull);
    constexpr u32 kResolution = 2048;
    u64 fall_through_pixels = 0, occluders = 0, violations = 0;
    for (int scenario = 0; scenario < 120; ++scenario) {
        const View view = RandomView(rng);
        // The slices' corners, from the cascades of any sun (the splits do not depend on it).
        const std::vector<ShadowCascade> reference = player::FitShadowCascades(view, Vec3(0.1f, -1.0f, 0.1f), kResolution, 4, 100.0f);
        if (reference.size() != 4) return;
        for (usize j = 0; j + 1 < reference.size(); ++j) {
            Vec3 corners[8];
            FrustumSliceCorners(view, reference[j].split_near, reference[j].split_far, corners);
            Vec3 centre(0, 0, 0);
            for (const Vec3& c : corners) centre = centre + c * 0.125f;
            for (const Vec3& corner : corners) {
                for (const f32 elevation : {0.45f, 0.95f, 1.4f}) {
                    for (int degrees = 0; degrees < 360; degrees += 2) {
                        const f32 azimuth = static_cast<f32>(degrees) * 3.14159265f / 180.0f;
                        const Vec3 travel(-std::cos(azimuth) * std::cos(elevation), -std::sin(elevation), -std::sin(azimuth) * std::cos(elevation));
                        const Vec3 to_sun = travel * -1.0f;
                        const std::vector<ShadowCascade> cascades = player::FitShadowCascades(view, travel, kResolution, 4, 100.0f);
                        const Vec3 p = corner + (centre - corner) * 0.0005f;
                        const Vec3 n(0.0f, 1.0f, 0.0f);
                        const int read_by = CascadeReadBy(cascades, kResolution, p, n, to_sun);
                        if (read_by <= static_cast<int>(j)) continue; // read by its own cascade, an earlier one, or none
                        ++fall_through_pixels;
                        const usize k = static_cast<usize>(read_by);
                        const player::CascadeReceivers region = player::CascadeReceiverRegion(view, cascades, k);
                        for (int o = 0; o < 40; ++o) {
                            ++occluders;
                            const f32 radius = rng.Range(0.05f, 3.0f);
                            const Vec3 off(rng.Range(-1.0f, 1.0f), rng.Range(-1.0f, 1.0f), rng.Range(-1.0f, 1.0f));
                            player::ShadowCaster caster;
                            caster.bounds_center = p + to_sun * rng.Range(0.3f, 60.0f) + off * (0.9f * radius / std::max(off.Length(), 1.0f));
                            caster.bounds_radius = radius;
                            if (!player::CasterReachesReceivers(caster, cascades[k], region)) ++violations;
                        }
                    }
                }
            }
        }
    }
    std::printf("    %llu pixels fell through to a later cascade; %llu occluders of them checked, %llu wrongly culled%s",
                static_cast<unsigned long long>(fall_through_pixels), static_cast<unsigned long long>(occluders),
                static_cast<unsigned long long>(violations), "\n");
    AETHER_CHECK(fall_through_pixels > 20); // the case is really exercised
    AETHER_CHECK(violations == 0);
}
