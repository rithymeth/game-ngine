#include "aether/core/log.h"
#include "aether/cook/cooker.h"
#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"
#include "aether/player/game.h"
#include "aether/player/scene_renderer.h"
#include "aether/player/user_paths.h"
#include "aether/reflection/serialize.h"
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
#include <vector>

using namespace aether;
namespace fs = std::filesystem;
namespace rhi = aether::gfx::rhi;

// The player's whole frame (SceneRenderer: clustered lights, cascaded shadows, sky and image-based lighting, HDR, bloom, temporal anti-aliasing,
// tone mapping) on D3D12 and on Vulkan, from one cooked project, compared. Each of those was once missing or wrong on Vulkan alone (the sky
// shader's push constants, the light buffer's binding, mip levels, the viewport's flip); a screenshot caught each, and this does instead.
// Set AETHER_PARITY_OUT to a directory to keep the two frames (BMP).

namespace {

reflect::Json SceneEntity(reflect::Json components) { return {{"guid", assets::ToString(assets::NewAssetGuid())}, {"components", std::move(components)}}; }

reflect::Json TransformJson(std::initializer_list<f32> position, std::initializer_list<f32> rotation = {0, 0, 0, 1}, f32 scale = 1.0f) {
    return {{"$v", 1}, {"position", position}, {"rotation", rotation}, {"scale", {scale, scale, scale}}};
}

reflect::Json Cube(const char* model, std::initializer_list<f32> position, std::initializer_list<f32> rotation, f32 scale) {
    return SceneEntity({{"Transform", TransformJson(position, rotation, scale)},
                        {"ModelRenderer", {{"$v", 1}, {"model", ""}, {"asset_path", model}}}});
}

// Renders `frames` frames of the cooked project on `backend` and returns the last one (RGBA8); empty when the backend is unavailable.
std::vector<u8> RenderProject(rhi::Backend backend, const fs::path& root, const fs::path& pak_file, u32 width, u32 height, u32 frames) {
    if (!rhi::IsBackendAvailable(backend)) return {};
    std::unique_ptr<rhi::IDevice> device;
    try {
        device = rhi::CreateDevice(backend, false);
    } catch (const std::exception&) {
    }
    if (!device) return {};
    player::GamePackage package;
    std::string error;
    AETHER_CHECK(package.Mount(pak_file.string(), 0, &error) && package.LoadManifest(&error));
    player::Game game(package);
    game.SetUserPaths(player::ResolveUserPaths("BackendParity", root / (backend == rhi::Backend::Vulkan ? "user_vk" : "user_dx")));
    AETHER_CHECK(game.LoadStartupScene(&error));
    game.BeginPlay();
    std::unique_ptr<rhi::ISwapChain> target = device->CreateSwapChain(nullptr, width, height, 2);
    std::vector<u8> pixels;
    {
        player::SceneRenderer renderer(*device, *target, package);
        std::unique_ptr<rhi::ICommandList> cmd = device->CreateCommandList();
        for (u32 frame = 0; frame < frames; ++frame) {
            game.Tick(1.0f / 60.0f);
            target->AcquireNextImage();
            cmd->Reset();
            renderer.Draw(game, *cmd, frame % 2);
            cmd->Close();
            device->WaitForFence(device->Submit(*cmd, target.get()));
            if (frame + 1 == frames) AETHER_CHECK(target->ReadBack(pixels) && pixels.size() == static_cast<usize>(width) * height * 4);
            target->Present(false);
        }
    }
    return pixels;
}

void WriteBmp(const fs::path& path, u32 width, u32 height, const std::vector<u8>& rgba) {
    std::ofstream file(path, std::ios::binary);
    const u32 size = 54 + width * height * 4;
    const u8 header[54] = {'B', 'M', u8(size), u8(size >> 8), u8(size >> 16), u8(size >> 24), 0, 0, 0, 0, 54, 0, 0, 0, 40, 0, 0, 0,
                           u8(width), u8(width >> 8), u8(width >> 16), u8(width >> 24), u8(height), u8(height >> 8), u8(height >> 16), u8(height >> 24),
                           1, 0, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    file.write(reinterpret_cast<const char*>(header), sizeof(header));
    for (u32 y = height; y-- > 0;) {
        for (u32 x = 0; x < width; ++x) {
            const u8* p = &rgba[(static_cast<usize>(y) * width + x) * 4];
            const u8 bgra[4] = {p[2], p[1], p[0], 255};
            file.write(reinterpret_cast<const char*>(bgra), 4);
        }
    }
}

} // namespace

AETHER_TEST(Player_AWholeFrameLooksTheSameOnD3D12AndVulkan) {
    if (!rhi::IsBackendAvailable(rhi::Backend::D3D12) || !rhi::IsBackendAvailable(rhi::Backend::Vulkan)) return;

    const fs::path root = fs::temp_directory_path() / "aether_backend_parity" / assets::ToString(assets::NewAssetGuid());
    fs::create_directories(root / "Content/models");
    fs::create_directories(root / "Content/Scenes");
    const fs::path games = fs::path(AETHER_REPO_ASSETS_DIR).parent_path() / "games/AETHER-01";
    for (const char* name : {"aether_greybox_core.gltf", "aether_greybox_armor.gltf", "aether_greybox_rifle.gltf", "test_cube.bin"}) {
        fs::copy_file(games / "Content/models" / name, root / "Content/models" / name, fs::copy_options::overwrite_existing);
    }
    reflect::Json project = reflect::Json::parse(std::ifstream(games / "AETHER-01.aproject"));
    project["name"] = "BackendParity";
    project["startup_scene"] = "Scenes/Main.ascene";
    project["always_cook"] = reflect::Json::array();
    std::ofstream(root / "BackendParity.aproject") << project.dump(2);

    // A camera looking down a little at a floor (a flattened cube) with three cubes on it; the sun (with shadows), a sky light (the
    // procedural sky), and a red point light beside the middle cube.
    const reflect::Json scene = {{"$type", "Scene"}, {"$version", 1}, {"entities", {
        SceneEntity({{"Transform", TransformJson({0.0f, 1.6f, 4.2f}, {-0.16f, 0.0f, 0.0f, 0.987f})},
                     {"Camera", {{"$v", 1}, {"projection", "Perspective"}, {"fov_degrees", 50.0f}, {"near_plane", 0.1f}, {"far_plane", 100.0f},
                                 {"ortho_height", 10.0f}, {"priority", 0}}}}),
        SceneEntity({{"Transform", TransformJson({0, 0, 0}, {-0.4f, 0.35f, 0.15f, 0.83f})},
                     {"DirectionalLight", {{"$v", 1}, {"color", {1.0f, 0.95f, 0.88f}}, {"intensity", 3.0f}, {"cast_shadows", true}}}}),
        SceneEntity({{"Transform", TransformJson({0, 0, 0})},
                     {"SkyLight", {{"$v", 1}, {"color", {1.0f, 1.0f, 1.0f}}, {"intensity", 1.0f}, {"environment", ""},
                                   {"environment_rotation_degrees", 0.0f}}}}),
        SceneEntity({{"Transform", TransformJson({0.5f, 0.7f, 1.1f})},
                     {"PointLight", {{"$v", 1}, {"color", {1.0f, 0.15f, 0.05f}}, {"intensity", 60.0f}, {"range", 8.0f}, {"cast_shadows", false}}}}),
        Cube("models/aether_greybox_rifle.gltf", {0.0f, -0.3f, 0.0f}, {0, 0, 0, 1}, 12.0f), // the floor
        Cube("models/aether_greybox_core.gltf", {-1.2f, 0.6f, 0.0f}, {0.0f, 0.38f, 0.0f, 0.92f}, 3.0f),
        Cube("models/aether_greybox_armor.gltf", {0.0f, 0.55f, 0.3f}, {0.0f, -0.2f, 0.0f, 0.98f}, 0.3f),
        Cube("models/aether_greybox_core.gltf", {1.3f, 0.6f, -0.5f}, {0.0f, 0.1f, 0.0f, 0.99f}, 3.0f)}}};
    std::ofstream(root / "Content/Scenes/Main.ascene") << scene.dump(2);

    cook::CookOptions options;
    options.project_file = root / "BackendParity.aproject";
    options.output_dir = root / "cooked";
    const auto cooked = cook::Cook(options);
    AETHER_CHECK(cooked.ok);
    if (!cooked.ok) return;

    constexpr u32 kWidth = 480, kHeight = 270, kFrames = 12;
    const std::vector<u8> dx = RenderProject(rhi::Backend::D3D12, root, cooked.pak_file, kWidth, kHeight, kFrames);
    const std::vector<u8> vk = RenderProject(rhi::Backend::Vulkan, root, cooked.pak_file, kWidth, kHeight, kFrames);
    AETHER_CHECK(dx.size() == static_cast<usize>(kWidth) * kHeight * 4 && vk.size() == dx.size());
    if (dx.size() != vk.size() || dx.empty()) return;
    if (const char* out = std::getenv("AETHER_PARITY_OUT")) {
        fs::create_directories(out);
        WriteBmp(fs::path(out) / "player_frame_d3d12.bmp", kWidth, kHeight, dx);
        WriteBmp(fs::path(out) / "player_frame_vulkan.bmp", kWidth, kHeight, vk);
    }

    double total_difference = 0.0;
    usize far_pixels = 0;
    for (usize i = 0; i < dx.size(); i += 4) {
        int difference = 0;
        for (usize c = 0; c < 3; ++c) difference = std::max(difference, std::abs(static_cast<int>(dx[i + c]) - static_cast<int>(vk[i + c])));
        total_difference += difference;
        if (difference > 48) ++far_pixels;
    }
    const usize pixels = dx.size() / 4;
    const double mean_difference = total_difference / static_cast<double>(pixels);
    const double far_fraction = static_cast<double>(far_pixels) / static_cast<double>(pixels);
    std::printf("    mean difference %.2f / 255, %.2f%% of pixels over 48\n", mean_difference, 100.0 * far_fraction);
    AETHER_CHECK(mean_difference < 3.0);
    AETHER_CHECK(far_fraction < 0.01);

    // Parity of a frame with nothing in it would pass, so the frame has to show the features the two backends are being compared on.
    const auto brightness = [&](const std::vector<u8>& image, u32 x, u32 y) {
        const u8* p = &image[(static_cast<usize>(y) * kWidth + x) * 4];
        return (static_cast<int>(p[0]) + p[1] + p[2]) / 3;
    };
    for (const std::vector<u8>* image : {&dx, &vk}) {
        // The sky: a gradient down the left edge, from the zenith to the horizon (the HDR background pass and the sky's environment).
        AETHER_CHECK(std::abs(brightness(*image, 8, 4) - brightness(*image, 8, 100)) > 15);
        // The red point light: pixels clearly redder than they are green or blue (the clustered light buffer).
        usize red = 0, dark = 0;
        for (usize i = 0; i < image->size(); i += 4) {
            const int r = (*image)[i], g = (*image)[i + 1], b = (*image)[i + 2];
            if (r > g + 20 && r > b + 20) ++red;
            if (r + g + b < 150) ++dark; // shadow and unlit sides, on the floor and the cubes
        }
        AETHER_CHECK(red > 50);
        AETHER_CHECK(dark > 200);
    }
    std::error_code ignored;
    fs::remove_all(root, ignored);
}

namespace {

void SetEnvironmentVariableForTest(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value); // an empty value removes it
#else
    if (*value != '\0') setenv(name, value, 1);
    else unsetenv(name);
#endif
}

} // namespace

// A packaged sky's cooked environment (Cooked/<guid>.aenv) lights and draws exactly like the one the player prefilters from the .hdr at load,
// and the player takes it when it is there.
AETHER_TEST(Player_ACookedSkyLooksTheSameAsOnePrefilteredAtLoad) {
    const rhi::Backend backend = rhi::IsBackendAvailable(rhi::Backend::D3D12) ? rhi::Backend::D3D12 : rhi::Backend::Vulkan;
    if (!rhi::IsBackendAvailable(backend)) return;

    const fs::path root = fs::temp_directory_path() / "aether_cooked_sky" / assets::ToString(assets::NewAssetGuid());
    fs::create_directories(root / "Content/models");
    fs::create_directories(root / "Content/Scenes");
    fs::create_directories(root / "Content/skies");
    const fs::path games = fs::path(AETHER_REPO_ASSETS_DIR).parent_path() / "games/AETHER-01";
    for (const char* name : {"aether_greybox_core.gltf", "test_cube.bin"}) {
        fs::copy_file(games / "Content/models" / name, root / "Content/models" / name, fs::copy_options::overwrite_existing);
    }
    // A 256 x 128 Radiance sky, flat scanlines: a blue-to-orange gradient with a bright sun patch.
    constexpr u32 kSkyWidth = 256, kSkyHeight = 128;
    std::string hdr = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 128 +X 256\n";
    for (u32 y = 0; y < kSkyHeight; ++y) {
        for (u32 x = 0; x < kSkyWidth; ++x) {
            const bool sun = x >= 150 && x < 160 && y >= 40 && y < 50;
            const int red = sun ? 250 : 40 + static_cast<int>(y) * 1, blue = sun ? 180 : 200 - static_cast<int>(y) * 1;
            for (const int b : {red, 100, blue, sun ? 140 : 128}) hdr.push_back(static_cast<char>(b));
        }
    }
    std::ofstream(root / "Content/skies/sky.hdr", std::ios::binary) << hdr;
    reflect::Json project = reflect::Json::parse(std::ifstream(games / "AETHER-01.aproject"));
    project["name"] = "BackendParity";
    project["startup_scene"] = "Scenes/Main.ascene";
    project["always_cook"] = reflect::Json::array();
    std::ofstream(root / "BackendParity.aproject") << project.dump(2);
    const reflect::Json scene = {{"$type", "Scene"}, {"$version", 1}, {"entities", {
        SceneEntity({{"Transform", TransformJson({0.0f, 1.0f, 3.0f})},
                     {"Camera", {{"$v", 1}, {"projection", "Perspective"}, {"fov_degrees", 70.0f}, {"near_plane", 0.1f}, {"far_plane", 100.0f},
                                 {"ortho_height", 10.0f}, {"priority", 0}}}}),
        SceneEntity({{"Transform", TransformJson({0, 0, 0})},
                     {"SkyLight", {{"$v", 1}, {"color", {1.0f, 1.0f, 1.0f}}, {"intensity", 1.0f}, {"environment", "skies/sky.hdr"},
                                   {"environment_rotation_degrees", 0.0f}}}}),
        Cube("models/aether_greybox_core.gltf", {0.0f, 1.0f, 0.0f}, {0.0f, 0.3f, 0.0f, 0.95f}, 4.0f)}}};
    std::ofstream(root / "Content/Scenes/Main.ascene") << scene.dump(2);

    cook::CookOptions options;
    options.project_file = root / "BackendParity.aproject";
    options.output_dir = root / "cooked";
    const auto cooked = cook::Cook(options);
    AETHER_CHECK(cooked.ok);
    if (!cooked.ok) return;

    constexpr u32 kWidth = 320, kHeight = 180, kFrames = 6;
    const auto render = [&](const char* cooked_sky, bool& took_cooked_path, bool& prefiltered) {
        SetEnvironmentVariableForTest("AETHER_RENDER_COOKED_SKY", cooked_sky);
        bool cooked_line = false, prefilter_line = false;
        const int sink = Logger::Instance().AddSink([&](const LogLine& line) {
            if (line.message.find("Sky environment 'skies/sky.hdr'") == std::string::npos) return;
            if (line.message.find("cooked, loaded") != std::string::npos) cooked_line = true;
            if (line.message.find("prefiltered in") != std::string::npos) prefilter_line = true;
        });
        std::vector<u8> pixels = RenderProject(backend, root, cooked.pak_file, kWidth, kHeight, kFrames);
        Logger::Instance().RemoveSink(sink);
        took_cooked_path = cooked_line;
        prefiltered = prefilter_line;
        return pixels;
    };
    bool used_cooked = false, prefiltered = false, used_cooked_off = false, prefiltered_off = false;
    const std::vector<u8> with_cooked = render("", used_cooked, prefiltered);
    const std::vector<u8> without = render("0", used_cooked_off, prefiltered_off);
    SetEnvironmentVariableForTest("AETHER_RENDER_COOKED_SKY", "");
    AETHER_CHECK(used_cooked && !prefiltered);          // the cooked environment, no prefilter
    AETHER_CHECK(!used_cooked_off && prefiltered_off);  // switched off: the .hdr prefiltered at load
    AETHER_CHECK(with_cooked.size() == static_cast<usize>(kWidth) * kHeight * 4 && without.size() == with_cooked.size());
    if (with_cooked.size() == without.size()) {
        AETHER_CHECK(with_cooked == without); // the same pixels, not close ones: the cooked data is what the prefilter makes
        // And the frame has the sky in it: not one flat colour.
        int lowest = 255, highest = 0;
        for (usize i = 0; i < with_cooked.size(); i += 4) {
            lowest = std::min<int>(lowest, with_cooked[i + 2]);
            highest = std::max<int>(highest, with_cooked[i + 2]);
        }
        AETHER_CHECK(highest - lowest > 40);
    }
    std::error_code ignored;
    fs::remove_all(root, ignored);
}
