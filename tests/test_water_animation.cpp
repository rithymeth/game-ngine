#include "aether/assets/png_writer.h"
#include "aether/assets/procedural_mesh.h"
#include "aether/cook/cooker.h"
#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"
#include "aether/player/game.h"
#include "aether/player/scene_renderer.h"
#include "aether/player/user_paths.h"
#include "aether/reflection/serialize.h"
#include "test_framework.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace aether;
using namespace aether::assets::procedural;
namespace fs = std::filesystem;
namespace rhi = aether::gfx::rhi;

// Animated water through the whole renderer: a material flagged AETHER_materials_water scrolls its normal map with the game's time (the scene
// shader reads the time from the frame constants), and a material that is not flagged does not. The same lit quad, drawn at two game times.

namespace {

reflect::Json SceneEntity(reflect::Json components) { return {{"guid", assets::ToString(assets::NewAssetGuid())}, {"components", std::move(components)}}; }

reflect::Json TransformJson(std::initializer_list<f32> position, std::initializer_list<f32> rotation = {0, 0, 0, 1}) {
    return {{"$v", 1}, {"position", position}, {"rotation", rotation}, {"scale", {1.0f, 1.0f, 1.0f}}};
}

void SetEnvironmentVariableForTest(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    if (*value != '\0') setenv(name, value, 1);
    else unsetenv(name);
#endif
}

// A flat quad 12 m across facing up, with a periodic ripple normal map (a few crossing sine waves), a glossy blue material, and the water flag as asked.
fs::path CookWaterScene(const fs::path& root, bool water) {
    fs::create_directories(root / "Content/models");
    fs::create_directories(root / "Content/Scenes");
    const fs::path games = fs::path(AETHER_REPO_ASSETS_DIR).parent_path() / "games/AETHER-01";
    reflect::Json project = reflect::Json::parse(std::ifstream(games / "AETHER-01.aproject"));
    project["name"] = "WaterScene";
    project["startup_scene"] = "Scenes/Main.ascene";
    project["always_cook"] = reflect::Json::array();
    std::ofstream(root / "WaterScene.aproject") << project.dump(2);

    // The ripples: 64 x 64, each wave a whole number of crests across, so the map tiles.
    constexpr u32 N = 64;
    std::vector<u8> rgb(static_cast<usize>(N) * N * 3);
    const f32 two_pi = 6.2831853f;
    for (u32 y = 0; y < N; ++y)
        for (u32 x = 0; x < N; ++x) {
            const f32 u = static_cast<f32>(x) / N, v = static_cast<f32>(y) / N;
            f32 dx = 0, dz = 0;
            const f32 waves[4][3] = {{5, 2, 0.3f}, {-3, 6, 1.7f}, {8, -4, 2.9f}, {2, 9, 4.1f}};
            for (const auto& wave : waves) {
                const f32 c = std::cos(two_pi * (wave[0] * u + wave[1] * v) + wave[2]);
                dx += c * wave[0] * 0.05f;
                dz += c * wave[1] * 0.05f;
            }
            const f32 inv = 1.0f / std::sqrt(dx * dx + dz * dz + 1.0f);
            u8* p = &rgb[(static_cast<usize>(y) * N + x) * 3];
            p[0] = static_cast<u8>(std::clamp(-dx * inv * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
            p[1] = static_cast<u8>(std::clamp(-dz * inv * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
            p[2] = static_cast<u8>(std::clamp(inv * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    const std::vector<u8> png = assets::EncodePngRgb8(N, N, rgb);
    std::ofstream(root / "Content/models/ripple.png", std::ios::binary).write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));

    MeshBuilder quad;
    quad.Quad(quad.Vertex(Vec3(-6, 0, 6), 0, 1), quad.Vertex(Vec3(6, 0, 6), 1, 1), quad.Vertex(Vec3(6, 0, -6), 1, 0), quad.Vertex(Vec3(-6, 0, -6), 0, 0));
    ProceduralMesh mesh;
    assets::MaterialData material;
    material.base_color = Vec4(0.05f, 0.2f, 0.4f, 1.0f);
    material.roughness = 0.08f;
    material.metallic = 0.0f;
    material.normal_texture = "ripple.png";
    material.normal_scale = 1.6f;
    material.water = water;
    mesh.materials.push_back(material);
    mesh.mesh.primitives.push_back(quad.Finish(Shading::Smooth, UvMode::Given, 0));
    const std::vector<u8> glb = EncodeGlb(mesh);
    std::ofstream(root / "Content/models/water.glb", std::ios::binary).write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));

    const reflect::Json entities = {
        SceneEntity({{"Transform", TransformJson({0.0f, 5.5f, 9.0f}, {-0.30f, 0.0f, 0.0f, 0.954f})},
                     {"Camera", {{"$v", 1}, {"projection", "Perspective"}, {"fov_degrees", 50.0f}, {"near_plane", 0.1f}, {"far_plane", 100.0f},
                                 {"ortho_height", 10.0f}, {"priority", 0}}}}),
        SceneEntity({{"Transform", TransformJson({0, 0, 0}, {-0.26f, 0.9f, 0.1f, 0.34f})},
                     {"DirectionalLight", {{"$v", 1}, {"color", {1.0f, 0.95f, 0.88f}}, {"intensity", 3.0f}, {"cast_shadows", false}}}}),
        SceneEntity({{"Transform", TransformJson({0, 0, 0})},
                     {"SkyLight", {{"$v", 1}, {"color", {1.0f, 1.0f, 1.0f}}, {"intensity", 1.0f}, {"environment", ""}, {"environment_rotation_degrees", 0.0f}}}}),
        SceneEntity({{"Transform", TransformJson({0, 0, 0})}, {"ModelRenderer", {{"$v", 1}, {"model", ""}, {"asset_path", "models/water.glb"}}}})};
    const reflect::Json scene = {{"$type", "Scene"}, {"$version", 1}, {"entities", entities}};
    std::ofstream(root / "Content/Scenes/Main.ascene") << scene.dump(2);
    cook::CookOptions options;
    options.project_file = root / "WaterScene.aproject";
    options.output_dir = root / "cooked";
    const auto cooked = cook::Cook(options);
    AETHER_CHECK(cooked.ok);
    return cooked.ok ? cooked.pak_file : fs::path();
}

// The frame after `ticks` fixed ticks of game time (60 a second), as RGBA.
std::vector<u8> FrameAfter(rhi::Backend backend, const fs::path& root, const fs::path& pak_file, u32 ticks, u32 width, u32 height) {
    std::vector<u8> pixels;
    std::unique_ptr<rhi::IDevice> device;
    try {
        device = rhi::CreateDevice(backend, false);
    } catch (const std::exception&) {
    }
    if (!device) return pixels;
    player::GamePackage package;
    std::string error;
    AETHER_CHECK(package.Mount(pak_file.string(), 0, &error) && package.LoadManifest(&error));
    player::Game game(package);
    game.SetUserPaths(player::ResolveUserPaths("WaterScene", root / "user"));
    AETHER_CHECK(game.LoadStartupScene(&error));
    game.BeginPlay();
    for (u32 tick = 0; tick < ticks; ++tick) game.Tick(1.0f / 60.0f);
    std::unique_ptr<rhi::ISwapChain> target = device->CreateSwapChain(nullptr, width, height, 2);
    {
        player::SceneRenderer renderer(*device, *target, package);
        std::unique_ptr<rhi::ICommandList> cmd = device->CreateCommandList();
        // A few frames at the same game time (the renderer's own resources settle in the first), the last one read.
        for (u32 frame = 0; frame < 4; ++frame) {
            target->AcquireNextImage();
            cmd->Reset();
            renderer.Draw(game, *cmd, frame % 2);
            cmd->Close();
            device->WaitForFence(device->Submit(*cmd, target.get()));
            if (frame == 3) AETHER_CHECK(target->ReadBack(pixels) && pixels.size() == static_cast<usize>(width) * height * 4);
            target->Present(false);
        }
    }
    return pixels;
}

double MeanDifference(const std::vector<u8>& a, const std::vector<u8>& b) {
    double total = 0.0;
    for (usize i = 0; i < a.size(); i += 4) {
        int difference = 0;
        for (usize c = 0; c < 3; ++c) difference = std::max(difference, std::abs(static_cast<int>(a[i + c]) - static_cast<int>(b[i + c])));
        total += difference;
    }
    return total / static_cast<double>(a.size() / 4);
}

} // namespace

// A tuft of grass close to the camera on a lit ground quad, with the wind flag as asked; the grass is scaled up (a tuft is half a metre) so it fills the view.
fs::path CookGrassScene(const fs::path& root, bool wind, f32 model_yaw_degrees = 0.0f, const reflect::Json* wind_component = nullptr, bool with_model = true) {
    fs::create_directories(root / "Content/models");
    fs::create_directories(root / "Content/Scenes");
    const fs::path games = fs::path(AETHER_REPO_ASSETS_DIR).parent_path() / "games/AETHER-01";
    reflect::Json project = reflect::Json::parse(std::ifstream(games / "AETHER-01.aproject"));
    project["name"] = "WaterScene";
    project["startup_scene"] = "Scenes/Main.ascene";
    project["always_cook"] = reflect::Json::array();
    std::ofstream(root / "WaterScene.aproject") << project.dump(2);
    GrassParams grass;
    grass.blades = 30;
    grass.height = 0.6f;
    grass.spread = 0.12f;
    grass.curl = 0.4f;
    grass.sway = wind ? 0.30f : 0.0f; // the strongest the packed strength can carry
    const std::vector<u8> glb = EncodeGlb(GenerateGrass(grass, 4));
    std::ofstream(root / "Content/models/grass.glb", std::ios::binary).write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
    const reflect::Json entities = {
        SceneEntity({{"Transform", TransformJson({0.0f, 0.5f, 1.7f}, {-0.12f, 0.0f, 0.0f, 0.993f})},
                     {"Camera", {{"$v", 1}, {"projection", "Perspective"}, {"fov_degrees", 50.0f}, {"near_plane", 0.05f}, {"far_plane", 50.0f},
                                 {"ortho_height", 10.0f}, {"priority", 0}}}}),
        SceneEntity({{"Transform", TransformJson({0, 0, 0}, {-0.26f, 0.9f, 0.1f, 0.34f})},
                     {"DirectionalLight", {{"$v", 1}, {"color", {1.0f, 0.95f, 0.88f}}, {"intensity", 3.0f}, {"cast_shadows", false}}}}),
        SceneEntity({{"Transform", TransformJson({0, 0, 0})},
                     {"SkyLight", {{"$v", 1}, {"color", {1.0f, 1.0f, 1.0f}}, {"intensity", 1.0f}, {"environment", ""}, {"environment_rotation_degrees", 0.0f}}}}),
        SceneEntity({{"Transform", {{"$v", 1}, {"position", {0.0f, 0.0f, 0.0f}}, {"rotation", {0.0f, std::sin(model_yaw_degrees * 3.14159265f / 360.0f), 0.0f, std::cos(model_yaw_degrees * 3.14159265f / 360.0f)}},
                                    {"scale", {2.0f, 2.0f, 2.0f}}}},
                     {"ModelRenderer", {{"$v", 1}, {"model", ""}, {"asset_path", "models/grass.glb"}}}})};
    reflect::Json all = entities;
    if (!with_model) all.erase(all.size() - 1);
    if (wind_component != nullptr) all.push_back(SceneEntity({{"Transform", TransformJson({0, 0, 0})}, {"Wind", *wind_component}}));
    const reflect::Json scene = {{"$type", "Scene"}, {"$version", 1}, {"entities", all}};
    std::ofstream(root / "Content/Scenes/Main.ascene") << scene.dump(2);
    cook::CookOptions options;
    options.project_file = root / "WaterScene.aproject";
    options.output_dir = root / "cooked";
    const auto cooked = cook::Cook(options);
    AETHER_CHECK(cooked.ok);
    return cooked.ok ? cooked.pak_file : fs::path();
}

static void Water_RipplesMoveWithTheGameTimeAndOnlyOnWater(rhi::Backend backend) {
    if (!rhi::IsBackendAvailable(backend)) return;
    SetEnvironmentVariableForTest("AETHER_RENDER_TAA", "0"); // no sub-pixel jitter between frames: what differs is the water
    constexpr u32 W = 640, H = 360;
    const fs::path root = fs::temp_directory_path() / "aether_water_scene" / assets::ToString(assets::NewAssetGuid());
    const fs::path wet_pak = CookWaterScene(root / "wet", true), dry_pak = CookWaterScene(root / "dry", false);
    if (wet_pak.empty() || dry_pak.empty()) return;
    const auto wet_early = FrameAfter(backend, root / "wet", wet_pak, 10, W, H), wet_early_again = FrameAfter(backend, root / "wet", wet_pak, 10, W, H);
    const auto wet_late = FrameAfter(backend, root / "wet", wet_pak, 190, W, H);
    const auto dry_early = FrameAfter(backend, root / "dry", dry_pak, 10, W, H), dry_late = FrameAfter(backend, root / "dry", dry_pak, 190, W, H);
    SetEnvironmentVariableForTest("AETHER_RENDER_TAA", "");
    AETHER_CHECK(wet_early.size() == static_cast<usize>(W) * H * 4 && wet_late.size() == wet_early.size() && dry_early.size() == wet_early.size() && dry_late.size() == wet_early.size());
    if (wet_early.size() != wet_late.size() || dry_early.size() != wet_early.size() || dry_late.size() != wet_early.size() || wet_early.empty()) return;
    const double repeat = MeanDifference(wet_early, wet_early_again), moved = MeanDifference(wet_early, wet_late), still = MeanDifference(dry_early, dry_late);
    std::printf("    %s: the same time twice differs by %.4f, water 3 s apart by %.3f, a plain material 3 s apart by %.4f (mean of the channel's largest step, 0..255)\n",
                backend == rhi::Backend::Vulkan ? "vulkan" : "d3d12", repeat, moved, still);
    AETHER_CHECK(repeat < 0.01);            // the same game time is the same picture
    AETHER_CHECK(moved > 0.3);              // the ripples moved
    AETHER_CHECK(moved > still * 10.0 + 0.1); // and a material without the flag did not
    std::error_code ignored;
    fs::remove_all(root, ignored);
}

AETHER_TEST(Water_RipplesMoveWithTheGameTimeAndOnlyOnWater_D3D12) { Water_RipplesMoveWithTheGameTimeAndOnlyOnWater(rhi::Backend::D3D12); }
AETHER_TEST(Water_RipplesMoveWithTheGameTimeAndOnlyOnWater_Vulkan) { Water_RipplesMoveWithTheGameTimeAndOnlyOnWater(rhi::Backend::Vulkan); }

// Wind: a grass material with the extension sways with the game's time, one without does not, and the same time is the same picture.
static void Wind_GrassSwaysWithTheGameTimeAndOnlyWhenFlagged(rhi::Backend backend) {
    if (!rhi::IsBackendAvailable(backend)) return;
    SetEnvironmentVariableForTest("AETHER_RENDER_TAA", "0");
    constexpr u32 W = 640, H = 360;
    const fs::path root = fs::temp_directory_path() / "aether_wind_scene" / assets::ToString(assets::NewAssetGuid());
    const fs::path windy_pak = CookGrassScene(root / "windy", true), calm_pak = CookGrassScene(root / "calm", false);
    if (windy_pak.empty() || calm_pak.empty()) return;
    const auto windy_a = FrameAfter(backend, root / "windy", windy_pak, 10, W, H), windy_a_again = FrameAfter(backend, root / "windy", windy_pak, 10, W, H);
    const auto windy_b = FrameAfter(backend, root / "windy", windy_pak, 70, W, H);
    const auto calm_a = FrameAfter(backend, root / "calm", calm_pak, 10, W, H), calm_b = FrameAfter(backend, root / "calm", calm_pak, 70, W, H);
    SetEnvironmentVariableForTest("AETHER_RENDER_TAA", "");
    AETHER_CHECK(windy_a.size() == static_cast<usize>(W) * H * 4 && windy_b.size() == windy_a.size() && calm_a.size() == windy_a.size() && calm_b.size() == windy_a.size());
    if (windy_a.size() != windy_b.size() || calm_a.size() != windy_a.size() || calm_b.size() != windy_a.size() || windy_a.empty()) return;
    const double repeat = MeanDifference(windy_a, windy_a_again), swayed = MeanDifference(windy_a, windy_b), still = MeanDifference(calm_a, calm_b);
    std::printf("    %s: the same time twice differs by %.4f, flagged grass one second apart by %.3f, unflagged by %.4f\n",
                backend == rhi::Backend::Vulkan ? "vulkan" : "d3d12", repeat, swayed, still);
    AETHER_CHECK(repeat < 0.01);
    AETHER_CHECK(swayed > 0.05);
    AETHER_CHECK(swayed > still * 10.0 + 0.02);
    std::error_code ignored;
    fs::remove_all(root, ignored);
}

AETHER_TEST(Wind_GrassSwaysWithTheGameTimeAndOnlyWhenFlagged_D3D12) { Wind_GrassSwaysWithTheGameTimeAndOnlyWhenFlagged(rhi::Backend::D3D12); }
AETHER_TEST(Wind_GrassSwaysWithTheGameTimeAndOnlyWhenFlagged_Vulkan) { Wind_GrassSwaysWithTheGameTimeAndOnlyWhenFlagged(rhi::Backend::Vulkan); }

// Where the grass's picture changed between two game times, as the centroid of what differs from the same scene without the grass: how far it moved along the
// screen's x and y, in pixels (the signed shift of the weighted centre).
struct Shift { double x = 0.0, y = 0.0, weight = 0.0; };
Shift CentroidShift(const std::vector<u8>& before, const std::vector<u8>& after, const std::vector<u8>& background, u32 width, u32 height) {
    auto centroid = [&](const std::vector<u8>& frame, double& cx, double& cy, double& total) {
        cx = cy = total = 0.0;
        for (u32 y = 0; y < height; ++y)
            for (u32 x = 0; x < width; ++x) {
                const usize i = (static_cast<usize>(y) * width + x) * 4;
                int d = 0;
                for (usize c = 0; c < 3; ++c) d = std::max(d, std::abs(static_cast<int>(frame[i + c]) - static_cast<int>(background[i + c])));
                if (d < 12) continue; // the ground and its light, not the grass
                cx += static_cast<double>(x) * d;
                cy += static_cast<double>(y) * d;
                total += d;
            }
        if (total > 0.0) { cx /= total; cy /= total; }
    };
    double ax, ay, at, bx, by, bt;
    centroid(before, ax, ay, at);
    centroid(after, bx, by, bt);
    return {bx - ax, by - ay, std::min(at, bt)};
}

// The wind is a direction in the world: a tuft turned half way round sways the same way on screen as one that is not, and turning the wind turns the sway.
static void Wind_TheComponentBlowsInTheWorldWhicheverWayAnObjectFaces(rhi::Backend backend) {
    if (!rhi::IsBackendAvailable(backend)) return;
    SetEnvironmentVariableForTest("AETHER_RENDER_TAA", "0");
    constexpr u32 W = 640, H = 360;
    const fs::path root = fs::temp_directory_path() / "aether_wind_direction" / assets::ToString(assets::NewAssetGuid());
    const reflect::Json east = {{"$v", 1}, {"direction_degrees", 0.0f}, {"strength", 2.0f}, {"gust_speed", 1.0f}, {"gustiness", 0.4f}};
    const reflect::Json toward_camera = {{"$v", 1}, {"direction_degrees", 90.0f}, {"strength", 2.0f}, {"gust_speed", 1.0f}, {"gustiness", 0.4f}};
    const fs::path east_pak = CookGrassScene(root / "east", true, 0.0f, &east), turned_pak = CookGrassScene(root / "turned", true, 180.0f, &east);
    const fs::path south_pak = CookGrassScene(root / "south", true, 0.0f, &toward_camera), empty_pak = CookGrassScene(root / "empty", true, 0.0f, &east, false);
    if (east_pak.empty() || turned_pak.empty() || south_pak.empty() || empty_pak.empty()) return;
    const auto background = FrameAfter(backend, root / "empty", empty_pak, 0, W, H);
    // Two game times far enough apart in the gust that the tips are on different sides of their rest.
    const auto east_a = FrameAfter(backend, root / "east", east_pak, 0, W, H), east_b = FrameAfter(backend, root / "east", east_pak, 40, W, H);
    const auto turned_a = FrameAfter(backend, root / "turned", turned_pak, 0, W, H), turned_b = FrameAfter(backend, root / "turned", turned_pak, 40, W, H);
    const auto south_a = FrameAfter(backend, root / "south", south_pak, 0, W, H), south_b = FrameAfter(backend, root / "south", south_pak, 40, W, H);
    SetEnvironmentVariableForTest("AETHER_RENDER_TAA", "");
    AETHER_CHECK(background.size() == static_cast<usize>(W) * H * 4 && east_a.size() == background.size() && turned_b.size() == background.size() && south_b.size() == background.size());
    if (background.empty() || east_a.size() != background.size() || east_b.size() != background.size() || turned_a.size() != background.size() || turned_b.size() != background.size() ||
        south_a.size() != background.size() || south_b.size() != background.size())
        return;
    const Shift e = CentroidShift(east_a, east_b, background, W, H), t = CentroidShift(turned_a, turned_b, background, W, H), s = CentroidShift(south_a, south_b, background, W, H);
    std::printf("    %s: sway along the screen's x, east wind %.2f px, the tuft turned half round %.2f px, wind toward the camera %.2f px (y %.2f)\n",
                backend == rhi::Backend::Vulkan ? "vulkan" : "d3d12", e.x, t.x, s.x, s.y);
    AETHER_CHECK(e.weight > 0.0 && t.weight > 0.0 && s.weight > 0.0);
    AETHER_CHECK(std::fabs(e.x) > 1.0);                              // an east wind moves the grass sideways on the screen
    AETHER_CHECK(e.x * t.x > 0.0);                                   // and the turned tuft the same way, not the mirror image
    AETHER_CHECK(t.x > e.x * 0.5 && t.x < e.x * 2.0);                // by about as much
    AETHER_CHECK(std::fabs(s.x) < std::fabs(e.x) * 0.5);             // a wind toward the camera moves it little sideways
    std::error_code ignored;
    fs::remove_all(root, ignored);
}

AETHER_TEST(Wind_TheComponentBlowsInTheWorldWhicheverWayAnObjectFaces_D3D12) { Wind_TheComponentBlowsInTheWorldWhicheverWayAnObjectFaces(rhi::Backend::D3D12); }
AETHER_TEST(Wind_TheComponentBlowsInTheWorldWhicheverWayAnObjectFaces_Vulkan) { Wind_TheComponentBlowsInTheWorldWhicheverWayAnObjectFaces(rhi::Backend::Vulkan); }
