#include "../player/src/post_pass.h"
#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"
#include "aether/renderer/post.h"
#include "test_framework.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

using namespace aether;

// Render roadmap step 4: render textures in the RHI (IDevice::CreateRenderTexture, ICommandList::BeginTexturePass) and the post pass's
// bloom chain built on them. The GPU test draws one bright patch into the HDR target and checks how the composite spreads it.

namespace {

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

constexpr u32 kWidth = 128, kHeight = 64;

// A patch of linear 8.0 at the left edge, rows 30..33, columns 0..3; black elsewhere. Grey, so channel order doesn't matter.
constexpr char kPatchShader[] = R"(
float4 VSMain(uint id : SV_VertexID) : SV_POSITION {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.5, 1.0);
}
float4 PSMain(float4 position : SV_POSITION) : SV_TARGET {
    bool lit = position.x < 4.0 && position.y >= 30.0 && position.y < 34.0;
    return float4((lit ? 8.0 : 0.0).xxx, 1.0);
}
)";

// Draws the patch into the scene colour target, optionally the bloom chain, then the post pass; the back buffer in `pixels`.
bool RenderPatch(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& target, player::PostPass& post, gfx::rhi::PipelineHandle patch,
                 const PostProcessSettings& settings, bool bloom, std::vector<u8>& pixels, bool* bloom_drawn = nullptr) {
    std::unique_ptr<gfx::rhi::ICommandList> cmd = device.CreateCommandList();
    target.AcquireNextImage();
    cmd->Reset();
    if (!cmd->BeginSceneColorPass(target, {0.0f, 0.0f, 0.0f, 1.0f})) return false;
    cmd->BindPipeline(patch);
    cmd->Draw(3);
    cmd->EndRenderPass();
    const bool drawn = bloom && post.RenderBloom(*cmd, settings);
    if (bloom_drawn) *bloom_drawn = drawn;
    cmd->BeginRenderPass(target, {1.0f, 0.0f, 1.0f, 1.0f});
    post.Draw(*cmd, settings);
    cmd->EndRenderPass();
    cmd->Close();
    device.WaitForFence(device.Submit(*cmd, &target));
    const bool read = target.ReadBack(pixels);
    target.Present(false);
    return read && pixels.size() == kWidth * kHeight * 4;
}

int Green(const std::vector<u8>& pixels, u32 x, u32 y) { return pixels[(static_cast<usize>(y) * kWidth + x) * 4 + 1]; }

bool Compiles(const std::string& hlsl, const char* stage, const char* entry) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / (std::string("aether_bloom_shader_") + stage);
    std::filesystem::create_directories(dir);
    const std::filesystem::path src = dir / "bloom.hlsl", log = dir / "log.txt";
    {
        std::ofstream f(src);
        f << "#define __spirv__ 1\n" << hlsl;
    }
    const std::string cmd = std::string("glslangValidator -D -V -S ") + stage + " -e " + entry + " \"" + src.string() + "\" -o \"" +
                            (dir / "out.spv").string() + "\" > \"" + log.string() + "\" 2>&1";
    const bool ok = std::system(cmd.c_str()) == 0;
    if (!ok) {
        std::ifstream l(log);
        std::printf("%s\n", std::string((std::istreambuf_iterator<char>(l)), std::istreambuf_iterator<char>()).c_str());
    }
    std::filesystem::remove_all(dir);
    return ok;
}

} // namespace

AETHER_TEST(Bloom_ConstantsAndChain) {
    static_assert(sizeof(player::BloomConstants) == 32 && sizeof(player::PostConstants) == 128);
    PostProcessSettings s;
    player::PostConstants c = player::PackPostConstants(s, 3);
    // Off until RenderBloom has drawn: no bloom index, the setting's intensity kept (0.05 by default) for Draw to use.
    AETHER_CHECK(c.bloom >= gfx::rhi::kMaxBindlessTextures && c.bloom_intensity == 0.05f && c.bloom_scale == 1.0f);
    s.bloom_intensity = 4.0f;
    AETHER_CHECK(player::PackPostConstants(s, 3).bloom_intensity == 1.0f);
    s.bloom_intensity = -1.0f;
    AETHER_CHECK(player::PackPostConstants(s, 3).bloom_intensity == 0.0f);
    s.bloom_intensity = std::numeric_limits<f32>::quiet_NaN();
    AETHER_CHECK(player::PackPostConstants(s, 3).bloom_intensity == 0.0f);

    // Half size per level from half the screen, at most five levels, none under 8 pixels a side.
    const std::vector<Extent> hd = player::PostBloomChain(1280, 720);
    AETHER_CHECK(hd.size() == player::kBloomLevels);
    if (hd.size() == player::kBloomLevels) {
        AETHER_CHECK((hd[0] == Extent{640, 360}) && (hd[4] == Extent{40, 23}));
    }
    AETHER_CHECK(player::PostBloomChain(kWidth, kHeight).size() == 3); // 64x32, 32x16, 16x8
    AETHER_CHECK(player::PostBloomChain(12, 12).empty());              // 6x6 is already under the minimum
}

AETHER_TEST(Bloom_ShadersCompileWithGlslang) {
    if (std::system("glslangValidator --version > /dev/null 2>&1") != 0) {
        std::printf("  (glslangValidator not found: skipping the HLSL compile check)\n");
        return;
    }
    for (const bool upsample : {false, true}) {
        const std::string source = player::BloomShaderSource(upsample);
        AETHER_CHECK(Compiles(source, "vert", "VSMain"));
        AETHER_CHECK(Compiles(source, "frag", "PSMain"));
    }
    AETHER_CHECK(Compiles(player::PostShaderSource(), "frag", "PSMain"));
}

static void Bloom_RenderTextures(gfx::rhi::Backend backend) {
    std::unique_ptr<gfx::rhi::IDevice> device = MakeDevice(backend);
    if (!device) return;
    std::unique_ptr<gfx::rhi::ISwapChain> target = device->CreateSwapChain(nullptr, kWidth, kHeight, 2);
    const gfx::rhi::SampledTextureHandle texture = device->CreateRenderTexture(32, 16);
    AETHER_CHECK(texture.IsValid() && texture.index < gfx::rhi::kMaxBindlessTextures);
    if (!texture.IsValid()) return;
    bool threw = false;
    try {
        (void)device->CreateRenderTexture(0, 16);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    AETHER_CHECK(threw);
    const u8 white[4] = {255, 255, 255, 255};
    const gfx::rhi::SampledTextureHandle plain = device->CreateTexture(1, 1, white);

    std::unique_ptr<gfx::rhi::ICommandList> cmd = device->CreateCommandList();
    cmd->Reset();
    const gfx::rhi::ClearColor black{0.0f, 0.0f, 0.0f, 1.0f};
    AETHER_CHECK(!cmd->BeginTexturePass(plain, &black));                         // a sampled texture isn't a render target
    AETHER_CHECK(!cmd->BeginTexturePass(gfx::rhi::SampledTextureHandle{}, &black)); // nor is no texture
    AETHER_CHECK(cmd->BeginTexturePass(texture, &black));
    AETHER_CHECK(!cmd->BeginTexturePass(texture, nullptr)); // nested: refused
    AETHER_CHECK(!cmd->CopySceneDepth());                   // no depth in a texture pass
    cmd->EndRenderPass();
    AETHER_CHECK(cmd->BeginTexturePass(texture, nullptr)); // again, keeping what it holds
    cmd->EndRenderPass();
    cmd->Close();
    device->WaitForFence(device->Submit(*cmd, nullptr));

    AETHER_CHECK(device->DestroyTexture(texture));
    cmd->Reset();
    AETHER_CHECK(!cmd->BeginTexturePass(texture, &black)); // destroyed: refused
    cmd->Close();
    device->WaitForFence(device->Submit(*cmd, nullptr));
    if (plain.IsValid()) device->DestroyTexture(plain);
}

static void Bloom_SpreadsABrightPatch(gfx::rhi::Backend backend) {
    std::unique_ptr<gfx::rhi::IDevice> device = MakeDevice(backend);
    if (!device) return;
    std::unique_ptr<gfx::rhi::ISwapChain> target = device->CreateSwapChain(nullptr, kWidth, kHeight, 2);
    player::PostPass post(*device, *target);
    if (!post.Valid()) {
        std::printf("    (no HDR scene colour target on this device: skipping)\n");
        return;
    }
    gfx::rhi::PipelineDesc patch_desc;
    patch_desc.hlsl_source = kPatchShader;
    patch_desc.scene_color_target = true;
    const gfx::rhi::PipelineHandle patch = device->CreatePipeline(patch_desc, *target);
    AETHER_CHECK(patch.IsValid());
    if (!patch.IsValid()) return;

    PostProcessSettings settings; // ACES, exposure 1
    settings.bloom_intensity = 0.5f;
    std::vector<u8> plain, bloomed;
    bool drawn = false;
    AETHER_CHECK(RenderPatch(*device, *target, post, patch, settings, false, plain));
    AETHER_CHECK(RenderPatch(*device, *target, post, patch, settings, true, bloomed, &drawn));
    AETHER_CHECK(drawn && post.BloomTextures().size() == 3);
    if (plain.size() != kWidth * kHeight * 4 || bloomed.size() != plain.size()) return;

    // Without bloom the patch is a hard edge: black right beside it.
    AETHER_CHECK(Green(plain, 2, 31) > 200 && Green(plain, 8, 31) == 0 && Green(plain, 2, 40) == 0);
    // With it, light spreads beyond the patch, falling off with distance along the row and up the column.
    AETHER_CHECK(Green(bloomed, 8, 31) > 20);
    AETHER_CHECK(Green(bloomed, 2, 40) > 10);
    int previous = 255;
    bool monotonic = true;
    for (u32 x = 4; x < 48; ++x) {
        const int g = Green(bloomed, x, 31);
        if (g > previous + 1) monotonic = false; // one step of rounding allowed
        previous = std::min(previous, g);
    }
    AETHER_CHECK(monotonic);
    AETHER_CHECK(Green(bloomed, 8, 31) > Green(bloomed, 24, 31) && Green(bloomed, 24, 31) >= Green(bloomed, 40, 31));
    // The lerp takes from the patch what it gives its surroundings: the patch itself gets dimmer, not brighter.
    AETHER_CHECK(Green(bloomed, 1, 31) <= Green(plain, 1, 31));
    // The taps clamp at the edges: nothing wraps round to the far side of the screen (the shared sampler wraps; measured 0 there, against
    // 162 four pixels from the patch).
    AETHER_CHECK(Green(bloomed, kWidth - 1, 31) == 0 && Green(bloomed, kWidth - 2, 31) == 0);

    // Intensity 0: RenderBloom draws nothing and the frame is exactly the step 1 post pass.
    settings.bloom_intensity = 0.0f;
    std::vector<u8> off;
    AETHER_CHECK(RenderPatch(*device, *target, post, patch, settings, true, off, &drawn));
    AETHER_CHECK(!drawn);
    PostProcessSettings no_bloom = settings;
    std::vector<u8> reference;
    AETHER_CHECK(RenderPatch(*device, *target, post, patch, no_bloom, false, reference));
    AETHER_CHECK(off == reference);

    // A resize remakes the chain at the new size on the next RenderBloom; releasing gives the slots back.
    target->Resize(kWidth * 2, kHeight * 2);
    settings.bloom_intensity = 0.5f;
    std::unique_ptr<gfx::rhi::ICommandList> cmd = device->CreateCommandList();
    target->AcquireNextImage();
    cmd->Reset();
    AETHER_CHECK(cmd->BeginSceneColorPass(*target, {0.0f, 0.0f, 0.0f, 1.0f}));
    cmd->EndRenderPass();
    AETHER_CHECK(post.RenderBloom(*cmd, settings));
    cmd->BeginRenderPass(*target, {0.0f, 0.0f, 0.0f, 1.0f});
    post.Draw(*cmd, settings);
    cmd->EndRenderPass();
    cmd->Close();
    device->WaitForFence(device->Submit(*cmd, target.get()));
    target->Present(false);
    AETHER_CHECK(post.BloomExtents().size() == 4 && (post.BloomExtents().front() == Extent{kWidth, kHeight}));
    post.ReleaseBloom();
    AETHER_CHECK(post.BloomTextures().empty());
    device->DestroyPipeline(patch);
}

// The rendering checks above, on both backends (Vulkan has render targets since the parity work).
AETHER_TEST(Bloom_RenderTextures_D3D12) { Bloom_RenderTextures(gfx::rhi::Backend::D3D12); }
AETHER_TEST(Bloom_RenderTextures_Vulkan) { Bloom_RenderTextures(gfx::rhi::Backend::Vulkan); }
AETHER_TEST(Bloom_SpreadsABrightPatch_D3D12) { Bloom_SpreadsABrightPatch(gfx::rhi::Backend::D3D12); }
AETHER_TEST(Bloom_SpreadsABrightPatch_Vulkan) { Bloom_SpreadsABrightPatch(gfx::rhi::Backend::Vulkan); }
