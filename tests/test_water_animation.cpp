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

#include <algorithm>
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
fs::path CookWaterScene(const fs::path& root, bool water, bool pillar = false) {
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

    reflect::Json entities = {
        SceneEntity({{"Transform", TransformJson({0.0f, 5.5f, 9.0f}, {-0.30f, 0.0f, 0.0f, 0.954f})},
                     {"Camera", {{"$v", 1}, {"projection", "Perspective"}, {"fov_degrees", 50.0f}, {"near_plane", 0.1f}, {"far_plane", 100.0f},
                                 {"ortho_height", 10.0f}, {"priority", 0}}}}),
        SceneEntity({{"Transform", TransformJson({0, 0, 0}, {-0.26f, 0.9f, 0.1f, 0.34f})},
                     {"DirectionalLight", {{"$v", 1}, {"color", {1.0f, 0.95f, 0.88f}}, {"intensity", 3.0f}, {"cast_shadows", false}}}}),
        SceneEntity({{"Transform", TransformJson({0, 0, 0})},
                     {"SkyLight", {{"$v", 1}, {"color", {1.0f, 1.0f, 1.0f}}, {"intensity", 1.0f}, {"environment", ""}, {"environment_rotation_degrees", 0.0f}}}}),
        SceneEntity({{"Transform", TransformJson({0, 0, 0})}, {"ModelRenderer", {{"$v", 1}, {"model", ""}, {"asset_path", "models/water.glb"}}}})};
    if (pillar) {
        // A red pillar standing at the water's far edge: where the water mirrors it, the water turns red.
        MeshBuilder box;
        box.Box(Vec3(-1.5f, 0.0f, -7.0f), Vec3(1.5f, 6.0f, -6.0f));
        ProceduralMesh pillar_mesh;
        assets::MaterialData red;
        red.base_color = Vec4(0.9f, 0.05f, 0.05f, 1.0f);
        red.roughness = 0.9f;
        red.metallic = 0.0f;
        pillar_mesh.materials.push_back(red);
        pillar_mesh.mesh.primitives.push_back(box.Finish(Shading::Flat, UvMode::BoxProjected, 0));
        const std::vector<u8> pillar_glb = EncodeGlb(pillar_mesh);
        std::ofstream(root / "Content/models/pillar.glb", std::ios::binary).write(reinterpret_cast<const char*>(pillar_glb.data()), static_cast<std::streamsize>(pillar_glb.size()));
        entities.push_back(SceneEntity({{"Transform", TransformJson({0, 0, 0})}, {"ModelRenderer", {{"$v", 1}, {"model", ""}, {"asset_path", "models/pillar.glb"}}}}));
    }
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
fs::path CookGrassScene(const fs::path& root, bool wind, f32 model_yaw_degrees = 0.0f, const reflect::Json* wind_component = nullptr, bool with_model = true,
                         const reflect::Json* zone_component = nullptr) {
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
    if (zone_component != nullptr) all.push_back(SceneEntity({{"Transform", TransformJson({0, 0, 0})}, {"WindZone", *zone_component}}));
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

// The water mirrors what stands beside it: a red pillar at the far edge of a lake turns the water in front of it red, and with the reflection off
// (AETHER_RENDER_REFLECTIONS=0) it does not. Devices without render textures with depth (D3D12 for now) draw no reflection, and the test says so.
static void Water_MirrorsThePillarBesideIt(rhi::Backend backend) {
    if (!rhi::IsBackendAvailable(backend)) return;
    SetEnvironmentVariableForTest("AETHER_RENDER_TAA", "0");
    constexpr u32 W = 640, H = 360;
    const fs::path root = fs::temp_directory_path() / "aether_water_mirror" / assets::ToString(assets::NewAssetGuid());
    const fs::path pak = CookWaterScene(root / "scene", true, true);
    if (pak.empty()) return;
    const auto mirrored = FrameAfter(backend, root / "scene", pak, 0, W, H);
    SetEnvironmentVariableForTest("AETHER_RENDER_REFLECTIONS", "0");
    const auto plain = FrameAfter(backend, root / "scene", pak, 0, W, H);
    SetEnvironmentVariableForTest("AETHER_RENDER_REFLECTIONS", "");
    SetEnvironmentVariableForTest("AETHER_RENDER_TAA", "");
    const usize bytes = static_cast<usize>(W) * H * 4;
    if (mirrored.size() != bytes || plain.size() != bytes) {
        AETHER_CHECK(false);
        return;
    }
    const auto reds = [&](const std::vector<u8>& frame) {
        usize count = 0;
        for (usize i = 0; i < static_cast<usize>(W) * H; ++i) count += frame[i * 4] > frame[i * 4 + 1] + 40 && frame[i * 4] > frame[i * 4 + 2] + 40;
        return count;
    };
    const usize on = reds(mirrored), off = reds(plain);
    std::printf("    %s: red pixels with the reflection %zu, without %zu\n", backend == rhi::Backend::Vulkan ? "vulkan" : "d3d12", on, off);
    AETHER_CHECK(off > 200);                       // the pillar itself is red either way
    if (backend == rhi::Backend::Vulkan) AETHER_CHECK(on > off + 300); // and the water in front of it, mirrored, adds more
    else AETHER_CHECK(on >= off);
    std::error_code ignored;
    fs::remove_all(root, ignored);
}
AETHER_TEST(Water_MirrorsThePillarBesideIt_D3D12) { Water_MirrorsThePillarBesideIt(rhi::Backend::D3D12); }
AETHER_TEST(Water_MirrorsThePillarBesideIt_Vulkan) { Water_MirrorsThePillarBesideIt(rhi::Backend::Vulkan); }

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

// A tuft on a grey ground with the sun low and shadows on, the wind blowing east: where the grass's shadow falls, as the centroid of the ground darkened by it.
fs::path CookShadowScene(const fs::path& root, bool wind, f32 detail = 0.0f, bool with_grass = true) {
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
    grass.sway = wind ? 0.30f : 0.0f;
    const std::vector<u8> glb = EncodeGlb(GenerateGrass(grass, 4));
    std::ofstream(root / "Content/models/grass.glb", std::ios::binary).write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
    MeshBuilder quad;
    quad.Quad(quad.Vertex(Vec3(-6, 0, 6), 0, 1), quad.Vertex(Vec3(6, 0, 6), 1, 1), quad.Vertex(Vec3(6, 0, -6), 1, 0), quad.Vertex(Vec3(-6, 0, -6), 0, 0));
    ProceduralMesh ground;
    assets::MaterialData material;
    material.base_color = Vec4(0.6f, 0.6f, 0.6f, 1.0f);
    material.roughness = 0.9f;
    material.metallic = 0.0f;
    material.detail = detail;
    ground.materials.push_back(material);
    ground.mesh.primitives.push_back(quad.Finish(Shading::Smooth, UvMode::Given, 0));
    const std::vector<u8> ground_glb = EncodeGlb(ground);
    std::ofstream(root / "Content/models/ground.glb", std::ios::binary).write(reinterpret_cast<const char*>(ground_glb.data()), static_cast<std::streamsize>(ground_glb.size()));
    const reflect::Json east = {{"$v", 1}, {"direction_degrees", 0.0f}, {"strength", 2.0f}, {"gust_speed", 1.0f}, {"gustiness", 0.4f}};
    const reflect::Json entities = {
        SceneEntity({{"Transform", TransformJson({0.0f, 6.0f, 3.0f}, {-0.5f, 0.0f, 0.0f, 0.866f})},
                     {"Camera", {{"$v", 1}, {"projection", "Perspective"}, {"fov_degrees", 50.0f}, {"near_plane", 0.1f}, {"far_plane", 60.0f},
                                 {"ortho_height", 10.0f}, {"priority", 0}}}}),
        SceneEntity({{"Transform", TransformJson({0, 0, 0}, {-0.3007f, 0.0f, 0.0f, 0.9537f})},
                     {"DirectionalLight", {{"$v", 1}, {"color", {1.0f, 0.95f, 0.88f}}, {"intensity", 3.0f}, {"cast_shadows", true}}}}),
        SceneEntity({{"Transform", TransformJson({0, 0, 0})},
                     {"SkyLight", {{"$v", 1}, {"color", {1.0f, 1.0f, 1.0f}}, {"intensity", 0.4f}, {"environment", ""}, {"environment_rotation_degrees", 0.0f}}}}),
        SceneEntity({{"Transform", TransformJson({0, 0, 0})}, {"ModelRenderer", {{"$v", 1}, {"model", ""}, {"asset_path", "models/ground.glb"}}}}),
        SceneEntity({{"Transform", {{"$v", 1}, {"position", {0.0f, 0.0f, 0.0f}}, {"rotation", {0.0f, 0.0f, 0.0f, 1.0f}}, {"scale", {6.0f, 6.0f, 6.0f}}}},
                     {"ModelRenderer", {{"$v", 1}, {"model", ""}, {"asset_path", "models/grass.glb"}}}}),
        SceneEntity({{"Transform", TransformJson({0, 0, 0})}, {"Wind", east}})};
    reflect::Json all_entities = entities;
    if (!with_grass) all_entities.erase(4); // the tuft (after the camera, the sun, the sky and the ground)
    const reflect::Json scene = {{"$type", "Scene"}, {"$version", 1}, {"entities", all_entities}};
    std::ofstream(root / "Content/Scenes/Main.ascene") << scene.dump(2);
    cook::CookOptions options;
    options.project_file = root / "WaterScene.aproject";
    options.output_dir = root / "cooked";
    const auto cooked = cook::Cook(options);
    AETHER_CHECK(cooked.ok);
    return cooked.ok ? cooked.pak_file : fs::path();
}
// The centroid of the ground's shadow: the pixels the sky tints blue (the lit ground is neutral grey, the grass is green), and how many there are.
struct ShadowCentroid { double x = 0.0, y = 0.0; usize pixels = 0; };
ShadowCentroid GroundShadow(const std::vector<u8>& frame, u32 width, u32 height) {
    ShadowCentroid out;
    for (u32 y = 0; y < height; ++y)
        for (u32 x = 0; x < width; ++x) {
            const usize i = (static_cast<usize>(y) * width + x) * 4;
            const int r = frame[i], g = frame[i + 1], b = frame[i + 2];
            if (b > r + 12 && b >= g) { out.x += x; out.y += y; ++out.pixels; }
        }
    if (out.pixels > 0) { out.x /= static_cast<double>(out.pixels); out.y /= static_cast<double>(out.pixels); }
    return out;
}

// The grass's shadow moves with the wind: the sun's shadow of a swaying tuft is where the tuft is drawn, so it shifts with the game's time, and a tuft with no
// sway casts one that stays put.
static void Wind_TheShadowSwaysWithTheGrass(rhi::Backend backend) {
    if (!rhi::IsBackendAvailable(backend)) return;
    SetEnvironmentVariableForTest("AETHER_RENDER_TAA", "0");
    constexpr u32 W = 640, H = 360;
    const fs::path root = fs::temp_directory_path() / "aether_wind_shadow" / assets::ToString(assets::NewAssetGuid());
    const fs::path windy_pak = CookShadowScene(root / "windy", true), still_pak = CookShadowScene(root / "still", false);
    if (windy_pak.empty() || still_pak.empty()) return;
    const auto windy_a = FrameAfter(backend, root / "windy", windy_pak, 0, W, H), windy_b = FrameAfter(backend, root / "windy", windy_pak, 40, W, H);
    const auto still_a = FrameAfter(backend, root / "still", still_pak, 0, W, H), still_b = FrameAfter(backend, root / "still", still_pak, 40, W, H);
    SetEnvironmentVariableForTest("AETHER_RENDER_TAA", "");
    const usize bytes = static_cast<usize>(W) * H * 4;
    if (windy_a.size() != bytes || windy_b.size() != bytes || still_a.size() != bytes || still_b.size() != bytes) {
        AETHER_CHECK(false);
        return;
    }
    const ShadowCentroid wa = GroundShadow(windy_a, W, H), wb = GroundShadow(windy_b, W, H), sa = GroundShadow(still_a, W, H), sb = GroundShadow(still_b, W, H);
    std::printf("    %s: shadow centroid x %.2f -> %.2f px swaying (%zu px), %.2f -> %.2f still (%zu px)\n", backend == rhi::Backend::Vulkan ? "vulkan" : "d3d12",
                wa.x, wb.x, wa.pixels, sa.x, sb.x, sa.pixels);
    AETHER_CHECK(wa.pixels > 200 && wb.pixels > 200 && sa.pixels > 200 && sb.pixels > 200); // there is a shadow to measure
    AETHER_CHECK(std::fabs(sb.x - sa.x) < 0.5);          // no sway, no movement
    AETHER_CHECK(std::fabs(wb.x - wa.x) > 4.0);          // the shadow moved with the wind
    std::error_code ignored;
    fs::remove_all(root, ignored);
}
AETHER_TEST(Wind_TheShadowSwaysWithTheGrass_D3D12) { Wind_TheShadowSwaysWithTheGrass(rhi::Backend::D3D12); }
AETHER_TEST(Wind_TheShadowSwaysWithTheGrass_Vulkan) { Wind_TheShadowSwaysWithTheGrass(rhi::Backend::Vulkan); }

// The detail layer: a flat grey ground lit by the sun is smooth, and with AETHER_materials_detail on its material the same ground shows a fine micro-surface, so the
// pixel-to-pixel variation of its brightness is much larger. Measured as the mean difference between neighbouring pixels over the middle of the picture (a smooth
// gradient has almost none).
static void Detail_AGroundWithTheLayerIsRoughUpClose(rhi::Backend backend) {
    if (!rhi::IsBackendAvailable(backend)) return;
    SetEnvironmentVariableForTest("AETHER_RENDER_TAA", "0");
    constexpr u32 W = 640, H = 360;
    const fs::path root = fs::temp_directory_path() / "aether_detail" / assets::ToString(assets::NewAssetGuid());
    const fs::path smooth_pak = CookShadowScene(root / "smooth", false, 0.0f, false), rough_pak = CookShadowScene(root / "rough", false, 1.0f, false);
    if (smooth_pak.empty() || rough_pak.empty()) return;
    const auto smooth = FrameAfter(backend, root / "smooth", smooth_pak, 0, W, H), rough = FrameAfter(backend, root / "rough", rough_pak, 0, W, H);
    SetEnvironmentVariableForTest("AETHER_RENDER_TAA", "");
    const usize bytes = static_cast<usize>(W) * H * 4;
    if (smooth.size() != bytes || rough.size() != bytes) {
        AETHER_CHECK(false);
        return;
    }
    const auto roughness = [&](const std::vector<u8>& frame) {
        double total = 0.0;
        usize count = 0;
        for (u32 y = 120; y < 280; ++y)
            for (u32 x = 160; x < 480; ++x) {
                const usize i = (static_cast<usize>(y) * W + x) * 4, j = i + 4;
                total += std::abs(static_cast<int>(frame[i + 1]) - static_cast<int>(frame[j + 1]));
                ++count;
            }
        return total / static_cast<double>(count);
    };
    const double flat = roughness(smooth), grainy = roughness(rough);
    std::printf("    %s: the mean step between neighbouring pixels is %.3f without the layer and %.3f with it\n", backend == rhi::Backend::Vulkan ? "vulkan" : "d3d12", flat, grainy);
    AETHER_CHECK(grainy > flat * 4.0 + 0.3);
    std::error_code ignored;
    fs::remove_all(root, ignored);
}
AETHER_TEST(Detail_AGroundWithTheLayerIsRoughUpClose_D3D12) { Detail_AGroundWithTheLayerIsRoughUpClose(rhi::Backend::D3D12); }
AETHER_TEST(Detail_AGroundWithTheLayerIsRoughUpClose_Vulkan) { Detail_AGroundWithTheLayerIsRoughUpClose(rhi::Backend::Vulkan); }

// Height fog: mist that pools low. A tall red pillar 30 m away fades into the mist at its foot and stands clear of it higher up: with the fog on, the foot has lost
// more of its colour than the top, relative to the same scene with no fog. (Fog colour is the sky's, near white here, so loss of colour is loss of redness.)
fs::path CookHeightFogScene(const fs::path& root, bool fog) {
    fs::create_directories(root / "Content/models");
    fs::create_directories(root / "Content/Scenes");
    const fs::path games = fs::path(AETHER_REPO_ASSETS_DIR).parent_path() / "games/AETHER-01";
    reflect::Json project = reflect::Json::parse(std::ifstream(games / "AETHER-01.aproject"));
    project["name"] = "WaterScene";
    project["startup_scene"] = "Scenes/Main.ascene";
    project["always_cook"] = reflect::Json::array();
    std::ofstream(root / "WaterScene.aproject") << project.dump(2);
    MeshBuilder box;
    box.Box(Vec3(-2.0f, 0.0f, -32.0f), Vec3(2.0f, 70.0f, -30.0f));
    ProceduralMesh pillar;
    assets::MaterialData red;
    red.base_color = Vec4(0.9f, 0.05f, 0.05f, 1.0f);
    red.roughness = 0.9f;
    red.metallic = 0.0f;
    pillar.materials.push_back(red);
    pillar.mesh.primitives.push_back(box.Finish(Shading::Flat, UvMode::BoxProjected, 0));
    const std::vector<u8> glb = EncodeGlb(pillar);
    std::ofstream(root / "Content/models/pillar.glb", std::ios::binary).write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
    const reflect::Json volume = {{"$v", 1}, {"global", true}, {"extents", {5.0f, 5.0f, 5.0f}}, {"blend_distance", 1.0f}, {"weight", 1.0f}, {"priority", 0},
                                  {"exposure_compensation", 0.0f}, {"bloom_intensity", 0.0f}, {"vignette", 0.0f}, {"saturation", 1.0f}, {"tonemapper", "ACES"},
                                  {"fog_density", 0.0f}, {"height_fog_density", fog ? 0.06f : 0.0f}, {"height_fog_falloff", 0.12f}, {"height_fog_base", 0.0f}};
    const reflect::Json entities = {
        SceneEntity({{"Transform", TransformJson({0.0f, 3.0f, 0.0f}, {0.0998f, 0.0f, 0.0f, 0.995f})},
                     {"Camera", {{"$v", 1}, {"projection", "Perspective"}, {"fov_degrees", 50.0f}, {"near_plane", 0.1f}, {"far_plane", 200.0f}, {"ortho_height", 10.0f}, {"priority", 0}}}}),
        SceneEntity({{"Transform", TransformJson({0, 0, 0}, {-0.3007f, 0.0f, 0.0f, 0.9537f})},
                     {"DirectionalLight", {{"$v", 1}, {"color", {1.0f, 0.95f, 0.88f}}, {"intensity", 3.0f}, {"cast_shadows", false}}}}),
        SceneEntity({{"Transform", TransformJson({0, 0, 0})}, {"SkyLight", {{"$v", 1}, {"color", {1.0f, 1.0f, 1.0f}}, {"intensity", 1.0f}, {"environment", ""}, {"environment_rotation_degrees", 0.0f}}}}),
        SceneEntity({{"Transform", TransformJson({0, 0, 0})}, {"ModelRenderer", {{"$v", 1}, {"model", ""}, {"asset_path", "models/pillar.glb"}}}}),
        SceneEntity({{"Transform", TransformJson({0, 0, 0})}, {"PostProcessVolume", volume}})};
    const reflect::Json scene = {{"$type", "Scene"}, {"$version", 1}, {"entities", entities}};
    std::ofstream(root / "Content/Scenes/Main.ascene") << scene.dump(2);
    cook::CookOptions options;
    options.project_file = root / "WaterScene.aproject";
    options.output_dir = root / "cooked";
    const auto cooked = cook::Cook(options);
    AETHER_CHECK(cooked.ok);
    return cooked.ok ? cooked.pak_file : fs::path();
}

static void Fog_HeightFogThickensLowAndThinsWithHeight(rhi::Backend backend) {
    if (!rhi::IsBackendAvailable(backend)) return;
    SetEnvironmentVariableForTest("AETHER_RENDER_TAA", "0");
    constexpr u32 W = 640, H = 360;
    const fs::path root = fs::temp_directory_path() / "aether_height_fog" / assets::ToString(assets::NewAssetGuid());
    const fs::path foggy_pak = CookHeightFogScene(root / "foggy", true), clear_pak = CookHeightFogScene(root / "clear", false);
    if (foggy_pak.empty() || clear_pak.empty()) return;
    const auto foggy = FrameAfter(backend, root / "foggy", foggy_pak, 0, W, H), clear = FrameAfter(backend, root / "clear", clear_pak, 0, W, H);
    SetEnvironmentVariableForTest("AETHER_RENDER_TAA", "");
    const usize bytes = static_cast<usize>(W) * H * 4;
    if (foggy.size() != bytes || clear.size() != bytes) {
        AETHER_CHECK(false);
        return;
    }
    // The pillar's rows in the clear frame (its red pixels), then the redness (r - g) of its lowest and highest tenth of those rows in each frame.
    u32 first = H, last = 0;
    for (u32 y = 0; y < H; ++y)
        for (u32 x = 0; x < W; ++x) {
            const usize i = (static_cast<usize>(y) * W + x) * 4;
            if (clear[i] > clear[i + 1] + 60) { first = std::min(first, y); last = std::max(last, y); }
        }
    AETHER_CHECK(last > first + 40);
    if (!(last > first + 40)) return;
    const u32 band = (last - first) / 10;
    const auto redness = [&](const std::vector<u8>& frame, u32 from, u32 to) {
        double total = 0.0;
        usize count = 0;
        for (u32 y = from; y < to; ++y)
            for (u32 x = W / 2 - 20; x < W / 2 + 20; ++x) {
                const usize i = (static_cast<usize>(y) * W + x) * 4;
                total += static_cast<int>(frame[i]) - static_cast<int>(frame[i + 1]);
                ++count;
            }
        return total / static_cast<double>(count);
    };
    const double top_kept = redness(foggy, first, first + band) / std::max(redness(clear, first, first + band), 1.0);
    const double foot_kept = redness(foggy, last - band, last) / std::max(redness(clear, last - band, last), 1.0);
    std::printf("    %s: share of the pillar's colour kept with the fog: top %.2f, foot %.2f\n", backend == rhi::Backend::Vulkan ? "vulkan" : "d3d12", top_kept, foot_kept);
    AETHER_CHECK(foot_kept < 0.85);            // the mist at its foot hides it
    AETHER_CHECK(top_kept > foot_kept + 0.12); // and it stands clearer higher up
    std::error_code ignored;
    fs::remove_all(root, ignored);
}
AETHER_TEST(Fog_HeightFogThickensLowAndThinsWithHeight_D3D12) { Fog_HeightFogThickensLowAndThinsWithHeight(rhi::Backend::D3D12); }
AETHER_TEST(Fog_HeightFogThickensLowAndThinsWithHeight_Vulkan) { Fog_HeightFogThickensLowAndThinsWithHeight(rhi::Backend::Vulkan); }

// A wind zone replaces the world's wind where an object stands in it: an east wind everywhere, a zone around the tuft that blows toward the camera, and only the
// tuft inside the zone loses its sideways sway; a zone elsewhere changes nothing.
static void Wind_AZoneChangesTheWindOnlyWhereTheObjectStands(rhi::Backend backend) {
    if (!rhi::IsBackendAvailable(backend)) return;
    SetEnvironmentVariableForTest("AETHER_RENDER_TAA", "0");
    constexpr u32 W = 640, H = 360;
    const fs::path root = fs::temp_directory_path() / "aether_wind_zone" / assets::ToString(assets::NewAssetGuid());
    const reflect::Json east = {{"$v", 1}, {"direction_degrees", 0.0f}, {"strength", 2.0f}, {"gust_speed", 1.0f}, {"gustiness", 0.4f}};
    const reflect::Json here = {{"$v", 1}, {"radius", 5.0f}, {"direction_degrees", 90.0f}, {"strength", 2.0f}, {"gust_speed", 1.0f}, {"falloff", 0.0f}};
    const fs::path plain_pak = CookGrassScene(root / "plain", true, 0.0f, &east), zoned_pak = CookGrassScene(root / "zoned", true, 0.0f, &east, true, &here);
    const fs::path empty_pak = CookGrassScene(root / "empty", true, 0.0f, &east, false);
    if (plain_pak.empty() || zoned_pak.empty() || empty_pak.empty()) return;
    const auto background = FrameAfter(backend, root / "empty", empty_pak, 0, W, H);
    const auto plain_a = FrameAfter(backend, root / "plain", plain_pak, 0, W, H), plain_b = FrameAfter(backend, root / "plain", plain_pak, 40, W, H);
    const auto zoned_a = FrameAfter(backend, root / "zoned", zoned_pak, 0, W, H), zoned_b = FrameAfter(backend, root / "zoned", zoned_pak, 40, W, H);
    SetEnvironmentVariableForTest("AETHER_RENDER_TAA", "");
    if (background.empty() || plain_a.size() != background.size() || plain_b.size() != background.size() || zoned_a.size() != background.size() || zoned_b.size() != background.size()) {
        AETHER_CHECK(false);
        return;
    }
    const Shift p = CentroidShift(plain_a, plain_b, background, W, H), z = CentroidShift(zoned_a, zoned_b, background, W, H);
    std::printf("    %s: sideways sway with the world's east wind %.2f px, with a toward-the-camera zone around the tuft %.2f px\n", backend == rhi::Backend::Vulkan ? "vulkan" : "d3d12", p.x, z.x);
    AETHER_CHECK(p.weight > 0.0 && z.weight > 0.0);
    AETHER_CHECK(std::fabs(p.x) > 1.0);
    AETHER_CHECK(std::fabs(z.x) < std::fabs(p.x) * 0.5);
    std::error_code ignored;
    fs::remove_all(root, ignored);
}
AETHER_TEST(Wind_AZoneChangesTheWindOnlyWhereTheObjectStands_D3D12) { Wind_AZoneChangesTheWindOnlyWhereTheObjectStands(rhi::Backend::D3D12); }
AETHER_TEST(Wind_AZoneChangesTheWindOnlyWhereTheObjectStands_Vulkan) { Wind_AZoneChangesTheWindOnlyWhereTheObjectStands(rhi::Backend::Vulkan); }

AETHER_TEST(Wind_TheComponentBlowsInTheWorldWhicheverWayAnObjectFaces_D3D12) { Wind_TheComponentBlowsInTheWorldWhicheverWayAnObjectFaces(rhi::Backend::D3D12); }
AETHER_TEST(Wind_TheComponentBlowsInTheWorldWhicheverWayAnObjectFaces_Vulkan) { Wind_TheComponentBlowsInTheWorldWhicheverWayAnObjectFaces(rhi::Backend::Vulkan); }
