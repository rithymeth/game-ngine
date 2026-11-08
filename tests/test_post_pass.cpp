#include "../player/src/post_pass.h"
#include "../player/src/decal_pass.h"
#include "../player/src/particle_pass.h"
#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"
#include "aether/renderer/post.h"
#include "test_framework.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

using namespace aether;

// Render roadmap step 1: the HDR scene colour target (ISwapChain::SceneColorTexture, ICommandList::BeginSceneColorPass) and the player's
// post pass, which tone maps it into the back buffer. The GPU tests draw known linear values into the target, run the real post pass and
// compare each pixel with PostReference, the CPU version built on renderer/post.h's tone mappers.

namespace {

bool Near(f32 a, f32 b, f32 eps) { return std::fabs(a - b) <= eps; }

std::unique_ptr<gfx::rhi::IDevice> MakeDevice(gfx::rhi::Backend backend) {
    std::unique_ptr<gfx::rhi::IDevice> device;
    if (gfx::rhi::IsBackendAvailable(backend)) {
        try {
            device = gfx::rhi::CreateDevice(backend, false);
        } catch (const std::exception&) {
        }
    }
    if (!device) std::printf("    (no %s device: skipping)\n", backend == gfx::rhi::Backend::Vulkan ? "Vulkan" : "D3D12");
    return device;
}

// Fills the HDR target with a horizontal ramp of grey values: column x holds 2^(x / (width - 1) * 14 - 8), from 1/256 up to 64, well past
// what an 8-bit target could hold. Grey, so the readback's channel order (RGBA or BGRA) doesn't matter.
constexpr char kRampShader[] = R"(
float4 VSMain(uint id : SV_VertexID) : SV_POSITION {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.5, 1.0);
}
float4 PSMain(float4 position : SV_POSITION) : SV_TARGET {
    float t = floor(position.x) / 63.0;
    return float4(exp2(t * 14.0 - 8.0).xxx, 1.0);
}
)";
f32 RampValue(u32 x) { return std::exp2(static_cast<f32>(x) / 63.0f * 14.0f - 8.0f); }

constexpr u32 kSize = 64;

// Draws the ramp into the scene colour target, then the post pass with `settings`; the back buffer in `pixels`.
bool RenderRamp(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& target, player::PostPass& post, gfx::rhi::PipelineHandle ramp,
                const PostProcessSettings& settings, std::vector<u8>& pixels) {
    std::unique_ptr<gfx::rhi::ICommandList> cmd = device.CreateCommandList();
    target.AcquireNextImage();
    cmd->Reset();
    if (!cmd->BeginSceneColorPass(target, {0.0f, 0.0f, 0.0f, 1.0f})) return false;
    cmd->BindPipeline(ramp);
    cmd->Draw(3);
    cmd->EndRenderPass();
    cmd->BeginRenderPass(target, {1.0f, 0.0f, 1.0f, 1.0f}); // magenta: any pixel the post pass misses shows
    post.Draw(*cmd, settings);
    cmd->EndRenderPass();
    cmd->Close();
    device.WaitForFence(device.Submit(*cmd, &target));
    const bool read = target.ReadBack(pixels);
    target.Present(false);
    return read && pixels.size() == kSize * kSize * 4;
}

// The largest difference (in 8-bit steps) between the GPU's green channel and PostReference over the middle row.
int WorstError(const std::vector<u8>& pixels, const player::PostConstants& constants) {
    int worst = 0;
    const u32 y = kSize / 2;
    for (u32 x = 0; x < kSize; ++x) {
        const f32 u = (static_cast<f32>(x) + 0.5f) / kSize, v = (static_cast<f32>(y) + 0.5f) / kSize;
        const f32 expected = player::PostReference(Vec3(RampValue(x), RampValue(x), RampValue(x)), constants, u, v).y * 255.0f;
        const int got = pixels[(static_cast<usize>(y) * kSize + x) * 4 + 1];
        worst = std::max(worst, std::abs(got - static_cast<int>(std::lround(expected))));
    }
    return worst;
}

void PostPass_TonemapsTheHdrTargetLikeTheCpuReference(gfx::rhi::Backend backend) {
    std::unique_ptr<gfx::rhi::IDevice> device = MakeDevice(backend);
    if (!device) return;
    std::unique_ptr<gfx::rhi::ISwapChain> target = device->CreateSwapChain(nullptr, kSize, kSize, 2);
    player::PostPass post(*device, *target);
    if (!post.Valid()) {
        std::printf("    (no HDR scene colour target on this device: the scene shader tone maps itself)\n");
        return;
    }
    gfx::rhi::PipelineDesc ramp_desc;
    ramp_desc.hlsl_source = kRampShader;
    ramp_desc.scene_color_target = true;
    const gfx::rhi::PipelineHandle ramp = device->CreatePipeline(ramp_desc, *target);
    AETHER_CHECK(ramp.IsValid());
    if (!ramp.IsValid()) return;

    struct Case {
        Tonemapper mapper;
        f32 compensation, vignette, saturation;
    };
    for (const Case& c : {Case{Tonemapper::ACES, 0.0f, 0.0f, 1.0f}, Case{Tonemapper::AgX, 0.0f, 0.0f, 1.0f},
                          Case{Tonemapper::ACES, 1.5f, 0.0f, 0.5f}, Case{Tonemapper::None, -2.0f, 0.0f, 1.0f},
                          Case{Tonemapper::ACES, 0.0f, 0.8f, 1.0f}}) {
        PostProcessSettings settings;
        settings.tonemapper = c.mapper;
        settings.exposure_compensation = c.compensation;
        settings.vignette = c.vignette;
        settings.saturation = c.saturation;
        std::vector<u8> pixels;
        AETHER_CHECK(RenderRamp(*device, *target, post, ramp, settings, pixels));
        if (pixels.size() != kSize * kSize * 4) return;
        const player::PostConstants constants = player::PackPostConstants(settings, post.SceneColorIndex());
        // Half-float storage and GPU transcendental precision: within two 8-bit steps everywhere.
        const int worst = WorstError(pixels, constants);
        if (worst > 2) std::printf("    tonemapper %d, compensation %.1f: worst error %d steps\n", static_cast<int>(c.mapper), c.compensation, worst);
        AETHER_CHECK(worst <= 2);
    }

    // The point of the target: values above 1 survive to the tone mapper. Under ACES the ramp keeps rising past linear 1 (column 36 is 1.0, 45 is 4)
    // up to 16 (column 54) instead of flattening at the clamp an 8-bit target would impose.
    PostProcessSettings aces;
    std::vector<u8> pixels;
    AETHER_CHECK(RenderRamp(*device, *target, post, ramp, aces, pixels));
    if (pixels.size() != kSize * kSize * 4) return;
    const auto green = [&](u32 x) { return static_cast<int>(pixels[(static_cast<usize>(kSize / 2) * kSize + x) * 4 + 1]); };
    AETHER_CHECK(green(54) > green(45) && green(45) > green(36));
    AETHER_CHECK(green(0) < 8 && green(63) > 250); // dark stays dark, very bright saturates to white
    device->DestroyPipeline(ramp);
}

} // namespace

AETHER_TEST(PostPass_ConstantsAre32BytesAndClampTheSettings) {
    PostProcessSettings s;
    s.exposure_compensation = 2.0f;
    s.saturation = 5.0f;
    s.vignette = -1.0f;
    s.tonemapper = Tonemapper::AgX;
    const player::PostConstants c = player::PackPostConstants(s, 7);
    AETHER_CHECK(c.scene_color == 7 && Near(c.exposure, 4.0f, 1e-6f) && c.tonemapper == 1);
    AETHER_CHECK(c.saturation == 2.0f && c.vignette == 0.0f);
    s.exposure_compensation = 40.0f; // clamped to the volume's own +-10 EV range
    AETHER_CHECK(Near(player::PackPostConstants(s, 7).exposure, 1024.0f, 1e-3f));
}

AETHER_TEST(PostPass_ReferenceIsTheRendererTonemapThenSrgb) {
    player::PostConstants c; // exposure 1, ACES, no grading
    const Vec3 hdr(3.0f, 0.4f, 0.05f);
    const Vec3 mapped = TonemapACES(hdr);
    const Vec3 got = player::PostReference(hdr, c, 0.5f, 0.5f);
    AETHER_CHECK(Near(got.x, LinearToSrgb(mapped.x), 1e-6f) && Near(got.y, LinearToSrgb(mapped.y), 1e-6f) &&
                 Near(got.z, LinearToSrgb(mapped.z), 1e-6f));
    // Exposure doubles the input; the vignette darkens corners only; saturation 0 is grey.
    c.exposure = 2.0f;
    AETHER_CHECK(Near(player::PostReference(Vec3(0.5f, 0.5f, 0.5f), c, 0.5f, 0.5f).x, LinearToSrgb(TonemapACES(Vec3(1, 1, 1)).x), 1e-6f));
    c.exposure = 1.0f;
    c.vignette = 1.0f;
    AETHER_CHECK(player::PostReference(hdr, c, 0.5f, 0.5f).x == got.x);
    AETHER_CHECK(player::PostReference(hdr, c, 0.0f, 0.0f).x < 1e-6f);
    c.vignette = 0.0f;
    c.saturation = 0.0f;
    const Vec3 grey = player::PostReference(hdr, c, 0.5f, 0.5f);
    AETHER_CHECK(Near(grey.x, grey.y, 1e-5f) && Near(grey.y, grey.z, 1e-5f));
}

AETHER_TEST(PostPass_ShaderCompilesWithGlslang) {
    if (std::system("glslangValidator --version > /dev/null 2>&1") != 0) {
        std::printf("  (glslangValidator not found: skipping the HLSL compile check)\n");
        return;
    }
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_post_shader";
    std::filesystem::create_directories(dir);
    const std::filesystem::path src = dir / "post.hlsl", log = dir / "log.txt";
    {
        std::ofstream f(src);
        f << "#define __spirv__ 1\n" << player::PostShaderSource();
    }
    for (const auto& stage : {std::pair<const char*, const char*>{"vert", "VSMain"}, {"frag", "PSMain"}}) {
        const std::string cmd = std::string("glslangValidator -D -V -S ") + stage.first + " -e " + stage.second + " \"" + src.string() +
                                "\" -o \"" + (dir / "out.spv").string() + "\" > \"" + log.string() + "\" 2>&1";
        const bool ok = std::system(cmd.c_str()) == 0;
        if (!ok) {
            std::ifstream l(log);
            std::printf("%s\n", std::string((std::istreambuf_iterator<char>(l)), std::istreambuf_iterator<char>()).c_str());
        }
        AETHER_CHECK(ok);
    }
    std::filesystem::remove_all(dir);
}

AETHER_TEST(PostPass_TonemapsTheHdrTargetLikeTheCpuReferenceOnD3D12) {
    PostPass_TonemapsTheHdrTargetLikeTheCpuReference(gfx::rhi::Backend::D3D12);
}
AETHER_TEST(PostPass_TonemapsTheHdrTargetLikeTheCpuReferenceOnVulkan) {
    PostPass_TonemapsTheHdrTargetLikeTheCpuReference(gfx::rhi::Backend::Vulkan);
}

AETHER_TEST(PostPass_SceneColorPassIsRefusedWithoutATarget) {
    std::unique_ptr<gfx::rhi::IDevice> device = MakeDevice(gfx::rhi::Backend::D3D12);
    if (!device) return;
    std::unique_ptr<gfx::rhi::ISwapChain> target = device->CreateSwapChain(nullptr, kSize, kSize, 2);
    std::unique_ptr<gfx::rhi::ICommandList> cmd = device->CreateCommandList();
    cmd->Reset();
    // No SceneColorTexture yet: refused, nothing recorded, and a normal pass still works afterwards.
    AETHER_CHECK(!cmd->BeginSceneColorPass(*target, {0.0f, 0.0f, 0.0f, 1.0f}));
    cmd->BeginRenderPass(*target, {0.0f, 0.0f, 0.0f, 1.0f});
    // Nested: refused even once the target exists.
    AETHER_CHECK(target->SceneColorTexture().IsValid());
    AETHER_CHECK(!cmd->BeginSceneColorPass(*target, {0.0f, 0.0f, 0.0f, 1.0f}));
    cmd->EndRenderPass();
    cmd->Close();
    device->WaitForFence(device->Submit(*cmd, target.get()));
    // The handle is stable across a resize.
    const u32 index = target->SceneColorTexture().index;
    target->Resize(kSize * 2, kSize);
    AETHER_CHECK(target->SceneColorTexture().index == index);
}

AETHER_TEST(PostPass_ParticleAndDecalPassesBuildHdrPipelines) {
    std::unique_ptr<gfx::rhi::IDevice> device = MakeDevice(gfx::rhi::Backend::D3D12);
    if (!device) return;
    std::unique_ptr<gfx::rhi::ISwapChain> target = device->CreateSwapChain(nullptr, kSize, kSize, 2);
    if (!target->SceneColorTexture().IsValid()) return;
    player::ParticlePass particles(*device, *target, /*hdr_target=*/true);
    AETHER_CHECK(particles.Valid());
    player::DecalPass decals(*device, *target, /*hdr_target=*/true);
    AETHER_CHECK(decals.HdrTarget() && (decals.Valid() || !target->SceneDepthTexture().IsValid()));
}
