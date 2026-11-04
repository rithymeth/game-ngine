#include "aether/terrain/terrain_pass.h"
#include "test_framework.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>

#if defined(_WIN32) || defined(AETHER_HAS_VULKAN)
#include "aether/gfx/rhi/device.h"
#define AETHER_TEST_TERRAIN_DEVICES 1
#endif

using namespace aether;
using namespace aether::terrain;

// Priority 3: terrain on the GPU. The constants and the shader's source on any machine; the blend and a drawn terrain scene on every RHI backend
// the machine has (Vulkan on lavapipe in CI, D3D12 on the Windows job).

namespace {

bool Contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

f32 SrgbToLinear(f32 c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

struct LayerColor {
    u8 rgb[3];
    u8 height;
};
// Four layers with distinct colours and heights (the alpha of the layer's texture).
constexpr LayerColor kLayers[4] = {{{230, 40, 40}, 51}, {{40, 200, 60}, 230}, {{40, 80, 230}, 128}, {{220, 220, 40}, 180}};

} // namespace

AETHER_TEST(TerrainGpu_ConstantsPackTheBindingsIntoOneHundredTwentyEightBytes) {
    AETHER_CHECK(sizeof(TerrainConstants) == 128);
    TerrainDrawParams params;
    for (u32 i = 0; i < 4; ++i) {
        params.layers[i].albedo_height.index = 3 + i * 5; // 3, 8, 13, 18
        params.layers[i].tile_scale = 0.1f * static_cast<f32>(i + 1);
    }
    params.splatmap.index = 27;
    params.splat_origin[0] = 10.0f, params.splat_origin[1] = -4.0f;
    params.splat_size[0] = 64.0f, params.splat_size[1] = 32.0f;
    params.height_contrast = 1.7f; // past the range: clamped
    params.sun_direction = Vec3(1, 2, 3);
    params.sun_intensity = 4.5f, params.sky_intensity = 0.25f;
    const TerrainConstants c = PackTerrainConstants(Mat4::Identity(), params);
    const u32 p = c.packed[0];
    AETHER_CHECK((p & 63u) == 3 && ((p >> 6) & 63u) == 8 && ((p >> 12) & 63u) == 13 && ((p >> 18) & 63u) == 18 && ((p >> 24) & 63u) == 27);
    AETHER_CHECK(c.splat_rect[0] == 10.0f && c.splat_rect[1] == -4.0f && c.splat_rect[2] == 1.0f / 64.0f && c.splat_rect[3] == 1.0f / 32.0f);
    const auto half = [](u32 bits) { // the test's own decoder, apart from the one the packing uses
        const u32 e = (bits >> 10) & 31u, m = bits & 1023u;
        return std::ldexp(static_cast<f32>(1024 + m), static_cast<int>(e) - 25);
    };
    AETHER_CHECK(std::fabs(half(c.packed[2] & 0xffffu) - 0.1f) < 1e-3f && std::fabs(half(c.packed[2] >> 16) - 0.2f) < 1e-3f);
    AETHER_CHECK(std::fabs(half(c.packed[3] & 0xffffu) - 0.3f) < 1e-3f && std::fabs(half(c.packed[3] >> 16) - 0.4f) < 1e-3f);
    AETHER_CHECK(c.light[0] == 1.0f && c.light[1] == 2.0f && c.light[2] == 3.0f && c.light[3] == 1.0f);
    AETHER_CHECK(half(c.packed[1] & 0xffffu) == 4.5f && half(c.packed[1] >> 16) == 0.25f);
    u32 surface;
    std::memcpy(&surface, &c.camera[3], 4);
    AETHER_CHECK(c.camera[0] == params.camera_position.x && (surface & 63u) == 57u && ((surface >> 24) & 3u) == 0); // 0.9 * 63 = 56.7
    // A splatmap with no size gets a unit rectangle rather than a divide by zero.
    params.splat_size[0] = params.splat_size[1] = 0.0f;
    const TerrainConstants unit = PackTerrainConstants(Mat4::Identity(), params);
    AETHER_CHECK(unit.splat_rect[2] == 1.0f && unit.splat_rect[3] == 1.0f);
}

AETHER_TEST(TerrainGpu_ReferenceColorUsesTheHeightsOfTheLayers) {
    TerrainDrawParams params;
    for (TerrainLayerBinding& layer : params.layers) layer.roughness = 1.0f; // the dielectric specular is the faintest, but sRGB lifts it to about 30 of 255
    const std::array<Vec3, 4> albedo = {Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1), Vec3(1, 1, 0)};
    const std::array<f32, 4> heights = {0.1f, 0.9f, 0.5f, 0.5f};
    const std::array<f32, 4> half = {0.5f, 0.5f, 0.0f, 0.0f};
    // Equal painted weights: with no contrast the two layers mix evenly, with full contrast the higher layer (green) wins.
    params.height_contrast = 0.0f;
    const std::array<u8, 3> flat = TerrainReferenceColor(albedo, half, heights, params);
    params.height_contrast = 1.0f;
    const std::array<u8, 3> sharp = TerrainReferenceColor(albedo, half, heights, params);
    AETHER_CHECK(std::abs(static_cast<int>(flat[0]) - static_cast<int>(flat[1])) <= 1); // red and green equally
    AETHER_CHECK(sharp[1] > sharp[0] + 40);
    // A layer with no paint contributes nothing whatever its height; no paint at all gives layer 0.
    params.height_contrast = 1.0f;
    AETHER_CHECK(TerrainReferenceColor(albedo, {1.0f, 0.0f, 0.0f, 0.0f}, heights, params)[1] <= 40);
    const std::array<u8, 3> none = TerrainReferenceColor(albedo, {0.0f, 0.0f, 0.0f, 0.0f}, heights, params);
    AETHER_CHECK(none[0] > 0 && none[1] <= 40 && none[2] <= 40);
    // Black albedo stays black; the sun brightens an up-facing surface over the sky alone.
    AETHER_CHECK(TerrainReferenceColor({Vec3(0, 0, 0), Vec3(0, 0, 0), Vec3(0, 0, 0), Vec3(0, 0, 0)}, {1, 0, 0, 0}, heights, params)[0] <= 40);
    params.sun_intensity = 0.0f;
    const std::array<u8, 3> shade = TerrainReferenceColor(albedo, {1, 0, 0, 0}, heights, params);
    params.sun_intensity = 3.0f;
    AETHER_CHECK(TerrainReferenceColor(albedo, {1, 0, 0, 0}, heights, params)[0] > shade[0]);
}

AETHER_TEST(TerrainGpu_LayersRoughnessAndMetallicShapeTheSpecular) {
    const std::array<Vec3, 4> grey = {Vec3(0.2f, 0.2f, 0.2f), Vec3(0.2f, 0.2f, 0.2f), Vec3(0.2f, 0.2f, 0.2f), Vec3(0.2f, 0.2f, 0.2f)};
    const std::array<f32, 4> heights = {0.5f, 0.5f, 0.5f, 0.5f};
    TerrainDrawParams params;
    params.sun_direction = Vec3(0, 1, 0);
    params.camera_position = Vec3(0, 10, 0); // looking straight down the half vector: the highlight is at its peak
    const auto at = [&](f32 roughness, f32 metallic) {
        for (TerrainLayerBinding& layer : params.layers) layer.roughness = roughness, layer.metallic = metallic;
        return TerrainReferenceColor(grey, {1, 0, 0, 0}, heights, params);
    };
    // Smooth dielectric: the highlight is a tight bright lobe; rough: it spreads and lowers.
    AETHER_CHECK(at(0.1f, 0.0f)[0] > at(0.9f, 0.0f)[0]);
    // A metal has no diffuse: at full roughness a grey metal is darker than the grey dielectric.
    AETHER_CHECK(at(1.0f, 1.0f)[0] < at(1.0f, 0.0f)[0]);
    // Moving the camera off the mirror direction dims a smooth surface's highlight.
    const u8 peak = at(0.2f, 0.0f)[0];
    params.camera_position = Vec3(10, 3, 0);
    AETHER_CHECK(at(0.2f, 0.0f)[0] < peak);
    // The values are stored in 6 and 2 bits and the packing says so.
    for (TerrainLayerBinding& layer : params.layers) layer.roughness = 1.0f, layer.metallic = 1.0f;
    u32 bits;
    const TerrainConstants c = PackTerrainConstants(Mat4::Identity(), params);
    std::memcpy(&bits, &c.camera[3], 4);
    AETHER_CHECK(bits == 0xffffffffu);
}

AETHER_TEST(TerrainGpu_ShaderBlendsLikeTheCpuAndReadsTheSplatmapLikeIt) {
    const std::string hlsl = TerrainShaderSource();
    for (const char* part : {"AetherBlendTerrainHeights", "VSMain", "PSMain", "GetDimensions", "(splat_size - 1.0) + 0.5", "g_Camera", "0.01 + h"}) {
        AETHER_CHECK(Contains(hlsl, part));
    }
    // The blend is one function shared with the parity scene, and the shader carries exactly it.
    AETHER_CHECK(Contains(hlsl, TerrainBlendHlsl()));
    AETHER_CHECK(Contains(TerrainBlendHlsl(), "return float4(1.0, 0.0, 0.0, 0.0)")); // the same all-zero fallback as BlendTerrainLayerHeights
    // One scene for the harness, whose reference is the CPU blend.
    const std::vector<gfx::parity::Scene>& scenes = TerrainParityScenes();
    AETHER_CHECK(scenes.size() == 1 && scenes[0].name == "terrain_height_blend");
    const std::vector<u8> reference = gfx::parity::ReferenceImage(scenes[0], 32, 16);
    AETHER_CHECK(reference.size() == 32 * 16 * 4);
    // Top row: painted = (u, 1 - u, ...), so layer 0 (low height) leads on the right edge and layer 1 (high) on the left.
    AETHER_CHECK(reference[(0 * 32 + 31) * 4 + 0] > reference[(0 * 32 + 31) * 4 + 1]);
    AETHER_CHECK(reference[(0 * 32 + 0) * 4 + 1] > reference[(0 * 32 + 0) * 4 + 0]);
}

AETHER_TEST(TerrainGpu_ShaderCompilesWithGlslang) {
    if (std::system("glslangValidator --version > /dev/null 2>&1") != 0) {
        std::printf("  (glslangValidator not found: skipping the HLSL compile check)\n");
        return;
    }
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_terrain_shader";
    std::filesystem::create_directories(dir);
    const std::filesystem::path src = dir / "terrain.hlsl", log = dir / "log.txt";
    {
        std::ofstream f(src);
        f << "#define __spirv__ 1\n" << TerrainShaderSource();
    }
    for (const auto& stage : {std::pair<const char*, const char*>{"vert", "VSMain"}, {"frag", "PSMain"}}) {
        const std::string cmd = std::string("glslangValidator -D -V -S ") + stage.first + " -e " + stage.second + " \"" + src.string() + "\" -o \"" +
                                (dir / "out.spv").string() + "\" > \"" + log.string() + "\" 2>&1";
        const bool ok = std::system(cmd.c_str()) == 0;
        if (!ok) {
            std::ifstream l(log);
            std::printf("%s\n", std::string((std::istreambuf_iterator<char>(l)), std::istreambuf_iterator<char>()).c_str());
        }
        AETHER_CHECK(ok);
    }
    std::filesystem::remove_all(dir);
}

#if defined(AETHER_TEST_TERRAIN_DEVICES)
namespace {

struct NamedDevice {
    const char* name;
    std::unique_ptr<gfx::rhi::IDevice> device;
};
std::vector<NamedDevice> Devices() {
    std::vector<NamedDevice> out;
    for (const gfx::rhi::Backend backend : {gfx::rhi::Backend::D3D12, gfx::rhi::Backend::Vulkan}) {
        if (!gfx::rhi::IsBackendAvailable(backend)) continue;
        try {
            if (auto device = gfx::rhi::CreateDevice(backend, false)) out.push_back({gfx::parity::BackendName(backend), std::move(device)});
        } catch (const std::exception&) {
        }
    }
    return out;
}

} // namespace

AETHER_TEST(TerrainGpu_HeightBlendMatchesTheCpuOnEveryBackend) {
    const gfx::parity::Tolerance tolerance{3, 0.002};
    usize ran = 0;
    for (const gfx::parity::Scene& scene : TerrainParityScenes()) {
        const gfx::parity::SceneReport report =
            gfx::parity::RunScene(scene, 128, 96, {gfx::rhi::Backend::D3D12, gfx::rhi::Backend::Vulkan}, tolerance);
        for (const gfx::parity::BackendRun& run : report.runs) {
            if (!run.available) continue;
            std::printf("    %s on %s: %s\n", scene.name.c_str(), gfx::parity::BackendName(run.backend),
                        run.rendered ? gfx::parity::Describe(run.vs_reference).c_str() : run.error.c_str());
        }
        AETHER_CHECK(report.Ok(tolerance));
        if (report.RenderedCount() > 0) ++ran;
    }
    if (ran == 0) std::printf("    (no RHI backend here: install a driver, or Mesa's lavapipe, to run this)\n");
}

AETHER_TEST(TerrainGpu_ADrawnTerrainShowsEachQuadrantsHeightBlendedLayers) {
    constexpr u32 kW = 128, kH = 96;
    std::vector<NamedDevice> devices = Devices();
    if (devices.empty()) std::printf("    (no RHI backend here: install a driver, or Mesa's lavapipe, to run this)\n");
    for (NamedDevice& named : devices) {
        gfx::rhi::IDevice& device = *named.device;
        // A flat 64 x 64 terrain of 2 x 2 chunks.
        TerrainSettings settings;
        settings.chunk_world_size = 32.0f;
        settings.verts_per_chunk = 33;
        TerrainData data;
        data.chunk_size = 33;
        Heightmap hm;
        hm.width = hm.height = 65;
        hm.cell_size = 1.0f;
        hm.heights.assign(65u * 65u, 0.0f);
        InitTerrainData(data, hm, settings);
        AETHER_CHECK(data.chunk_count_x == 2 && data.chunk_count_z == 2);

        std::unique_ptr<gfx::rhi::ISwapChain> target = device.CreateSwapChain(nullptr, kW, kH, 2);
        RhiTerrainGpu gpu(device);
        TerrainRenderer renderer(gpu);
        renderer.Build(data, settings);
        AETHER_CHECK(renderer.UploadedCount() == 4 && gpu.Live() == 8); // a vertex and an index buffer for each chunk

        TerrainPass pass(device, *target);
        AETHER_CHECK(pass.Valid());
        TerrainDrawParams params;
        params.height_contrast = 0.9f;
        params.camera_position = Vec3(32, 60, 32);
        for (u32 i = 0; i < 4; ++i) {
            const u8 texel[4] = {kLayers[i].rgb[0], kLayers[i].rgb[1], kLayers[i].rgb[2], kLayers[i].height};
            params.layers[i].albedo_height = device.CreateTexture(1, 1, texel);
            params.layers[i].tile_scale = 0.2f;
            AETHER_CHECK(params.layers[i].albedo_height.IsValid());
        }
        // The splatmap: top-left layer 0, top-right layer 1, bottom-left an even mix of them, bottom-right 60% layer 2 and 40% layer 3.
        const SplatmapLayer proto{};
        std::vector<SplatmapLayer> layers(4, proto);
        constexpr u32 kSplat = 8;
        std::vector<f32> weights(static_cast<usize>(kSplat) * kSplat * 4, 0.0f);
        for (u32 z = 0; z < kSplat; ++z) {
            for (u32 x = 0; x < kSplat; ++x) {
                f32* w = &weights[(static_cast<usize>(z) * kSplat + x) * 4];
                const bool right = x >= kSplat / 2, bottom = z >= kSplat / 2;
                if (!bottom) w[right ? 1 : 0] = 1.0f;
                else if (!right) w[0] = 0.5f, w[1] = 0.5f;
                else w[2] = 0.6f, w[3] = 0.4f;
            }
        }
        const SplatmapData splat = BuildSplatmap(kSplat, layers, weights);
        params.splatmap = UploadSplatmap(device, splat);
        AETHER_CHECK(params.splatmap.IsValid());
        params.splat_origin[0] = params.splat_origin[1] = 0.0f;
        params.splat_size[0] = params.splat_size[1] = 64.0f;

        const Mat4 view = Mat4::LookAtRH(Vec3(32, 60, 32), Vec3(32, 0, 32), Vec3(0, 0, -1));
        const Mat4 view_projection = Mat4::PerspectiveRH(60.0f * 3.14159265f / 180.0f, static_cast<f32>(kW) / kH, 0.1f, 200.0f) * view;

        std::unique_ptr<gfx::rhi::ICommandList> cmd = device.CreateCommandList();
        target->AcquireNextImage();
        cmd->Reset();
        cmd->BeginRenderPass(*target, {0.0f, 0.0f, 0.0f, 1.0f});
        const u32 drawn = pass.Draw(*cmd, gpu, renderer.Chunks(), view_projection, params);
        cmd->EndRenderPass();
        cmd->Close();
        device.WaitForFence(device.Submit(*cmd, target.get()));
        std::vector<u8> pixels;
        AETHER_CHECK(target->ReadBack(pixels));
        target->Present(false);
        AETHER_CHECK(drawn == 4 && pixels.size() == static_cast<usize>(kW) * kH * 4);
        if (pixels.size() != static_cast<usize>(kW) * kH * 4) continue;

        std::array<Vec3, 4> albedo;
        std::array<f32, 4> heights;
        for (usize i = 0; i < 4; ++i) {
            albedo[i] = Vec3(SrgbToLinear(kLayers[i].rgb[0] / 255.0f), SrgbToLinear(kLayers[i].rgb[1] / 255.0f), SrgbToLinear(kLayers[i].rgb[2] / 255.0f));
            heights[i] = kLayers[i].height / 255.0f;
        }
        const char* names[4] = {"top left", "top right", "bottom left", "bottom right"};
        const f32 centres[4][2] = {{16, 16}, {48, 16}, {16, 48}, {48, 48}}; // world x, z
        for (int q = 0; q < 4; ++q) {
            const Vec4 clip = view_projection * Vec4(centres[q][0], 0.0f, centres[q][1], 1.0f);
            const u32 px = static_cast<u32>((clip.x / clip.w * 0.5f + 0.5f) * kW), py = static_cast<u32>((0.5f - clip.y / clip.w * 0.5f) * kH);
            const u8* got = &pixels[(static_cast<usize>(py) * kW + px) * 4];
            const std::array<f32, 4> painted = splat.GetWeights(centres[q][0] / 64.0f, centres[q][1] / 64.0f);
            const std::array<u8, 3> want = TerrainReferenceColor(albedo, painted, heights, params, Vec3(centres[q][0], 0.0f, centres[q][1]));
            const bool ok = std::abs(got[0] - want[0]) <= 4 && std::abs(got[1] - want[1]) <= 4 && std::abs(got[2] - want[2]) <= 4;
            if (!ok) {
                std::printf("    %s, %s quadrant: got (%u, %u, %u), want (%u, %u, %u)\n", named.name, names[q], got[0], got[1], got[2], want[0], want[1],
                            want[2]);
            }
            AETHER_CHECK(ok);
        }
        // The mixed quadrant is sensitive to the heights: without them the colour would be the plain average of the two layers.
        TerrainDrawParams flat_params = params;
        flat_params.height_contrast = 0.0f;
        const std::array<f32, 4> mixed = splat.GetWeights(16.0f / 64.0f, 48.0f / 64.0f);
        const std::array<u8, 3> sharp = TerrainReferenceColor(albedo, mixed, heights, params, Vec3(16, 0, 48));
        const std::array<u8, 3> plain = TerrainReferenceColor(albedo, mixed, heights, flat_params, Vec3(16, 0, 48));
        AETHER_CHECK(std::abs(sharp[0] - plain[0]) + std::abs(sharp[1] - plain[1]) > 30);
        // A corner of the image is outside the terrain: the clear colour.
        AETHER_CHECK(pixels[0] == 0 && pixels[1] == 0 && pixels[2] == 0);
        std::printf("    %s: terrain drawn, 4 quadrants match\n", named.name);
    }
}
#endif
