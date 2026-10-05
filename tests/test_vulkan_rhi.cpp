#include "test_framework.h"

// Phase 24 step 4: the Vulkan RHI off Windows - glslang compiling the
// engine's HLSL to SPIR-V, an offscreen swap chain, and a frame drawn
// through the abstract RHI and read back. Runs on any Vulkan driver;
// in CI that is lavapipe (Mesa's software Vulkan).

#if defined(AETHER_HAS_VULKAN) && !defined(_WIN32)

#include "aether/gfx/rhi/device.h"
#include "aether/gfx/shader_compiler.h"
#include "aether/platform/window.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>

using namespace aether;
using namespace aether::gfx;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

// Push constants carry the color and a vertical offset; the triangle is
// drawn from SV_VertexID, so the pipeline needs no vertex buffer.
constexpr const char* kShader = R"(
struct PushConstants {
    float4 color;
    float4 offset;
};
[[vk::push_constant]]
ConstantBuffer<PushConstants> g_PC : register(b0);

float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 corners[3] = { float2(-1.0, 0.0), float2(1.0, 0.0), float2(0.0, 1.0) };
    return float4(corners[id] + g_PC.offset.xy, 0.0, 1.0);
}

float4 PSMain() : SV_Target {
    return g_PC.color;
}
)";

struct Push {
    f32 color[4];
    f32 offset[4];
};

std::unique_ptr<rhi::IDevice> MakeDevice() {
    try {
        return rhi::CreateDevice(rhi::Backend::Vulkan, false);
    } catch (const std::exception&) {
        return nullptr;
    }
}

const u8* Pixel(const std::vector<u8>& rgba, u32 width, u32 x, u32 y) {
    return &rgba[(static_cast<usize>(y) * width + x) * 4];
}

bool Near(const u8* p, int r, int g, int b) {
    const auto close = [](int a, int b) { return a - b <= 2 && b - a <= 2; };
    return close(p[0], r) && close(p[1], g) && close(p[2], b);
}

} // namespace

AETHER_TEST(Vulkan_GlslangCompilesHlslToSpirv) {
    const ShaderBytecode vs = CompileHLSLToSPIRV(kShader, "VSMain", "vs_6_0", "test_vs");
    const ShaderBytecode ps = CompileHLSLToSPIRV(kShader, "PSMain", "ps_6_0", "test_ps");
    CHECK(vs.Size() > 20 && vs.Size() % 4 == 0 && ps.Size() > 20);
    u32 magic = 0;
    std::memcpy(&magic, vs.Data(), 4);
    CHECK(magic == 0x07230203u);
    // The entry point keeps its HLSL name (the pipeline asks for it by name).
    const std::string words(reinterpret_cast<const char*>(vs.Data()), vs.Size());
    CHECK(words.find("VSMain") != std::string::npos);
    bool threw = false;
    try {
        CompileHLSLToSPIRV("float4 PSMain() : SV_Target { return undefined_thing; }", "PSMain", "ps_6_0", "bad");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(!rhi::IsBackendAvailable(rhi::Backend::D3D12) && rhi::IsBackendAvailable(rhi::Backend::Vulkan));
}

AETHER_TEST(Vulkan_OffscreenFrameReadsBack) {
    std::unique_ptr<rhi::IDevice> device = MakeDevice();
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    CHECK(device->GetBackend() == rhi::Backend::Vulkan);
    constexpr u32 kWidth = 64, kHeight = 48;
    std::unique_ptr<rhi::ISwapChain> target = device->CreateSwapChain(nullptr, kWidth, kHeight, 2);
    CHECK(target->Width() == kWidth && target->Height() == kHeight && target->BufferCount() == 2);

    rhi::PipelineDesc desc;
    desc.hlsl_source = kShader;
    desc.push_constant_size_bytes = sizeof(Push);
    const rhi::PipelineHandle pipeline = device->CreatePipeline(desc, *target);
    CHECK(pipeline.IsValid());

    std::unique_ptr<rhi::ICommandList> cmd = device->CreateCommandList();
    const auto frame = [&](f32 offset_y, std::vector<u8>& pixels) {
        target->AcquireNextImage();
        cmd->Reset();
        cmd->BeginRenderPass(*target, {0.25f, 0.5f, 0.75f, 1.0f});
        cmd->BindPipeline(pipeline);
        const Push push{{1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, offset_y, 0.0f, 0.0f}};
        cmd->SetPushConstants(&push, sizeof(push));
        cmd->Draw(3);
        cmd->EndRenderPass();
        cmd->Close();
        device->WaitForFence(device->Submit(*cmd, target.get()));
        const bool ok = target->ReadBack(pixels);
        target->Present(false);
        return ok;
    };

    // The triangle fills the top half's middle: +Y is up on both backends.
    std::vector<u8> pixels;
    CHECK(frame(0.0f, pixels));
    CHECK(pixels.size() == kWidth * kHeight * 4);
    CHECK(Near(Pixel(pixels, kWidth, kWidth / 2, kHeight / 8), 255, 0, 0));     // inside, near the top
    CHECK(Near(Pixel(pixels, kWidth, kWidth / 2, kHeight * 3 / 4), 64, 128, 191)); // bottom half: the clear
    CHECK(Near(Pixel(pixels, kWidth, 1, 1), 64, 128, 191));                      // top-left corner: outside
    CHECK(Pixel(pixels, kWidth, 0, 0)[3] == 255);

    // The next frame goes to the other image; moved down, the bottom half fills.
    CHECK(frame(-1.0f, pixels));
    CHECK(Near(Pixel(pixels, kWidth, kWidth / 2, kHeight * 5 / 8), 255, 0, 0));
    CHECK(Near(Pixel(pixels, kWidth, kWidth / 2, kHeight / 8), 64, 128, 191));

    // Resizing keeps it working.
    target->Resize(32, 32);
    CHECK(target->Width() == 32 && target->Height() == 32);
    CHECK(frame(0.0f, pixels) && pixels.size() == 32 * 32 * 4);
    CHECK(Near(Pixel(pixels, 32, 16, 4), 255, 0, 0));
}

AETHER_TEST(Vulkan_TexturedQuadThroughBindlessTable) {
    std::unique_ptr<rhi::IDevice> device = MakeDevice();
    if (!device) return;
    std::unique_ptr<rhi::ISwapChain> target = device->CreateSwapChain(nullptr, 32, 32, 1);
    // A 2x1 texture, left green and right blue, stretched over the target.
    const u8 texels[8] = {0, 255, 0, 255, 0, 0, 255, 255};
    const rhi::SampledTextureHandle texture = device->CreateTexture(2, 1, texels);
    CHECK(texture.IsValid());

    rhi::PipelineDesc desc;
    desc.hlsl_source = R"(
struct PushConstants { uint texture_index; uint3 pad; };
[[vk::push_constant]]
ConstantBuffer<PushConstants> g_PC : register(b0);
Texture2D g_Textures[32] : register(t0, space0);
SamplerState g_Sampler : register(s0, space1);
struct VSOut { float4 position : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}
float4 PSMain(VSOut i) : SV_Target {
    return g_Textures[g_PC.texture_index].Sample(g_Sampler, i.uv);
}
)";
    desc.push_constant_size_bytes = 16;
    desc.enable_bindless_textures = true;
    const rhi::PipelineHandle pipeline = device->CreatePipeline(desc, *target);

    std::unique_ptr<rhi::ICommandList> cmd = device->CreateCommandList();
    target->AcquireNextImage();
    cmd->Reset();
    cmd->BeginRenderPass(*target, {0, 0, 0, 1});
    cmd->BindPipeline(pipeline);
    cmd->BindBindlessTextures();
    const u32 push[4] = {texture.index, 0, 0, 0};
    cmd->SetPushConstants(push, sizeof(push));
    cmd->Draw(3);
    cmd->EndRenderPass();
    cmd->Close();
    device->WaitForFence(device->Submit(*cmd, target.get()));
    std::vector<u8> pixels;
    CHECK(target->ReadBack(pixels));
    // Texel centres (a quarter and three quarters across), whatever the filter.
    const u8* left = Pixel(pixels, 32, 8, 16);
    const u8* right = Pixel(pixels, 32, 23, 16);
    CHECK(left[1] > 200 && left[2] < 60);
    CHECK(right[2] > 200 && right[1] < 60);
}

#if defined(AETHER_HAS_WINDOW)
AETHER_TEST(Vulkan_WindowSwapChainPresents) {
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY")) {
        std::printf("    (no display: run under xvfb-run to test a window swap chain)\n");
        return;
    }
    std::unique_ptr<rhi::IDevice> device = MakeDevice();
    if (!device) return;
    WindowDesc window_desc;
    window_desc.title = "Aether Vulkan test";
    window_desc.width = 96, window_desc.height = 64;
    window_desc.visible = false;
    Window window(window_desc);
    CHECK(window.IsValid());
    std::unique_ptr<rhi::ISwapChain> swap_chain =
        device->CreateSwapChain(window.PlatformWindow(), window.Width(), window.Height(), 2);
    CHECK(swap_chain->Width() == 96 && swap_chain->Height() == 64 && swap_chain->BufferCount() >= 2);
    std::unique_ptr<rhi::ICommandList> cmd = device->CreateCommandList();
    for (int i = 0; i < 3; ++i) {
        CHECK(window.PumpMessages());
        swap_chain->AcquireNextImage();
        cmd->Reset();
        cmd->BeginRenderPass(*swap_chain, {0.0f, 1.0f, 0.0f, 1.0f});
        cmd->EndRenderPass();
        cmd->Close();
        device->WaitForFence(device->Submit(*cmd, swap_chain.get()));
        std::vector<u8> pixels;
        if (swap_chain->ReadBack(pixels)) { // where the surface allows copies out
            CHECK(Near(Pixel(pixels, 96, 48, 32), 0, 255, 0));
        }
        swap_chain->Present(true);
    }
}
#endif

#endif
