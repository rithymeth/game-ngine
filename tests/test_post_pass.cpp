#include "../player/src/post_pass.h"
#include "../player/src/decal_pass.h"
#include "../player/src/environment.h"
#include "../player/src/particle_pass.h"
#include "aether/assets/png_writer.h"
#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"
#include "aether/reflection/serialize.h"
#include "aether/renderer/components.h"
#include "aether/renderer/post.h"
#include "aether/scene/gameplay.h"
#include "test_framework.h"

#include <algorithm>
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

// A step edge: grey 0.2 before column kStepColumn, 1.0 from it on (the lens effects that read neighbouring pixels have something to do there).
constexpr u32 kStepColumn = 10;
constexpr char kStepShader[] = R"(
float4 VSMain(uint id : SV_VertexID) : SV_POSITION {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.5, 1.0);
}
float4 PSMain(float4 position : SV_POSITION) : SV_TARGET {
    return float4((floor(position.x) < 10.0 ? 0.2 : 1.0).xxx, 1.0);
}
)";

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
    AETHER_CHECK(c.scene_color == 7 && Near(c.exposure, 4.0f, 1e-6f) && player::PostTonemapperOf(c) == 1 && player::PostFrameOf(c) == 0);
    AETHER_CHECK(c.saturation == 2.0f && player::LensVignette(c.lens) == 0.0f);
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
    c.lens = player::PackLens(1.0f, 0.0f, 0.0f, 0.0f);
    AETHER_CHECK(player::PostReference(hdr, c, 0.5f, 0.5f).x == got.x);
    AETHER_CHECK(player::PostReference(hdr, c, 0.0f, 0.0f).x < 1e-6f);
    c.lens = 0;
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

static void PostPass_SceneColorPassIsRefusedWithoutATarget(gfx::rhi::Backend backend) {
    std::unique_ptr<gfx::rhi::IDevice> device = MakeDevice(backend);
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

static void PostPass_ParticleAndDecalPassesBuildHdrPipelines(gfx::rhi::Backend backend) {
    std::unique_ptr<gfx::rhi::IDevice> device = MakeDevice(backend);
    if (!device) return;
    std::unique_ptr<gfx::rhi::ISwapChain> target = device->CreateSwapChain(nullptr, kSize, kSize, 2);
    if (!target->SceneColorTexture().IsValid()) return;
    player::ParticlePass particles(*device, *target, /*hdr_target=*/true);
    AETHER_CHECK(particles.Valid());
    player::DecalPass decals(*device, *target, /*hdr_target=*/true);
    AETHER_CHECK(decals.HdrTarget() && (decals.Valid() || !target->SceneDepthTexture().IsValid()));
}

// The rendering checks above, on both backends (Vulkan has render targets since the parity work).
AETHER_TEST(PostPass_SceneColorPassIsRefusedWithoutATarget_D3D12) { PostPass_SceneColorPassIsRefusedWithoutATarget(gfx::rhi::Backend::D3D12); }
AETHER_TEST(PostPass_SceneColorPassIsRefusedWithoutATarget_Vulkan) { PostPass_SceneColorPassIsRefusedWithoutATarget(gfx::rhi::Backend::Vulkan); }
AETHER_TEST(PostPass_ParticleAndDecalPassesBuildHdrPipelines_D3D12) { PostPass_ParticleAndDecalPassesBuildHdrPipelines(gfx::rhi::Backend::D3D12); }
AETHER_TEST(PostPass_ParticleAndDecalPassesBuildHdrPipelines_Vulkan) { PostPass_ParticleAndDecalPassesBuildHdrPipelines(gfx::rhi::Backend::Vulkan); }

// Height fog is a post volume setting like the distance haze: it blends by weight, and a bounded volume gives it only inside.
AETHER_TEST(PostVolume_HeightFogBlendsLikeTheOtherSettings) {
    RenderScene scene;
    PostProcessVolume base;
    base.global = true;
    base.height_fog_density = 0.04f;
    base.height_fog_falloff = 0.1f;
    base.height_fog_base = 10.0f;
    scene.post_volumes.push_back({Vec3(0, 0, 0), base});
    const PostProcessSettings all = BlendPostProcess(scene, Vec3(500, 0, 0));
    const auto near = [](f32 a, f32 b) { return std::fabs(a - b) < 1e-5f; };
    AETHER_CHECK(near(all.height_fog_density, 0.04f) && near(all.height_fog_falloff, 0.1f) && near(all.height_fog_base, 10.0f));
    PostProcessVolume local;
    local.global = false;
    local.extents = Vec3(5, 5, 5);
    local.blend_distance = 0.0f;
    local.height_fog_density = 0.2f;
    local.height_fog_falloff = 0.3f;
    local.height_fog_base = 30.0f;
    local.priority = 1;
    scene.post_volumes.push_back({Vec3(0, 0, 0), local});
    const PostProcessSettings inside = BlendPostProcess(scene, Vec3(0, 0, 0)), outside = BlendPostProcess(scene, Vec3(500, 0, 0));
    AETHER_CHECK(near(inside.height_fog_density, 0.2f) && near(inside.height_fog_base, 30.0f));
    AETHER_CHECK(near(outside.height_fog_density, 0.04f) && near(outside.height_fog_base, 10.0f));
    // A plain default volume has no height fog.
    AETHER_CHECK(PostProcessVolume{}.height_fog_density == 0.0f);
}

// --- Lens effects: chromatic aberration, film grain and sharpen --------------------------------------------------------------------

AETHER_TEST(PostPass_LensWordPacksFourStrengthsAndTheFrame) {
    // Each strength is clamped to 0..1, kept in 8 bits and read back in its own byte; what is not a number counts as 0.
    const u32 lens = player::PackLens(0.5f, 1.0f, 0.0f, 0.2f);
    AETHER_CHECK(Near(player::LensVignette(lens), 128.0f / 255.0f, 1e-6f) && player::LensChromaticAberration(lens) == 1.0f);
    AETHER_CHECK(player::LensFilmGrain(lens) == 0.0f && Near(player::LensSharpen(lens), 51.0f / 255.0f, 1e-6f));
    AETHER_CHECK(player::PackLens(-4.0f, 9.0f, std::nanf(""), INFINITY) == (0u | (255u << 8) | (0u << 16) | (0u << 24)));
    AETHER_CHECK(player::PackLens(0.0f, 0.0f, 0.0f, 0.0f) == 0u && player::PackLens(1.0f, 1.0f, 1.0f, 1.0f) == 0xffffffffu);
    // The tone mapper keeps its 2 bits, the reflections take 8 and the frame the other 22.
    player::PostConstants c;
    c.tonemapper = player::PackTonemapperFrame(2, 123456u, 200u);
    AETHER_CHECK(player::PostTonemapperOf(c) == 2 && player::PostScreenSpaceReflectionsOf(c) == 200u && player::PostFrameOf(c) == 123456u);
    // The pack carries the lens settings into the word, and the constants are still the 128 bytes every device guarantees.
    static_assert(sizeof(player::PostConstants) == 128);
    PostProcessSettings s;
    s.vignette = 0.25f;
    s.chromatic_aberration = 0.5f;
    s.film_grain = 0.75f;
    s.sharpen = 1.0f;
    const player::PostConstants packed = player::PackPostConstants(s, 3);
    AETHER_CHECK(Near(player::LensVignette(packed.lens), 0.25f, 1.0f / 255.0f) && Near(player::LensChromaticAberration(packed.lens), 0.5f, 1.0f / 255.0f) &&
                 Near(player::LensFilmGrain(packed.lens), 0.75f, 1.0f / 255.0f) && player::LensSharpen(packed.lens) == 1.0f);
    AETHER_CHECK(player::PackPostConstants(PostProcessSettings{}, 3).lens == 0u); // a default volume has no lens effects
}

AETHER_TEST(PostVolume_LensEffectsBlendAndSerialize) {
    const auto near = [](f32 a, f32 b) { return std::fabs(a - b) < 1e-5f; };
    // Off by default, in the volume and in what a scene with no volume gets.
    AETHER_CHECK(PostProcessVolume{}.chromatic_aberration == 0.0f && PostProcessVolume{}.film_grain == 0.0f && PostProcessVolume{}.sharpen == 0.0f);
    RenderScene scene;
    const PostProcessSettings none = BlendPostProcess(scene, Vec3(0, 0, 0));
    AETHER_CHECK(none.chromatic_aberration == 0.0f && none.film_grain == 0.0f && none.sharpen == 0.0f);
    // They blend like the other settings: a global volume everywhere, a bounded one over it inside its box.
    PostProcessVolume base;
    base.global = true;
    base.chromatic_aberration = 0.2f;
    base.film_grain = 0.4f;
    base.sharpen = 0.6f;
    scene.post_volumes.push_back({Vec3(0, 0, 0), base});
    PostProcessVolume local;
    local.global = false;
    local.extents = Vec3(5, 5, 5);
    local.blend_distance = 0.0f;
    local.chromatic_aberration = 1.0f;
    local.film_grain = 0.0f;
    local.sharpen = 0.1f;
    local.priority = 1;
    scene.post_volumes.push_back({Vec3(0, 0, 0), local});
    const PostProcessSettings inside = BlendPostProcess(scene, Vec3(0, 0, 0)), outside = BlendPostProcess(scene, Vec3(500, 0, 0));
    AETHER_CHECK(near(inside.chromatic_aberration, 1.0f) && near(inside.film_grain, 0.0f) && near(inside.sharpen, 0.1f));
    AETHER_CHECK(near(outside.chromatic_aberration, 0.2f) && near(outside.film_grain, 0.4f) && near(outside.sharpen, 0.6f));
    // Half weight is the midpoint.
    scene.post_volumes[1].settings.weight = 0.5f;
    AETHER_CHECK(near(BlendPostProcess(scene, Vec3(0, 0, 0)).chromatic_aberration, 0.6f));
    // A saved volume round-trips, and one saved before these fields existed loads with them off.
    const reflect::Json saved = reflect::ToJson(base);
    PostProcessVolume back;
    AETHER_CHECK(reflect::FromJson(back, saved) && near(back.chromatic_aberration, 0.2f) && near(back.film_grain, 0.4f) && near(back.sharpen, 0.6f));
    reflect::Json old = saved;
    old.erase("chromatic_aberration");
    old.erase("film_grain");
    old.erase("sharpen");
    PostProcessVolume loaded;
    loaded.film_grain = 0.9f; // not what the file says: the default is
    AETHER_CHECK(reflect::FromJson(loaded, old) && loaded.chromatic_aberration == 0.0f && loaded.sharpen == 0.0f);
}


namespace {

// The ramp (greys) through the post pass `frames` times with `settings`, the last frame's back buffer in `pixels`.
bool RenderLensFrames(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& target, player::PostPass& post, gfx::rhi::PipelineHandle ramp,
                      const PostProcessSettings& settings, int frames, std::vector<u8>& pixels) {
    for (int i = 0; i < frames; ++i) {
        if (!RenderRamp(device, target, post, ramp, settings, pixels)) return false;
    }
    return true;
}

int PixelChannel(const std::vector<u8>& pixels, u32 x, u32 y, u32 channel) { return pixels[(static_cast<usize>(y) * kSize + x) * 4 + channel]; }

void PostPass_LensEffectsMatchTheCpuReference(gfx::rhi::Backend backend) {
    std::unique_ptr<gfx::rhi::IDevice> device = MakeDevice(backend);
    if (!device) return;
    std::unique_ptr<gfx::rhi::ISwapChain> target = device->CreateSwapChain(nullptr, kSize, kSize, 2);
    player::PostPass post(*device, *target);
    if (!post.Valid()) {
        std::printf("    (no HDR scene colour target on this device)\n");
        return;
    }
    gfx::rhi::PipelineDesc ramp_desc;
    ramp_desc.hlsl_source = kRampShader;
    ramp_desc.scene_color_target = true;
    const gfx::rhi::PipelineHandle ramp = device->CreatePipeline(ramp_desc, *target);
    AETHER_CHECK(ramp.IsValid());
    if (!ramp.IsValid()) return;
    const auto grey = [](f32 v) { return Vec3(v, v, v); };

    // Film grain: the shader's noise is the CPU's, pixel for pixel, and a new frame brings new noise. The first Draw of a pass is frame 0.
    PostProcessSettings grain;
    grain.film_grain = 1.0f;
    std::vector<u8> first, second;
    AETHER_CHECK(RenderLensFrames(*device, *target, post, ramp, grain, 1, first));
    AETHER_CHECK(RenderLensFrames(*device, *target, post, ramp, grain, 1, second));
    if (first.size() != kSize * kSize * 4 || second.size() != first.size()) return;
    for (u32 frame = 0; frame < 2; ++frame) {
        player::PostConstants constants = player::PackPostConstants(grain, post.SceneColorIndex());
        constants.tonemapper = player::PackTonemapperFrame(player::PostTonemapperOf(constants), frame);
        int worst = 0;
        for (u32 y = 0; y < kSize; y += 7) {
            for (u32 x = 0; x < kSize; ++x) {
                const f32 u = (static_cast<f32>(x) + 0.5f) / kSize, v = (static_cast<f32>(y) + 0.5f) / kSize;
                const f32 expected = player::PostReferenceWithGrain(grey(RampValue(x)), constants, u, v, x, y).y * 255.0f;
                const int got = PixelChannel(frame == 0 ? first : second, x, y, 1);
                worst = std::max(worst, std::abs(got - static_cast<int>(std::lround(expected))));
            }
        }
        if (worst > 2) std::printf("    grain, frame %u: worst error %d steps\n", frame, worst);
        AETHER_CHECK(worst <= 2);
    }
    int moved = 0;
    for (u32 x = 0; x < kSize; ++x) moved += std::abs(PixelChannel(first, x, 20, 1) - PixelChannel(second, x, 20, 1)) > 3;
    AETHER_CHECK(moved > 10); // the grain is not the same on the next frame
    // The grain is monochrome: all three channels move together.
    for (u32 x = 0; x < kSize; x += 5) AETHER_CHECK(std::abs(PixelChannel(first, x, 20, 0) - PixelChannel(first, x, 20, 1)) <= 1 && std::abs(PixelChannel(first, x, 20, 2) - PixelChannel(first, x, 20, 1)) <= 1);

    // The effects that read neighbouring pixels are checked on an edge, where they have something to do: grey 0.2 up to column 9, 1.0 from column 10.
    gfx::rhi::PipelineDesc step_desc;
    step_desc.hlsl_source = kStepShader;
    step_desc.scene_color_target = true;
    const gfx::rhi::PipelineHandle step = device->CreatePipeline(step_desc, *target);
    AETHER_CHECK(step.IsValid());
    if (!step.IsValid()) return;
    const auto step_value = [](u32 x) { return x < kStepColumn ? 0.2f : 1.0f; };
    const auto clamp_column = [](int c) { return static_cast<u32>(std::clamp(c, 0, static_cast<int>(kSize) - 1)); };
    std::vector<u8> pixels;

    // Chromatic aberration: red is read from one side of the pixel and blue from the other, by an amount that grows toward the edges of the frame,
    // so the edge near the left of the frame grows a red and a blue fringe; the middle of the frame is left alone.
    PostProcessSettings aberration;
    aberration.chromatic_aberration = 1.0f;
    const player::PostConstants plain = player::PackPostConstants(aberration, post.SceneColorIndex());
    for (const bool use_step : {false, true}) {
        AETHER_CHECK(RenderRamp(*device, *target, post, use_step ? step : ramp, aberration, pixels));
        if (pixels.size() != kSize * kSize * 4) return;
        const auto value = [&](u32 x) { return use_step ? step_value(x) : RampValue(x); };
        int worst = 0, fringes = 0;
        for (u32 x = 0; x < kSize; ++x) {
            const f32 uv_x = (static_cast<f32>(x) + 0.5f) / kSize;
            const int shift = static_cast<int>(std::lround(ChromaticOffset(uv_x, 0.5f, 1.0f).x * kSize));
            // The colour the pass tone maps is red from one pixel, green from this one and blue from the other side (the tone mapper mixes the
            // channels, so the three can not be mapped apart).
            const Vec3 sampled(value(clamp_column(static_cast<int>(x) + shift)), value(x), value(clamp_column(static_cast<int>(x) - shift)));
            const Vec3 want = player::PostReference(sampled, plain, 0.5f, 0.5f);
            worst = std::max({worst, std::abs(PixelChannel(pixels, x, 32, 0) - static_cast<int>(std::lround(want.x * 255.0f))),
                              std::abs(PixelChannel(pixels, x, 32, 1) - static_cast<int>(std::lround(want.y * 255.0f))),
                              std::abs(PixelChannel(pixels, x, 32, 2) - static_cast<int>(std::lround(want.z * 255.0f)))});
            fringes += std::abs(PixelChannel(pixels, x, 32, 0) - PixelChannel(pixels, x, 32, 2)) > 40;
        }
        if (worst > 2) std::printf("    chromatic aberration (%s): worst error %d steps\n", use_step ? "edge" : "ramp", worst);
        AETHER_CHECK(worst <= 2);
        if (use_step) AETHER_CHECK(fringes == 2); // the two pixels beside the edge: red and blue are on opposite sides of it
    }
    // Away from the edge a flat area of the step image has equal channels, however far the channels are shifted.
    AETHER_CHECK(std::abs(PixelChannel(pixels, kSize - 8, 32, 0) - PixelChannel(pixels, kSize - 8, 32, 2)) <= 1);

    // Sharpen: the luminance moves away from its four neighbours' average, so the dark side of the edge goes darker and the bright side brighter,
    // and the flat areas stay as they were.
    PostProcessSettings sharpen;
    sharpen.sharpen = 0.6f;
    const player::PostConstants sharp_constants = player::PackPostConstants(sharpen, post.SceneColorIndex());
    for (const bool use_step : {false, true}) {
        AETHER_CHECK(RenderRamp(*device, *target, post, use_step ? step : ramp, sharpen, pixels));
        if (pixels.size() != kSize * kSize * 4) return;
        const auto value = [&](u32 x) { return use_step ? step_value(x) : RampValue(x); };
        int worst = 0, changed = 0;
        for (u32 x = 0; x < kSize; ++x) {
            const f32 l = value(x), left = value(clamp_column(static_cast<int>(x) - 1)), right = value(clamp_column(static_cast<int>(x) + 1));
            const f32 neighbours = (left + right + l + l) * 0.25f;
            const f32 sharpened = SharpenLuminance(l, neighbours, player::LensSharpen(sharp_constants.lens));
            const f32 expected = player::PostReference(grey(sharpened), sharp_constants, 0.5f, 0.5f).y * 255.0f;
            const f32 unsharpened = player::PostReference(grey(l), sharp_constants, 0.5f, 0.5f).y * 255.0f;
            worst = std::max(worst, std::abs(PixelChannel(pixels, x, 32, 1) - static_cast<int>(std::lround(expected))));
            changed += std::fabs(expected - unsharpened) > 10.0f;
        }
        if (worst > 3) std::printf("    sharpen (%s): worst error %d steps\n", use_step ? "edge" : "ramp", worst);
        AETHER_CHECK(worst <= 3);
        if (use_step) {
            AETHER_CHECK(changed == 2); // the reference moves exactly the two pixels beside the edge by a lot, so the match above means something
            AETHER_CHECK(PixelChannel(pixels, kStepColumn - 1, 32, 1) < PixelChannel(pixels, kStepColumn - 3, 32, 1)); // darker beside the edge than away from it
            AETHER_CHECK(PixelChannel(pixels, kStepColumn, 32, 1) > PixelChannel(pixels, kStepColumn + 3, 32, 1));      // brighter beside it
        }
    }
}

// --- Screen-space reflections on the GPU ------------------------------------------------------------------------------------------

// The reflections' synthetic scene. The camera is at the origin looking down -Z with a 60 degree vertical field (an identity view), over a
// floor at y = -1 and a wall at z = -20 behind it. Each pixel shows floor or wall, whichever its eye ray reaches first; the wall's green rises
// with height and the floor is flat, so a floor pixel's mirror ray (up and forward, into the wall) lands on a colour the test works out
// analytically. The scene colour is an RGBA16F texture (its alpha is the surface's roughness, as the scene shader writes it) and the depth an
// R32F one, both sampled by the pass.
constexpr f32 kSsrProjection = 1.7320508f; // 1 / tan(30 degrees): the projection's [1][1], and [0][0] on a square target
constexpr f32 kSsrNear = 0.1f, kSsrFar = 100.0f, kSsrWallDepth = 20.0f;
const Vec3 kSsrFloorColor{0.2f, 0.3f, 0.3f};

f32 SsrWallGreen(f32 height) { return 0.05f + 1.5f * std::clamp((height + 1.0f) / 10.0f, 0.0f, 1.0f); }

// The eye ray of a pixel centre, in view space (unit length).
Vec3 SsrEyeRay(u32 x, u32 y) {
    const f32 ndc_x = (static_cast<f32>(x) + 0.5f) / kSize * 2.0f - 1.0f;
    const f32 ndc_y = 1.0f - (static_cast<f32>(y) + 0.5f) / kSize * 2.0f;
    const f32 x_slope = ndc_x / kSsrProjection, y_slope = ndc_y / kSsrProjection;
    const f32 inverse = 1.0f / std::sqrt(x_slope * x_slope + y_slope * y_slope + 1.0f);
    return Vec3(x_slope * inverse, y_slope * inverse, -inverse);
}

struct SsrScenePixel {
    f32 view_z = 1e30f; // metres to the surface
    Vec3 color{0, 0, 0};
    f32 roughness = 1.0f;
};

SsrScenePixel SsrPixel(u32 x, u32 y) {
    const Vec3 ray = SsrEyeRay(x, y);
    const f32 floor_t = ray.y < 0.0f ? -1.0f / ray.y : 1e30f; // the floor is y = -1
    const f32 wall_t = -kSsrWallDepth / ray.z;                // the wall is z = -kSsrWallDepth
    SsrScenePixel pixel;
    if (floor_t < wall_t) {
        pixel.view_z = floor_t;
        pixel.color = kSsrFloorColor;
        pixel.roughness = 0.0f; // a mirror: its reflection must be replaced
    } else {
        pixel.view_z = wall_t;
        pixel.color = Vec3(0.2f, SsrWallGreen(ray.y * wall_t), 0.3f);
        pixel.roughness = 1.0f; // rough: nothing reflects
    }
    return pixel;
}

f32 SsrDeviceDepth(f32 view_z) { return kSsrFar * (1.0f - kSsrNear / view_z) / (kSsrFar - kSsrNear); }

void PostPass_ReflectionsLandWhereTheMirrorRayMeetsTheScene(gfx::rhi::Backend backend) {
    std::unique_ptr<gfx::rhi::IDevice> device = MakeDevice(backend);
    if (!device) return;
    std::unique_ptr<gfx::rhi::ISwapChain> target = device->CreateSwapChain(nullptr, kSize, kSize, 2);
    player::PostPass post(*device, *target);
    if (!post.Valid()) {
        std::printf("    (no HDR scene colour target on this device)\n");
        return;
    }
    std::vector<u16> halves;
    std::vector<f32> depths;
    halves.reserve(kSize * kSize * 4);
    depths.reserve(kSize * kSize);
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            const SsrScenePixel pixel = SsrPixel(x, y);
            halves.insert(halves.end(), {player::FloatToHalf(pixel.color.x), player::FloatToHalf(pixel.color.y),
                                         player::FloatToHalf(pixel.color.z), player::FloatToHalf(pixel.roughness)});
            depths.push_back(SsrDeviceDepth(pixel.view_z));
        }
    }
    const gfx::rhi::SampledTextureHandle scene = device->CreateHalfFloatTexture(kSize, kSize, 1, halves.data());
    const gfx::rhi::SampledTextureHandle depth = device->CreateR32FloatTexture(kSize, kSize, depths.data());
    if (!scene.IsValid() || !depth.IsValid()) {
        std::printf("    (no half-float or R32F texture on this device: skipping)\n");
        if (scene.IsValid()) device->DestroyTexture(scene);
        if (depth.IsValid()) device->DestroyTexture(depth);
        return;
    }
    const u32 px = 32, py = 48; // the floor
    const u32 wall_px = 32, wall_py = 10;
    const SsrScenePixel floor_pixel = SsrPixel(px, py), wall_pixel = SsrPixel(wall_px, wall_py);
    AETHER_CHECK(floor_pixel.roughness == 0.0f && wall_pixel.roughness == 1.0f);
    // The floor pixel's mirror ray and where it meets the wall: the colour the march must find.
    const Vec3 ray = SsrEyeRay(px, py);
    const Vec3 point = ray * floor_pixel.view_z;
    const Vec3 mirror(ray.x, -ray.y, ray.z); // reflect about the floor's normal (0, 1, 0)
    const f32 to_wall = (-kSsrWallDepth - point.z) / mirror.z;
    const f32 hit_height = point.y + mirror.y * to_wall;
    AETHER_CHECK(to_wall > 0.0f && mirror.z < 0.0f && hit_height > -1.0f && hit_height < 10.0f); // the hit is on the wall
    const Vec3 hit(0.2f, SsrWallGreen(hit_height), 0.3f);
    const f32 facing = -ray.y; // the floor's normal against the eye ray
    const f32 weight = std::min(0.04f + 0.96f * std::pow(1.0f - facing, 5.0f), 0.9f); // strength 1, smooth, away from the edges
    const Vec3 blended(floor_pixel.color.x + (hit.x - floor_pixel.color.x) * weight,
                       floor_pixel.color.y + (hit.y - floor_pixel.color.y) * weight,
                       floor_pixel.color.z + (hit.z - floor_pixel.color.z) * weight);
    const f32 u = (static_cast<f32>(px) + 0.5f) / kSize, v = (static_cast<f32>(py) + 0.5f) / kSize;
    const PostProcessSettings reflections = [] {
        PostProcessSettings settings;
        settings.screen_space_reflections = 1.0f;
        return settings;
    }();
    const PostProcessSettings off = [] {
        PostProcessSettings settings;
        settings.screen_space_reflections = 0.0f;
        return settings;
    }();
    const player::PostConstants constants = player::PackPostConstants(reflections, scene.index);
    const auto steps = [&](const Vec3& linear) { return static_cast<int>(std::lround(player::PostReference(linear, constants, u, v).y * 255.0f)); };
    const int expected = steps(blended), unreflected = steps(floor_pixel.color);
    const int wall_expected = static_cast<int>(std::lround(
        player::PostReference(wall_pixel.color, constants, (static_cast<f32>(wall_px) + 0.5f) / kSize, (static_cast<f32>(wall_py) + 0.5f) / kSize).y *
        255.0f));

    std::unique_ptr<gfx::rhi::ICommandList> cmd = device->CreateCommandList();
    const auto render = [&](const PostProcessSettings& settings, std::vector<u8>& pixels) {
        post.SetSource(scene.index);
        // The pass takes its depth, projection and near/far from the occlusion constants (its strength 0: only the reflections read the depth).
        post.SetAmbientOcclusion({depth.index, 0.0f, 1.0f, kSsrNear, kSsrFar, kSsrProjection, kSsrProjection});
        target->AcquireNextImage();
        cmd->Reset();
        cmd->BeginRenderPass(*target, {1.0f, 0.0f, 1.0f, 1.0f}); // magenta: any pixel the pass misses shows
        post.Draw(*cmd, settings);
        cmd->EndRenderPass();
        cmd->Close();
        device->WaitForFence(device->Submit(*cmd, target.get()));
        const bool read = target->ReadBack(pixels);
        target->Present(false);
        return read && pixels.size() == kSize * kSize * 4;
    };
    std::vector<u8> reflected, ignored;
    AETHER_CHECK(render(reflections, reflected));
    AETHER_CHECK(render(off, ignored));
    if (reflected.size() != kSize * kSize * 4 || ignored.size() != reflected.size()) return;
    // The mirror floor shows the wall; with the reflections off (and on a rough surface) it keeps its own colour.
    const int got = PixelChannel(reflected, px, py, 1);
    if (std::abs(got - expected) > 4) std::printf("    reflection: green %d, expected %d (unreflected %d)\n", got, expected, unreflected);
    AETHER_CHECK(std::abs(expected - unreflected) > 6); // the check means something: the reflection moves the pixel
    AETHER_CHECK(std::abs(got - expected) <= 4);
    AETHER_CHECK(std::abs(PixelChannel(ignored, px, py, 1) - unreflected) <= 2);
    AETHER_CHECK(std::abs(PixelChannel(reflected, wall_px, wall_py, 1) - wall_expected) <= 2); // the rough wall reflects nothing
    device->DestroyTexture(scene);
    device->DestroyTexture(depth);
}

} // namespace

AETHER_TEST(PostPass_LensEffectsMatchTheCpuReferenceOnD3D12) { PostPass_LensEffectsMatchTheCpuReference(gfx::rhi::Backend::D3D12); }
AETHER_TEST(PostPass_LensEffectsMatchTheCpuReferenceOnVulkan) { PostPass_LensEffectsMatchTheCpuReference(gfx::rhi::Backend::Vulkan); }
AETHER_TEST(PostPass_ReflectionsLandWhereTheMirrorRayMeetsTheSceneOnD3D12) {
    PostPass_ReflectionsLandWhereTheMirrorRayMeetsTheScene(gfx::rhi::Backend::D3D12);
}
AETHER_TEST(PostPass_ReflectionsLandWhereTheMirrorRayMeetsTheSceneOnVulkan) {
    PostPass_ReflectionsLandWhereTheMirrorRayMeetsTheScene(gfx::rhi::Backend::Vulkan);
}

// --- Screen-space reflections: settings and packing ------------------------------------------------------------------------------
// --- Screen-space reflections ----------------------------------------------------------------------------------------------------

AETHER_TEST(PostPass_DepthOfFieldUsesThePhysicalLens) {
    CineCamera lens;
    lens.focal_length_mm = 50.0f;
    lens.sensor_height_mm = 24.0f;
    lens.aperture_f = 2.0f;
    lens.focus_distance = 10.0f;
    const auto packed = player::PackDepthOfField(lens, 1080, 0.1f, 100.0f);
    const f32 scale = 1080.0f * 0.05f * 0.05f / (2.0f * 2.0f * 9.95f * 0.024f);
    AETHER_CHECK(Near(packed.radius_scale, scale, 1e-5f));
    AETHER_CHECK(player::DepthOfFieldRadius(packed, 10.0f) == 0.0f);
    AETHER_CHECK(Near(player::DepthOfFieldRadius(packed, 5.0f), -scale, 1e-5f));
    AETHER_CHECK(Near(player::DepthOfFieldRadius(packed, 20.0f), scale * 0.5f, 1e-5f));
    AETHER_CHECK(player::DepthOfFieldRadius(packed, 0.1f) == -packed.max_radius);
    AETHER_CHECK(Near(player::PackDepthOfField(lens, 2160, 0.1f, 100.0f).radius_scale, scale * 2.0f, 1e-5f));
    lens.aperture_f *= 2.0f;
    AETHER_CHECK(Near(player::PackDepthOfField(lens, 1080, 0.1f, 100.0f).radius_scale, scale * 0.5f, 1e-5f));
    for (const f32 invalid : {0.0f, -1.0f, std::nanf(""), HUGE_VALF}) {
        CineCamera bad = lens;
        bad.aperture_f = invalid;
        AETHER_CHECK(player::PackDepthOfField(bad, 1080, 0.1f, 100.0f).radius_scale == 0.0f);
        bad = lens;
        bad.focus_distance = invalid;
        AETHER_CHECK(player::PackDepthOfField(bad, 1080, 0.1f, 100.0f).radius_scale == 0.0f);
        bad = lens;
        bad.sensor_height_mm = invalid;
        AETHER_CHECK(player::PackDepthOfField(bad, 1080, 0.1f, 100.0f).radius_scale == 0.0f);
        bad = lens;
        bad.focal_length_mm = invalid;
        AETHER_CHECK(player::PackDepthOfField(bad, 1080, 0.1f, 100.0f).radius_scale == 0.0f);
    }
    lens.focus_distance = 0.01f;
    AETHER_CHECK(player::PackDepthOfField(lens, 1080, 0.1f, 100.0f).radius_scale == 0.0f);
    AETHER_CHECK(player::PackDepthOfField(CineCamera{}, 0, 0.1f, 100.0f).radius_scale == 0.0f);
    AETHER_CHECK(player::PackDepthOfField(CineCamera{}, 1080, 1.0f, 0.1f).radius_scale == 0.0f);
}

namespace {

void CheckDepthOfField(gfx::rhi::Backend backend) {
    auto device = MakeDevice(backend);
    if (!device) return;
    auto target = device->CreateSwapChain(nullptr, kSize, kSize, 2);
    player::PostPass post(*device, *target);
    if (!post.Valid()) return;
    std::vector<u16> color;
    std::vector<f32> depths;
    for (u32 y = 0; y < kSize; ++y) {
        const f32 distance = y < 21 ? 0.5f : y < 43 ? 1.0f : 4.0f;
        for (u32 x = 0; x < kSize; ++x) {
            const u16 value = player::FloatToHalf(x % 2 == 0 ? 0.125f : 4.0f);
            color.insert(color.end(), {value, value, value, player::FloatToHalf(1.0f)});
            depths.push_back(SsrDeviceDepth(distance));
        }
    }
    auto scene = device->CreateHalfFloatTexture(kSize, kSize, 1, color.data());
    auto depth = device->CreateR32FloatTexture(kSize, kSize, depths.data());
    AETHER_CHECK(scene.IsValid() && depth.IsValid());
    if (!scene.IsValid() || !depth.IsValid()) {
        if (scene.IsValid()) device->DestroyTexture(scene);
        if (depth.IsValid()) device->DestroyTexture(depth);
        return;
    }
    CineCamera lens;
    lens.focal_length_mm = 200.0f;
    lens.aperture_f = 0.7f;
    lens.focus_distance = 1.0f;
    PostProcessSettings settings;
    settings.screen_space_reflections = 0.0f;
    u32 extent = kSize;
    bool bloom = false;
    auto commands = device->CreateCommandList();
    const auto render = [&](const CineCamera* camera, bool bind_depth, bool blur_expected, std::vector<u8>& pixels) {
        post.SetSource(scene.index);
        if (bind_depth) post.SetAmbientOcclusion({depth.index, 0.0f, 1.0f, kSsrNear, kSsrFar, kSsrProjection, kSsrProjection});
        target->AcquireNextImage();
        commands->Reset();
        if (bloom) AETHER_CHECK(post.RenderBloom(*commands, settings));
        if (camera) AETHER_CHECK(post.RenderDepthOfField(*commands, settings, *camera) == blur_expected);
        commands->BeginRenderPass(*target, {1, 0, 1, 1});
        post.Draw(*commands, settings);
        commands->EndRenderPass();
        commands->Close();
        device->WaitForFence(device->Submit(*commands, target.get()));
        const bool read = target->ReadBack(pixels);
        target->Present(false);
        AETHER_CHECK(read && pixels.size() == extent * extent * 4);
        return read && pixels.size() == extent * extent * 4;
    };
    std::vector<u8> sharp, blurred, reset, rack;
    if (render(nullptr, true, false, sharp) && render(&lens, true, true, blurred) &&
        render(&lens, false, false, reset)) {
        AETHER_CHECK(sharp == reset);
        if (const char* directory = std::getenv("AETHER_RENDER_TEST_OUT")) {
            for (const auto& capture : {std::pair{"sharp", &sharp}, std::pair{"defocused", &blurred}}) {
                const auto png = assets::EncodePngRgba8(kSize, kSize, *capture.second);
                const std::string name = std::string(backend == gfx::rhi::Backend::D3D12 ? "d3d12-" : "vulkan-") + capture.first + ".png";
                std::ofstream file(std::filesystem::path(directory) / name, std::ios::binary);
                file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
                AETHER_CHECK(file.good());
            }
        }
        for (const u32 y : {21u, 32u, 42u}) {
            for (u32 x = 16; x < 48; ++x) {
                AETHER_CHECK(std::abs(PixelChannel(sharp, x, y, 1) - PixelChannel(blurred, x, y, 1)) <= 2);
            }
        }
        for (const u32 y : {10u, 53u}) {
            const int original = std::abs(PixelChannel(sharp, 31, y, 1) - PixelChannel(sharp, 32, y, 1));
            const int defocused = std::abs(PixelChannel(blurred, 31, y, 1) - PixelChannel(blurred, 32, y, 1));
            AETHER_CHECK(original > 50);
            AETHER_CHECK(defocused < original / 2);
        }
        lens.focus_distance = 4.0f;
        if (render(&lens, true, true, rack)) {
            AETHER_CHECK(std::abs(PixelChannel(rack, 32, 53, 1) - PixelChannel(sharp, 32, 53, 1)) <= 2);
            AETHER_CHECK(PixelChannel(rack, 32, 32, 1) > PixelChannel(sharp, 32, 32, 1) + 20);
        }
        lens.focus_distance = 1.0f;
        settings.fog_density = 0.1f;
        settings.exposure_compensation = -1.0f;
        settings.vignette = 0.5f;
        settings.chromatic_aberration = 0.5f;
        settings.sharpen = 0.2f;
        settings.bloom_intensity = 0.3f;
        bloom = true;
        if (render(nullptr, true, false, sharp) && render(&lens, true, true, blurred)) {
            for (u32 x = 16; x < 48; ++x) {
                AETHER_CHECK(std::abs(PixelChannel(sharp, x, 32, 1) - PixelChannel(blurred, x, 32, 1)) <= 2);
            }
        }
        lens.aperture_f = 0.0f;
        if (render(&lens, true, false, reset)) AETHER_CHECK(sharp == reset);
        device->DestroyTexture(scene);
        device->DestroyTexture(depth);
        extent = 32;
        target->Resize(extent, extent);
        color.assign(extent * extent * 4, player::FloatToHalf(0.5f));
        depths.assign(extent * extent, SsrDeviceDepth(1.0f));
        scene = device->CreateHalfFloatTexture(extent, extent, 1, color.data());
        depth = device->CreateR32FloatTexture(extent, extent, depths.data());
        AETHER_CHECK(scene.IsValid() && depth.IsValid());
        lens.aperture_f = 0.7f;
        lens.focus_distance = 4.0f;
        settings = {};
        settings.screen_space_reflections = 0.0f;
        if (scene.IsValid() && depth.IsValid() && render(nullptr, true, false, sharp) && render(&lens, true, true, blurred)) {
            for (usize i = 1; i < sharp.size(); i += 4) AETHER_CHECK(std::abs(static_cast<int>(sharp[i]) - blurred[i]) <= 2);
        }
    }
    device->DestroyTexture(scene);
    device->DestroyTexture(depth);
}

} // namespace

namespace {

void CheckAutoExposure(gfx::rhi::Backend backend) {
    auto device = MakeDevice(backend);
    if (!device) return;
    auto target = device->CreateSwapChain(nullptr, 32, 32, 2);
    player::PostPass post(*device, *target);
    if (!post.Valid()) return;
    PostProcessSettings settings;
    settings.screen_space_reflections = 0.0f;
    bool meter = true, bloom = false, dof = false;
    const char* capture = nullptr;
    u64 camera = 1;
    f32 dt = 0.25f;
    std::vector<f32> image;
    const auto timer = device->SupportsGpuTiming() ? device->CreateGpuTimer(2) : gfx::rhi::GpuTimerHandle{};
    f64 metering_ms = 0;
    u32 measured_frames = 0;
    const auto render = [&](f32 luminance, f32 expected_ev, bool automatic) {
        const u32 width = target->Width(), height = target->Height();
        std::vector<u16> color;
        for (usize i = 0; i < width * height; ++i) {
            const u16 value = player::FloatToHalf(image.empty() ? luminance : image[i]);
            color.insert(color.end(), {value, value, value, player::FloatToHalf(1.0f)});
        }
        const auto scene = device->CreateHalfFloatTexture(width, height, 1, color.data());
        AETHER_CHECK(scene.IsValid());
        if (!scene.IsValid()) return;
        gfx::rhi::SampledTextureHandle depth;
        if (dof) {
            const std::vector<f32> depths(width * height, SsrDeviceDepth(1.0f));
            depth = device->CreateR32FloatTexture(width, height, depths.data());
            AETHER_CHECK(depth.IsValid());
            if (!depth.IsValid()) { device->DestroyTexture(scene); return; }
            post.SetAmbientOcclusion({depth.index, 0.0f, 1.0f, kSsrNear, kSsrFar, kSsrProjection, kSsrProjection});
        }
        post.SetSource(scene.index);
        target->AcquireNextImage();
        auto commands = device->CreateCommandList();
        commands->Reset();
        if (timer.IsValid()) commands->WriteGpuTimestamp(timer, 0);
        if (meter) AETHER_CHECK(post.RenderAutoExposure(*commands, settings, dt, camera) == automatic);
        if (timer.IsValid()) { commands->WriteGpuTimestamp(timer, 1); commands->ResolveGpuTimer(timer); }
        if (bloom) AETHER_CHECK(post.RenderBloom(*commands, settings));
        if (dof) {
            CineCamera lens;
            lens.focal_length_mm = 200.0f;
            lens.aperture_f = 0.7f;
            lens.focus_distance = 4.0f;
            AETHER_CHECK(post.RenderDepthOfField(*commands, settings, lens));
        }
        commands->BeginRenderPass(*target, {1, 0, 1, 1});
        post.Draw(*commands, settings);
        commands->EndRenderPass();
        commands->Close();
        device->WaitForFence(device->Submit(*commands, target.get()));
        std::vector<u8> pixels;
        const bool read = target->ReadBack(pixels);
        target->Present(false);
        AETHER_CHECK(read && pixels.size() == width * height * 4);
        if (read && pixels.size() == width * height * 4) {
            if (const char* directory = std::getenv("AETHER_RENDER_TEST_OUT"); directory && capture) {
                const auto png = assets::EncodePngRgba8(width, height, pixels);
                const std::string name = std::string(backend == gfx::rhi::Backend::D3D12 ? "d3d12-" : "vulkan-") + capture + ".png";
                std::ofstream file(std::filesystem::path(directory) / name, std::ios::binary);
                file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
                AETHER_CHECK(file.good());
            }
            auto constants = player::PackPostConstants(settings, scene.index);
            if (automatic) constants.exposure *= 1.0f + (ExposureFromEV100(expected_ev) - 1.0f) * std::clamp(settings.auto_exposure, 0.0f, 1.0f);
            int worst = 0;
            for (u32 y = 0; y < height; ++y) {
                for (u32 x = 0; x < width; ++x) {
                    const usize i = static_cast<usize>(y) * width + x;
                    const f32 value = player::HalfToFloat(color[i * 4]);
                    const auto expected = player::PostReference(Vec3(value, value, value), constants,
                                                                 (x + 0.5f) / width, (y + 0.5f) / height);
                    worst = std::max(worst, std::abs(static_cast<int>(pixels[i * 4 + 1]) - static_cast<int>(std::lround(expected.y * 255))));
                }
            }
            if (worst > 2) std::printf("    auto exposure: luminance %g, EV %g, strength %g, worst error %d\n", luminance, expected_ev, settings.auto_exposure, worst);
            AETHER_CHECK(worst <= 2);
        }
        if (timer.IsValid()) {
            f64 times[2]{};
            if (device->ReadGpuTimer(timer, times) && automatic) { metering_ms += times[1]; ++measured_frames; }
        }
        device->DestroyTexture(scene);
        if (depth.IsValid()) device->DestroyTexture(depth);
    };
    render(1.0f, 0.0f, false);
    settings.auto_exposure = 1.0f;
    render(1.0f, 3.0f, true);
    f32 ev = 3.0f;
    for (const f32 luminance : {0.0625f, 0.0625f, 4.0f, 4.0f}) {
        ev = AdaptEV100(ev, EV100FromLuminance(luminance), dt, {});
        render(luminance, ev, true);
        ev = player::HalfToFloat(player::FloatToHalf(ev));
    }
    dt = 0.0f;
    render(0.125f, ev, true);
    ++camera;
    render(0.125f, 0.0f, true);
    settings.auto_exposure = 0.5f;
    render(0.125f, 0.0f, true);
    post.ResetAutoExposure();
    settings.auto_exposure = 1.0f;
    render(4.0f, 5.0f, true);
    meter = false;
    render(1.0f, 0.0f, false);
    meter = true;
    render(0.125f, 0.0f, true);
    for (const f32 invalid : {0.0f, -1.0f, std::nanf(""), HUGE_VALF}) {
        settings.auto_exposure = invalid;
        render(1.0f, 0.0f, false);
        settings.auto_exposure = 1.0f;
        render(0.125f, 0.0f, true);
    }
    for (const f32 invalid : {-1.0f, std::nanf(""), HUGE_VALF}) {
        dt = invalid;
        render(1.0f, 0.0f, false);
    }
    dt = 0.0f;
    render(0.125f, 0.0f, true);
    settings.auto_exposure = 2.0f;
    render(0.125f, 0.0f, true);
    settings.auto_exposure = 1.0f;
    target->Resize(31, 17);
    image.assign(31 * 17, 4.0f);
    for (u32 y = 0; y < 17; ++y) for (u32 x = 0; x < 15; ++x) image[y * 31 + x] = 0.0f;
    render(4.0f, 5.0f, true);
    image.assign(31 * 17, 1.0f);
    image[0] = 64.0f;
    post.ResetAutoExposure();
    render(1.0f, 3.0f + 6.0f / (31 * 17), true);
    image.clear();
    target->Resize(1, 1);
    render(0.0f, -4.0f, true);
    render(0.0625f, -4.0f, true);
    target->Resize(64, 64);
    image.resize(64 * 64);
    for (u32 y = 0; y < 64; ++y) for (u32 x = 0; x < 64; ++x) image[y * 64 + x] = std::exp2(static_cast<f32>(x / 8) - 4.0f);
    capture = "exposure-manual";
    meter = false;
    render(1.0f, 0.0f, false);
    capture = "exposure-automatic";
    meter = true;
    render(1.0f, 2.5f, true);
    capture = nullptr;
    image.clear();
    post.ResetAutoExposure();
    bloom = dof = true;
    settings.exposure_compensation = 1.5f;
    settings.bloom_intensity = 0.4f;
    settings.vignette = 0.5f;
    render(0.125f, 0.0f, true);
    if (measured_frames) std::printf("    %s GPU auto exposure: %.4f ms mean over %u test frames\n",
                                     backend == gfx::rhi::Backend::D3D12 ? "D3D12" : "Vulkan", metering_ms / measured_frames, measured_frames);
    if (timer.IsValid()) device->DestroyGpuTimer(timer);
}

} // namespace

AETHER_TEST(PostPass_AutoExposureOnD3D12) { CheckAutoExposure(gfx::rhi::Backend::D3D12); }
AETHER_TEST(PostPass_AutoExposureOnVulkan) { CheckAutoExposure(gfx::rhi::Backend::Vulkan); }

AETHER_TEST(PostVolume_AutoExposureBlendsAndDefaultsOff) {
    AETHER_CHECK(PostProcessVolume{}.auto_exposure == 0.0f && PostProcessSettings{}.auto_exposure == 0.0f);
    RenderScene scene;
    PostProcessVolume volume;
    volume.auto_exposure = 1.0f;
    volume.weight = 0.5f;
    scene.post_volumes.push_back({Vec3(0, 0, 0), volume});
    AETHER_CHECK(BlendPostProcess(scene, Vec3(0, 0, 0)).auto_exposure == 0.5f);
    scene.post_volumes[0].settings.global = false;
    scene.post_volumes[0].settings.blend_distance = 2.0f;
    AETHER_CHECK(BlendPostProcess(scene, Vec3(6, 0, 0)).auto_exposure == 0.25f);
    AETHER_CHECK(BlendPostProcess(scene, Vec3(8, 0, 0)).auto_exposure == 0.0f);
    reflect::Json saved = reflect::ToJson(volume);
    PostProcessVolume loaded;
    AETHER_CHECK(reflect::FromJson(loaded, saved) && loaded.auto_exposure == 1.0f);
    saved.erase("auto_exposure");
    loaded = {};
    AETHER_CHECK(reflect::FromJson(loaded, saved) && loaded.auto_exposure == 0.0f);
}

AETHER_TEST(PostPass_BloomAndExposureShareOneTextureWord) {
    player::PostConstants constants;
    AETHER_CHECK(sizeof(constants) == 128 && player::PostBloomOf(constants) == 255u && player::PostExposureOf(constants) == 255u);
    constants.textures = player::PackPostTextures(17, 73);
    AETHER_CHECK(player::PostBloomOf(constants) == 17u && player::PostExposureOf(constants) == 73u);
    constants.textures = player::PackPostTextures(255, player::PostExposureOf(constants));
    AETHER_CHECK(player::PostBloomOf(constants) == 255u && player::PostExposureOf(constants) == 73u);
}

AETHER_TEST(PostPass_DepthOfFieldOnD3D12) { CheckDepthOfField(gfx::rhi::Backend::D3D12); }
AETHER_TEST(PostPass_DepthOfFieldOnVulkan) { CheckDepthOfField(gfx::rhi::Backend::Vulkan); }

AETHER_TEST(PostPass_ReflectionStrengthRidesInTheTonemapperWord) {
    // It is an 8-bit strength beside the tone mapper (0 is off), clamped, and what is not a number counts as 0.
    PostProcessSettings s;
    s.screen_space_reflections = 0.5f;
    const player::PostConstants half = player::PackPostConstants(s, 1);
    AETHER_CHECK(player::PostScreenSpaceReflectionsOf(half) == 128u);
    AETHER_CHECK(player::PostTonemapperOf(half) == 0 && player::PostFrameOf(half) == 0u);
    s.screen_space_reflections = 4.0f;
    AETHER_CHECK(player::PostScreenSpaceReflectionsOf(player::PackPostConstants(s, 1)) == 255u);
    s.screen_space_reflections = std::nanf("");
    AETHER_CHECK(player::PostScreenSpaceReflectionsOf(player::PackPostConstants(s, 1)) == 0u);
    s.screen_space_reflections = 0.0f;
    AETHER_CHECK(player::PostScreenSpaceReflectionsOf(player::PackPostConstants(s, 1)) == 0u);
    // A default volume reflects at half strength.
    AETHER_CHECK(player::PostScreenSpaceReflectionsOf(player::PackPostConstants(PostProcessSettings{}, 1)) == 128u);
    // It never reaches the reference the tone mapper is compared against: the pass reflects only what the depth shows.
    player::PostConstants c;
    c.tonemapper = player::PackTonemapperFrame(0, 0, 255u);
    AETHER_CHECK(Near(player::PostReference(Vec3(1, 2, 3), c, 0.5f, 0.5f).y, player::PostReference(Vec3(1, 2, 3), player::PostConstants{}, 0.5f, 0.5f).y, 1e-6f));
}

AETHER_TEST(PostVolume_ReflectionsBlendAndDefaultOn) {
    const auto near = [](f32 a, f32 b) { return std::fabs(a - b) < 1e-5f; };
    AETHER_CHECK(near(PostProcessVolume{}.screen_space_reflections, 0.5f) && near(PostProcessSettings{}.screen_space_reflections, 0.5f));
    RenderScene scene;
    PostProcessVolume base;
    base.screen_space_reflections = 0.2f;
    scene.post_volumes.push_back({Vec3(0, 0, 0), base});
    AETHER_CHECK(near(BlendPostProcess(scene, Vec3(500, 0, 0)).screen_space_reflections, 0.2f));
    PostProcessVolume local;
    local.global = false;
    local.extents = Vec3(5, 5, 5);
    local.blend_distance = 0.0f;
    local.priority = 1;
    local.screen_space_reflections = 1.0f;
    scene.post_volumes.push_back({Vec3(0, 0, 0), local});
    AETHER_CHECK(near(BlendPostProcess(scene, Vec3(0, 0, 0)).screen_space_reflections, 1.0f));
    AETHER_CHECK(near(BlendPostProcess(scene, Vec3(500, 0, 0)).screen_space_reflections, 0.2f));
    // A saved volume round-trips, and one saved before the field existed loads with the default.
    const reflect::Json saved = reflect::ToJson(base);
    PostProcessVolume back;
    back.screen_space_reflections = 0.0f;
    AETHER_CHECK(reflect::FromJson(back, saved) && near(back.screen_space_reflections, 0.2f));
    reflect::Json old = saved;
    old.erase("screen_space_reflections");
    PostProcessVolume loaded;
    AETHER_CHECK(reflect::FromJson(loaded, old) && near(loaded.screen_space_reflections, 0.5f));
}
