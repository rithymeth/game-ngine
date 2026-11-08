#include "../player/src/environment.h"
#include "../player/src/frame_lights.h"
#include "../player/src/sky_pass.h"
#include "aether/assets/image.h"
#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"
#include "aether/player/scene_renderer.h"
#include "test_framework.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

using namespace aether;
namespace rhi = aether::gfx::rhi;

// Render roadmap step 3: image-based lighting from the sky. Half-float textures with mips in the RHI (IDevice::CreateHalfFloatTexture),
// the CPU environment builder (player/src/environment.h: procedural sky, .hdr decoding, GGX prefiltering, irradiance SH), and the
// SkyPass that puts it in the frame data and draws the background.

namespace {

constexpr f32 kTestPi = 3.14159265f;

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

bool Near(f32 a, f32 b, f32 tolerance) { return std::fabs(a - b) <= tolerance; }
bool Near(const Vec3& a, const Vec3& b, f32 tolerance) { return Near(a.x, b.x, tolerance) && Near(a.y, b.y, tolerance) && Near(a.z, b.z, tolerance); }

player::EnvironmentImage Uniform(u32 width, const Vec3& c) {
    std::vector<f32> rgb;
    for (u32 i = 0; i < width * (width / 2); ++i) rgb.insert(rgb.end(), {c.x, c.y, c.z});
    return player::MakeEnvironment(width, width / 2, rgb);
}

// A level's texel, from the prefiltered chain's halves.
Vec3 LevelTexel(const player::PrefilteredEnvironment& env, u32 level, u32 x, u32 y) {
    usize offset = 0;
    for (u32 i = 0; i < level; ++i) offset += static_cast<usize>(env.width >> i) * (env.height >> i);
    const usize t = (offset + static_cast<usize>(y) * (env.width >> level) + x) * 4;
    return Vec3(player::HalfToFloat(env.halves[t]), player::HalfToFloat(env.halves[t + 1]), player::HalfToFloat(env.halves[t + 2]));
}

// Radiance format, flat scanlines (no run-length encoding), from (r, g, b, e) quads.
std::vector<u8> RgbeFile(u32 width, u32 height, const std::vector<u8>& quads) {
    const std::string header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " + std::to_string(height) + " +X " + std::to_string(width) + "\n";
    std::vector<u8> out(header.begin(), header.end());
    out.insert(out.end(), quads.begin(), quads.end());
    return out;
}

} // namespace


AETHER_TEST(Ibl_HalfFloatConversion) {
    AETHER_CHECK(player::FloatToHalf(1.0f) == 0x3C00 && player::FloatToHalf(-2.0f) == 0xC000 && player::FloatToHalf(0.0f) == 0);
    AETHER_CHECK(player::FloatToHalf(65504.0f) == 0x7BFF && player::FloatToHalf(1e6f) == 0x7C00); // the largest half; overflow is infinity
    AETHER_CHECK(player::FloatToHalf(std::numeric_limits<f32>::infinity()) == 0x7C00 && (player::FloatToHalf(std::nanf("")) & 0x3FF) != 0);
    for (const f32 v : {0.5f, 3.14159f, 1000.25f, 6.0e-5f, 1.0e-6f, 40000.0f}) {
        const f32 back = player::HalfToFloat(player::FloatToHalf(v));
        AETHER_CHECK(std::fabs(back - v) <= std::fabs(v) * 1e-3f + 6e-8f);
    }
}

AETHER_TEST(Ibl_EquirectangularDirections) {
    AETHER_CHECK(Near(player::EquirectToDirection(0.5f, 0.5f), Vec3(0, 0, -1), 1e-5f));   // the middle looks down -Z
    AETHER_CHECK(Near(player::EquirectToDirection(0.75f, 0.5f), Vec3(1, 0, 0), 1e-5f));   // a quarter to the right is +X
    AETHER_CHECK(Near(player::EquirectToDirection(0.3f, 0.0f), Vec3(0, 1, 0), 1e-5f));    // the top row is straight up
    for (const f32 u : {0.1f, 0.4f, 0.6f, 0.9f}) {
        for (const f32 v : {0.2f, 0.5f, 0.8f}) {
            f32 u2 = 0, v2 = 0;
            player::DirectionToEquirect(player::EquirectToDirection(u, v), u2, v2);
            AETHER_CHECK(Near(u, u2, 1e-4f) && Near(v, v2, 1e-4f));
        }
    }
}

AETHER_TEST(Ibl_UniformSkyIsUniformAtEveryLevel) {
    const player::PrefilteredEnvironment env = player::PrefilterEnvironment(Uniform(128, Vec3(0.5f, 1.0f, 2.0f)));
    AETHER_CHECK(env.width == 128 && env.height == 64 && env.mip_count == 5); // 128 down to 8 wide
    AETHER_CHECK(env.halves.size() == (128 * 64 + 64 * 32 + 32 * 16 + 16 * 8 + 8 * 4) * 4);
    if (env.mip_count != 5) return;
    for (u32 level = 0; level < env.mip_count; ++level) {
        AETHER_CHECK(Near(LevelTexel(env, level, 0, 0), Vec3(0.5f, 1.0f, 2.0f), 0.01f));
        AETHER_CHECK(Near(LevelTexel(env, level, (env.width >> level) / 2, (env.height >> level) / 2), Vec3(0.5f, 1.0f, 2.0f), 0.01f));
    }
    // Irradiance from a uniform radiance L is pi L, whichever way the surface faces.
    for (const Vec3& n : {Vec3(0, 1, 0), Vec3(0, -1, 0), Vec3(1, 0, 0), Vec3(0.6f, 0, 0.8f)}) {
        AETHER_CHECK(Near(player::EvaluateIrradiance(env.irradiance_sh, n), Vec3(0.5f, 1.0f, 2.0f) * kTestPi, 0.05f));
    }
}

AETHER_TEST(Ibl_RougherLevelsSpreadABrightSpot) {
    // Black, with a 4x4 texel spot of 100 straight ahead (-Z, the image's centre).
    std::vector<f32> rgb(256 * 128 * 3, 0.0f);
    for (u32 y = 62; y < 66; ++y)
        for (u32 x = 126; x < 130; ++x)
            for (u32 c = 0; c < 3; ++c) rgb[(y * 256 + x) * 3 + c] = 100.0f;
    const player::PrefilteredEnvironment env = player::PrefilterEnvironment(player::MakeEnvironment(256, 128, rgb));
    AETHER_CHECK(env.mip_count == 6);
    if (env.mip_count != 6) return;
    const auto centre = [&](u32 level) { return LevelTexel(env, level, (env.width >> level) / 2, (env.height >> level) / 2).x; };
    const auto aside = [&](u32 level) { // 30 degrees to the side of the spot
        const u32 w = env.width >> level;
        return LevelTexel(env, level, w / 2 + w / 12, (env.height >> level) / 2).x;
    };
    AETHER_CHECK(centre(0) > 20.0f && aside(0) == 0.0f); // level 0: the spot, sharp
    AETHER_CHECK(centre(5) < centre(2) && centre(2) < centre(0)); // rougher: dimmer at the centre...
    AETHER_CHECK(aside(4) > aside(1) + 0.01f);                   // ...and wider
    // The diffuse light from it falls on surfaces facing it and not on those facing away.
    const Vec3 facing = player::EvaluateIrradiance(env.irradiance_sh, Vec3(0, 0, -1));
    const Vec3 away = player::EvaluateIrradiance(env.irradiance_sh, Vec3(0, 0, 1));
    AETHER_CHECK(facing.x > 0.01f && away.x < facing.x * 0.1f);
}

AETHER_TEST(Ibl_RotationTurnsTheSkyAboutUp) {
    // A bright column at u = 0.5 (-Z) turned by 90 degrees lands at u = 0.75 (+X).
    std::vector<f32> rgb(64 * 32 * 3, 0.0f);
    for (u32 y = 0; y < 32; ++y)
        for (u32 c = 0; c < 3; ++c) rgb[(y * 64 + 32) * 3 + c] = rgb[(y * 64 + 31) * 3 + c] = 1.0f;
    const player::EnvironmentImage turned = player::MakeEnvironment(64, 32, rgb, 90.0f);
    AETHER_CHECK(turned.rgb[(16 * 64 + 48) * 3] > 0.4f && turned.rgb[(16 * 64 + 32) * 3] == 0.0f);
}

AETHER_TEST(Ibl_ImagesAreCleanedAndChecked) {
    const player::EnvironmentImage cleaned = player::MakeEnvironment(2, 1, {std::nanf(""), -1.0f, 1e9f, 1.0f, std::numeric_limits<f32>::infinity(), 0.25f});
    AETHER_CHECK(cleaned.rgb.size() == 6 && cleaned.rgb[0] == 0.0f && cleaned.rgb[1] == 0.0f && cleaned.rgb[2] == 65000.0f && cleaned.rgb[4] == 0.0f);
    AETHER_CHECK(player::MakeEnvironment(2, 2, {1, 2, 3}).width == 0); // wrong size
    AETHER_CHECK(player::PrefilterEnvironment(player::EnvironmentImage{}).mip_count == 0);
    // sRGB 8-bit: 255 is 1, 188 about 0.5.
    const player::EnvironmentImage ldr = player::MakeEnvironmentFromSrgb(1, 1, {255, 188, 0, 255});
    AETHER_CHECK(ldr.rgb.size() == 3 && Near(ldr.rgb[0], 1.0f, 1e-5f) && Near(ldr.rgb[1], 0.5f, 0.01f) && ldr.rgb[2] == 0.0f);
}

AETHER_TEST(Ibl_ProceduralSkyLightsFromAbove) {
    const player::PrefilteredEnvironment env = player::PrefilterEnvironment(player::MakeProceduralSky());
    AETHER_CHECK(env.width == 256 && env.mip_count == 6);
    const Vec3 up = player::EvaluateIrradiance(env.irradiance_sh, Vec3(0, 1, 0)) * (1.0f / kTestPi);
    const Vec3 down = player::EvaluateIrradiance(env.irradiance_sh, Vec3(0, -1, 0)) * (1.0f / kTestPi);
    // As bright, on a white upward-facing surface, as the flat ambient term was (0.28 to 0.42); blue overhead; dimmer from the ground.
    AETHER_CHECK(up.x > 0.2f && up.z < 0.7f && up.z > up.x && down.x < up.x && down.z < up.z);
}

AETHER_TEST(Ibl_DecodesRadianceImages) {
    // (1, 1, 1) and (4, 2, 1): mantissa / 256 * 2^(e - 128).
    const std::vector<u8> file = RgbeFile(2, 1, {128, 128, 128, 129, 128, 64, 32, 131});
    AETHER_CHECK(assets::IsHdrImage(file));
    assets::HdrImageData image;
    AETHER_CHECK(assets::DecodeHdrImageFromMemory(file, image) && image.width == 2 && image.height == 1 && image.rgb.size() == 6);
    if (image.rgb.size() == 6) {
        AETHER_CHECK(Near(image.rgb[0], 1.0f, 0.01f) && Near(image.rgb[3], 4.0f, 0.02f) && Near(image.rgb[4], 2.0f, 0.02f) && Near(image.rgb[5], 1.0f, 0.01f));
    }
    // Not a Radiance file, damaged, or over the pixel budget: refused, the output untouched.
    const std::vector<u8> png = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    AETHER_CHECK(!assets::IsHdrImage(png) && !assets::DecodeHdrImageFromMemory(png, image) && image.width == 2);
    std::vector<u8> truncated = file;
    truncated.resize(truncated.size() - 6);
    AETHER_CHECK(!assets::DecodeHdrImageFromMemory(truncated, image) || image.width == 2);
    AETHER_CHECK(!assets::DecodeHdrImageFromMemory(file, image, 1));
    AETHER_CHECK(!assets::DecodeHdrImageFromMemory({}, image));
}

AETHER_TEST(Ibl_FrameDataAndSceneShader) {
    static_assert(sizeof(player::FrameData) >= 1024); // the clustered lights' block follows (test_light_clusters.cpp)
    static_assert(offsetof(player::FrameData, sky_sh) == 864 && offsetof(player::FrameData, environment_info) == 1008);
    AETHER_CHECK(player::FrameData{}.environment_info[0] == 255);
    const std::string source = player::SceneShaderSource(true, true);
    AETHER_CHECK(source.find("float3 SkyLighting(") != std::string::npos && source.find("uint4 environment_info;") != std::string::npos);
    AETHER_CHECK(source.find("if (g_Frame.environment_info.x < 128)") != std::string::npos);
    // The frame data is declared once, by frame_lights (shared with the sky pass's shader).
    AETHER_CHECK(source.find("struct FrameData {") == source.rfind("struct FrameData {"));
    const std::string sky = player::SkyPass::ShaderSource();
    AETHER_CHECK(sky.find("struct FrameData {") != std::string::npos && sky.find("EnvironmentRadiance(direction, 0.0)") != std::string::npos);
    // Without frame data the shader keeps its flat ambient term.
    AETHER_CHECK(player::SceneShaderSource(false, false).find("SkyLighting") == std::string::npos ||
                 player::SceneShaderSource(false, false).find("#ifdef AETHER_FRAME_DATA") != std::string::npos);
}

AETHER_TEST(Ibl_SkyConstantsSpanTheView) {
    Camera camera;
    camera.fov_degrees = 90.0f;
    const View view = MakeView(camera, Mat4::Identity(), 2.0f);
    const player::SkyConstants c = player::SkyPass::MakeConstants(view);
    // Looking down -Z: the top-right corner's ray is forward + right * tan(45) * 2 + up * tan(45).
    AETHER_CHECK(Near(Vec3(c.forward[0], c.forward[1], c.forward[2]), Vec3(0, 0, -1), 1e-5f));
    AETHER_CHECK(Near(Vec3(c.right[0], c.right[1], c.right[2]), Vec3(2, 0, 0), 1e-4f) && Near(Vec3(c.up[0], c.up[1], c.up[2]), Vec3(0, 1, 0), 1e-4f));
}

static void Ibl_HalfFloatTextures(rhi::Backend backend) {
    std::unique_ptr<rhi::IDevice> device = MakeDevice(backend);
    if (!device) return;
    // 4x2 with three levels (4x2, 2x1, 1x1): red, green, blue.
    std::vector<u16> halves;
    const auto fill = [&](usize texels, f32 r, f32 g, f32 b) {
        for (usize i = 0; i < texels; ++i)
            for (const f32 v : {r, g, b, 1.0f}) halves.push_back(player::FloatToHalf(v));
    };
    fill(8, 1, 0, 0);
    fill(2, 0, 1, 0);
    fill(1, 0, 0, 1);
    const rhi::SampledTextureHandle texture = device->CreateHalfFloatTexture(4, 2, 3, halves.data());
    AETHER_CHECK(texture.IsValid());
    if (!texture.IsValid()) return;
    for (const auto& bad : {std::tuple<u32, u32, u32>{0, 2, 1}, {4, 2, 0}, {4, 2, 4}}) {
        bool threw = false;
        try {
            (void)device->CreateHalfFloatTexture(std::get<0>(bad), std::get<1>(bad), std::get<2>(bad), halves.data());
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        AETHER_CHECK(threw);
    }
    bool threw = false;
    try {
        (void)device->CreateHalfFloatTexture(4, 2, 1, nullptr);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    AETHER_CHECK(threw);
    std::unique_ptr<rhi::ICommandList> probe = device->CreateCommandList();
    probe->Reset();
    const rhi::ClearColor black{0, 0, 0, 1};
    AETHER_CHECK(!probe->BeginTexturePass(texture, &black) && !probe->BeginDepthPass(texture, true)); // sampled only
    probe->Close();

    // Each third of the screen samples one level.
    constexpr u32 kSize = 48;
    std::unique_ptr<rhi::ISwapChain> target = device->CreateSwapChain(nullptr, kSize, kSize, 2);
    rhi::PipelineDesc desc;
    desc.hlsl_source = R"(
struct PushConstants { uint texture_index; uint pad0; uint pad1; uint pad2; };
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
    float lod = floor(position.x / 16.0);
    return float4(g_Textures[g_PC.texture_index].SampleLevel(g_Sampler, float2(0.5, 0.5), lod).rgb, 1.0);
}
)";
    desc.push_constant_size_bytes = 16;
    desc.enable_bindless_textures = true;
    const rhi::PipelineHandle pipeline = device->CreatePipeline(desc, *target);
    std::unique_ptr<rhi::ICommandList> cmd = device->CreateCommandList();
    target->AcquireNextImage();
    cmd->Reset();
    cmd->BeginRenderPass(*target, {1, 1, 1, 1});
    cmd->BindPipeline(pipeline);
    cmd->BindBindlessTextures();
    const u32 push[4] = {texture.index, 0, 0, 0};
    cmd->SetPushConstants(push, sizeof(push));
    cmd->Draw(3);
    cmd->EndRenderPass();
    cmd->Close();
    device->WaitForFence(device->Submit(*cmd, target.get()));
    std::vector<u8> pixels;
    AETHER_CHECK(target->ReadBack(pixels) && pixels.size() == kSize * kSize * 4);
    target->Present(false);
    if (pixels.size() == kSize * kSize * 4) {
        const auto at = [&](u32 x, u32 c) { return pixels[(static_cast<usize>(kSize / 2) * kSize + x) * 4 + c]; };
        AETHER_CHECK(at(8, 0) > 240 && at(8, 1) < 16 && at(8, 2) < 16);
        AETHER_CHECK(at(24, 1) > 240 && at(24, 0) < 16 && at(24, 2) < 16);
        AETHER_CHECK(at(40, 2) > 240 && at(40, 0) < 16 && at(40, 1) < 16);
    }
    device->DestroyPipeline(pipeline);
    AETHER_CHECK(device->DestroyTexture(texture));
}

AETHER_TEST(Ibl_SkyPassOnD3D12AndVulkan) {
    for (const rhi::Backend backend : {rhi::Backend::D3D12, rhi::Backend::Vulkan}) {
        std::unique_ptr<rhi::IDevice> device = MakeDevice(backend);
        if (!device) continue;
        std::unique_ptr<rhi::ISwapChain> target = device->CreateSwapChain(nullptr, 32, 32, 2);
        player::SkyPass sky(*device, *target, true);
        AETHER_CHECK(sky.Valid() && sky.CanDrawBackground());
        player::FrameData data;
        sky.Fill(data);
        AETHER_CHECK(data.environment_info[0] == 255); // nothing yet
        AETHER_CHECK(sky.SetEnvironment("@0", player::EnvironmentImage{}) && sky.HasEnvironment() && sky.Key() == "@0");
        sky.Fill(data);
        AETHER_CHECK(data.environment_info[0] == sky.Texture().index && data.environment_info[1] == 6 && data.environment_info[2] == 128);
        AETHER_CHECK(data.sky_sh[0][2] > 0.0f && sky.Environment().halves.empty()); // the texels went to the GPU
        // A new environment replaces the old one's texture.
        AETHER_CHECK(sky.SetEnvironment("b@0", Uniform(64, Vec3(1, 1, 1))) && sky.Environment().width == 64);
    }
}

// The rendering checks above, on both backends (Vulkan has render targets since the parity work).
AETHER_TEST(Ibl_HalfFloatTextures_D3D12) { Ibl_HalfFloatTextures(rhi::Backend::D3D12); }
AETHER_TEST(Ibl_HalfFloatTextures_Vulkan) { Ibl_HalfFloatTextures(rhi::Backend::Vulkan); }
