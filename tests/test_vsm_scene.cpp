#include "aether/cook/cooker.h"
#include "aether/core/log.h"
#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"
#include "aether/player/game.h"
#include "aether/player/scene_renderer.h"
#include "aether/player/user_paths.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"
#include "test_framework.h"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace aether;
namespace fs = std::filesystem;
namespace rhi = aether::gfx::rhi;

// The sun's virtual shadow maps through the whole renderer: pages are asked for by the feedback pass, drawn, and read by the scene shader in
// place of the cascades. Set AETHER_PARITY_OUT to a directory to keep the frames (BMP).

namespace {

reflect::Json SceneEntity(reflect::Json components) { return {{"guid", assets::ToString(assets::NewAssetGuid())}, {"components", std::move(components)}}; }

reflect::Json TransformJson(std::initializer_list<f32> position, std::initializer_list<f32> rotation = {0, 0, 0, 1}, f32 scale = 1.0f) {
    return {{"$v", 1}, {"position", position}, {"rotation", rotation}, {"scale", {scale, scale, scale}}};
}

reflect::Json Model(const char* path, std::initializer_list<f32> position, std::initializer_list<f32> rotation, f32 scale) {
    return SceneEntity({{"Transform", TransformJson(position, rotation, scale)},
                        {"ModelRenderer", {{"$v", 1}, {"model", ""}, {"asset_path", path}}}});
}

reflect::Json Mover(reflect::Json entity) {
    entity["components"]["Tags"] = {{"$v", 1}, {"names", {"Mover"}}};
    return entity;
}

void SetEnvironmentVariableForTest(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value); // an empty value removes it
#else
    if (*value != '\0') setenv(name, value, 1);
    else unsetenv(name);
#endif
}

struct SceneRun {
    std::vector<u8> pixels;
    player::SceneRenderer::VirtualShadowStats last;
    player::SceneRenderer::RayTracedShadowStats rt;
    player::SceneRenderer::GpuTimings gpu;
    u32 max_resident = 0, drawn_pages = 0, max_requests = 0, suppressed = 0;
    std::vector<u32> drawn_by_frame; // pages drawn in each frame
};

// A sunlit floor with two boxes and a small cube (tagged Mover) on it, seen from `camera_distance` metres away; with `character`, also a
// skinned figure (Kael's rig, its walk clip as the one the player plays on non-player entities) that animates every frame.
fs::path CookScene(const fs::path& root, f32 camera_distance, bool character = false) {
    fs::create_directories(root / "Content/models");
    fs::create_directories(root / "Content/Scenes");
    const fs::path games = fs::path(AETHER_REPO_ASSETS_DIR).parent_path() / "games/AETHER-01";
    for (const char* name : {"aether_greybox_core.gltf", "aether_greybox_armor.gltf", "aether_greybox_rifle.gltf", "test_cube.bin"}) {
        fs::copy_file(games / "Content/models" / name, root / "Content/models" / name, fs::copy_options::overwrite_existing);
    }
    reflect::Json project = reflect::Json::parse(std::ifstream(games / "AETHER-01.aproject"));
    project["name"] = "VsmScene";
    project["startup_scene"] = "Scenes/Main.ascene";
    project["always_cook"] = reflect::Json::array();
    std::ofstream(root / "VsmScene.aproject") << project.dump(2);
    if (character) {
        std::ifstream rig_file(games / "Content/Characters/Kael/Kael_Rig.gltf");
        AETHER_CHECK(rig_file.good());
        if (!rig_file.good()) return {};
        reflect::Json rig = reflect::Json::parse(rig_file);
        for (reflect::Json& animation : rig["animations"]) {
            if (animation["name"] == "Walk") animation["name"] = "Enemy_Idle";
        }
        std::ofstream(root / "Content/models/kael.gltf") << rig.dump();
    }
    reflect::Json entities = {
        SceneEntity({{"Transform", TransformJson({0.0f, 1.0f, camera_distance}, {-0.18f, 0.0f, 0.0f, 0.984f})},
                     {"Camera", {{"$v", 1}, {"projection", "Perspective"}, {"fov_degrees", 50.0f}, {"near_plane", 0.1f}, {"far_plane", 100.0f},
                                 {"ortho_height", 10.0f}, {"priority", 0}}}}),
        SceneEntity({{"Transform", TransformJson({0, 0, 0}, {-0.4f, 0.35f, 0.15f, 0.83f})},
                     {"DirectionalLight", {{"$v", 1}, {"color", {1.0f, 0.95f, 0.88f}}, {"intensity", 3.0f}, {"cast_shadows", true}}}}),
        SceneEntity({{"Transform", TransformJson({0, 0, 0})},
                     {"SkyLight", {{"$v", 1}, {"color", {1.0f, 1.0f, 1.0f}}, {"intensity", 1.0f}, {"environment", ""},
                                   {"environment_rotation_degrees", 0.0f}}}}),
        Model("models/aether_greybox_rifle.gltf", {0.0f, -0.3f, 0.0f}, {0, 0, 0, 1}, 12.0f), // the floor
        Model("models/aether_greybox_core.gltf", {-0.9f, 0.4f, 0.0f}, {0.0f, 0.38f, 0.0f, 0.92f}, 2.0f),
        Mover(Model("models/aether_greybox_armor.gltf", {0.0f, 0.3f, 0.2f}, {0.0f, -0.2f, 0.0f, 0.98f}, 0.2f)),
        Model("models/aether_greybox_core.gltf", {0.9f, 0.4f, -0.3f}, {0.0f, 0.1f, 0.0f, 0.99f}, 2.0f)};
    if (character) entities.push_back(Model("models/kael.gltf", {0.2f, 0.05f, 0.5f}, {0, 0, 0, 1}, 0.4f));
    const reflect::Json scene = {{"$type", "Scene"}, {"$version", 1}, {"entities", entities}};
    std::ofstream(root / "Content/Scenes/Main.ascene") << scene.dump(2);
    cook::CookOptions options;
    options.project_file = root / "VsmScene.aproject";
    options.output_dir = root / "cooked";
    const auto cooked = cook::Cook(options);
    AETHER_CHECK(cooked.ok);
    return cooked.ok ? cooked.pak_file : fs::path();
}

// `before_frame` runs before each frame is drawn (to move things).
SceneRun RunScene(rhi::Backend backend, const fs::path& root, const fs::path& pak_file, u32 width, u32 height, u32 frames,
                  const std::function<void(player::Game&, u32)>& before_frame = {}) {
    SceneRun run;
    if (!rhi::IsBackendAvailable(backend)) return run;
    std::unique_ptr<rhi::IDevice> device;
    try {
        device = rhi::CreateDevice(backend, false);
    } catch (const std::exception&) {
    }
    if (!device) return run;
    player::GamePackage package;
    std::string error;
    AETHER_CHECK(package.Mount(pak_file.string(), 0, &error) && package.LoadManifest(&error));
    player::Game game(package);
    game.SetUserPaths(player::ResolveUserPaths("VsmScene", root / (backend == rhi::Backend::Vulkan ? "user_vk" : "user_dx")));
    AETHER_CHECK(game.LoadStartupScene(&error));
    game.BeginPlay();
    std::unique_ptr<rhi::ISwapChain> target = device->CreateSwapChain(nullptr, width, height, 2);
    {
        player::SceneRenderer renderer(*device, *target, package);
        std::unique_ptr<rhi::ICommandList> cmd = device->CreateCommandList();
        for (u32 frame = 0; frame < frames; ++frame) {
            game.Tick(1.0f / 60.0f);
            if (before_frame) before_frame(game, frame);
            target->AcquireNextImage();
            cmd->Reset();
            renderer.Draw(game, *cmd, frame % 2);
            cmd->Close();
            device->WaitForFence(device->Submit(*cmd, target.get()));
            run.last = renderer.VirtualShadows();
            run.rt = renderer.RayTracedShadows();
            if (const auto timings = renderer.GpuFrameTimings(); timings.valid) run.gpu = timings;
            run.max_resident = std::max(run.max_resident, run.last.resident);
            run.max_requests = std::max(run.max_requests, run.last.requests);
            run.suppressed = std::max(run.suppressed, run.last.suppressed);
            run.drawn_pages += run.last.drawn_pages;
            run.drawn_by_frame.push_back(run.last.drawn_pages);
            if (frame + 1 == frames) AETHER_CHECK(target->ReadBack(run.pixels) && run.pixels.size() == static_cast<usize>(width) * height * 4);
            target->Present(false);
        }
    }
    return run;
}

// The sum of squared luminance steps between neighbouring pixels: a shadow edge blurred over more pixels has less of it.
double EdgeEnergy(const std::vector<u8>& rgba, u32 width, u32 height) {
    const auto luma = [&](u32 x, u32 y) {
        const u8* p = &rgba[(static_cast<usize>(y) * width + x) * 4];
        return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
    };
    double energy = 0.0;
    for (u32 y = 0; y + 1 < height; ++y) {
        for (u32 x = 0; x + 1 < width; ++x) {
            const double dx = luma(x + 1, y) - luma(x, y), dy = luma(x, y + 1) - luma(x, y);
            energy += dx * dx + dy * dy;
        }
    }
    return energy;
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

constexpr u32 kWidth = 1280, kHeight = 720;

const char* Name(rhi::Backend backend) { return backend == rhi::Backend::Vulkan ? "vulkan" : "d3d12"; }

} // namespace

// The sun's shadow from pages: asked for by the feedback, drawn once, kept while nothing moves, and the same shadow as the cascades' but crisper
// close up, where the cascades' texel is coarser than a pixel.
static void Vsm_PagesShadowTheSunAndAreKeptWhileNothingMoves(rhi::Backend backend) {
    if (!rhi::IsBackendAvailable(backend)) return;
    const fs::path root = fs::temp_directory_path() / "aether_vsm_scene" / assets::ToString(assets::NewAssetGuid());
    const fs::path pak = CookScene(root, 1.6f);
    if (pak.empty()) return;
    constexpr u32 kFrames = 20;
    SetEnvironmentVariableForTest("AETHER_RENDER_VSM", "");
    const SceneRun with = RunScene(backend, root, pak, kWidth, kHeight, kFrames);
    SetEnvironmentVariableForTest("AETHER_RENDER_VSM", "0");
    const SceneRun without = RunScene(backend, root, pak, kWidth, kHeight, kFrames);
    SetEnvironmentVariableForTest("AETHER_RENDER_VSM", "");
    AETHER_CHECK(with.last.enabled && !without.last.enabled);
    AETHER_CHECK(with.max_requests > 0 && with.max_resident > 0 && with.drawn_pages >= with.max_resident);
    AETHER_CHECK(with.last.drawn_pages == 0 && with.last.pending == 0); // a still scene redraws nothing once its pages are there
    AETHER_CHECK(with.pixels.size() == static_cast<usize>(kWidth) * kHeight * 4 && without.pixels.size() == with.pixels.size());
    if (with.pixels.size() != without.pixels.size() || with.pixels.empty()) return;
    const double difference = MeanDifference(with.pixels, without.pixels);
    const double sharp = EdgeEnergy(with.pixels, kWidth, kHeight), blurred = EdgeEnergy(without.pixels, kWidth, kHeight);
    std::printf("    %s: %u pages resident (%u asked for), %u drawn in all; against the cascades: mean difference %.3f, edge energy %.4f x\n",
                Name(backend), with.max_resident, with.max_requests, with.drawn_pages, difference, sharp / blurred);
    AETHER_CHECK(difference < 1.0);        // the same shadows: only their edges differ
    AETHER_CHECK(sharp > blurred * 1.005); // and those edges are crisper
    std::error_code ignored;
    fs::remove_all(root, ignored);
}

AETHER_TEST(Vsm_PagesShadowTheSunAndAreKeptWhileNothingMoves_D3D12) { Vsm_PagesShadowTheSunAndAreKeptWhileNothingMoves(rhi::Backend::D3D12); }
AETHER_TEST(Vsm_PagesShadowTheSunAndAreKeptWhileNothingMoves_Vulkan) { Vsm_PagesShadowTheSunAndAreKeptWhileNothingMoves(rhi::Backend::Vulkan); }

// A caster that moves has its pages redrawn, and the shadow follows it: the frame matches the cascades', which have no pages to keep right.
static void Vsm_AMovingCasterRedrawsThePagesItReaches(rhi::Backend backend) {
    if (!rhi::IsBackendAvailable(backend)) return;
    const fs::path root = fs::temp_directory_path() / "aether_vsm_scene" / assets::ToString(assets::NewAssetGuid());
    const fs::path pak = CookScene(root, 1.6f);
    if (pak.empty()) return;
    constexpr u32 kFrames = 28, kMoveAt = 14;
    const auto move = [](player::Game& game, u32 frame) {
        if (frame < kMoveAt) return;
        const std::vector<Entity> movers = FindEntitiesWithTag(game.GetWorld(), "Mover");
        if (movers.size() != 1) return;
        if (Transform* transform = game.GetWorld().GetComponent<Transform>(movers.front())) {
            transform->position.x = 0.3f + 0.05f * static_cast<f32>(frame - kMoveAt); // a slow walk to the right
        }
    };
    SetEnvironmentVariableForTest("AETHER_RENDER_VSM", "");
    const SceneRun with = RunScene(backend, root, pak, kWidth, kHeight, kFrames, move);
    SetEnvironmentVariableForTest("AETHER_RENDER_VSM", "0");
    const SceneRun without = RunScene(backend, root, pak, kWidth, kHeight, kFrames, move);
    SetEnvironmentVariableForTest("AETHER_RENDER_VSM", "");
    AETHER_CHECK(with.drawn_by_frame.size() == kFrames);
    if (with.drawn_by_frame.size() != kFrames || with.pixels.size() != without.pixels.size() || with.pixels.empty()) return;
    u32 before = 0, after = 0;
    for (u32 frame = 0; frame < kFrames; ++frame) (frame < kMoveAt - 2 ? before : after) += with.drawn_by_frame[frame];
    for (u32 frame = kMoveAt - 3; frame < kMoveAt; ++frame) AETHER_CHECK(with.drawn_by_frame[frame] == 0); // quiet before it moves
    AETHER_CHECK(before > 0 && after > 0);
    u32 busy_frames = 0;
    for (u32 frame = kMoveAt; frame < kFrames; ++frame) busy_frames += with.drawn_by_frame[frame] > 0 ? 1u : 0u;
    AETHER_CHECK(busy_frames + 2 >= kFrames - kMoveAt); // a page or more redrawn nearly every frame it moves
    const double difference = MeanDifference(with.pixels, without.pixels);
    std::printf("    %s: %u pages drawn before it moved, %u after (%u frames of %u); mean difference from the cascades %.3f\n", Name(backend),
                before, after, busy_frames, kFrames - kMoveAt, difference);
    AETHER_CHECK(difference < 1.0); // the shadow is where the caster is, not where it was
    std::error_code ignored;
    fs::remove_all(root, ignored);
}

AETHER_TEST(Vsm_AMovingCasterRedrawsThePagesItReaches_D3D12) { Vsm_AMovingCasterRedrawsThePagesItReaches(rhi::Backend::D3D12); }
AETHER_TEST(Vsm_AMovingCasterRedrawsThePagesItReaches_Vulkan) { Vsm_AMovingCasterRedrawsThePagesItReaches(rhi::Backend::Vulkan); }

// The two backends build and read the same pages.
AETHER_TEST(Vsm_BothBackendsDrawTheSamePages) {
    if (!rhi::IsBackendAvailable(rhi::Backend::D3D12) || !rhi::IsBackendAvailable(rhi::Backend::Vulkan)) return;
    const fs::path root = fs::temp_directory_path() / "aether_vsm_scene" / assets::ToString(assets::NewAssetGuid());
    const fs::path pak = CookScene(root, 1.6f);
    if (pak.empty()) return;
    const SceneRun dx = RunScene(rhi::Backend::D3D12, root, pak, kWidth, kHeight, 20);
    const SceneRun vk = RunScene(rhi::Backend::Vulkan, root, pak, kWidth, kHeight, 20);
    AETHER_CHECK(dx.last.enabled && vk.last.enabled);
    AETHER_CHECK(dx.max_resident == vk.max_resident && dx.drawn_pages == vk.drawn_pages && dx.max_requests == vk.max_requests);
    if (dx.pixels.size() == vk.pixels.size() && !dx.pixels.empty()) AETHER_CHECK(MeanDifference(dx.pixels, vk.pixels) < 1.0);
    std::error_code ignored;
    fs::remove_all(root, ignored);
}

// A skinned figure that animates every frame is in the cascades and in no page: the pages under it are left to the cascades, nothing is redrawn
// for it, and the frame is still the cascades' (its shadow comes from them). Drawing it into pages instead cost 8 to 12 ms a frame for a
// 192k-vertex character, every frame, so this is the rule that keeps skinned casters affordable.
static void Vsm_ADeformingCasterIsLeftToTheCascades(rhi::Backend backend) {
    if (!rhi::IsBackendAvailable(backend)) return;
    const fs::path root = fs::temp_directory_path() / "aether_vsm_scene" / assets::ToString(assets::NewAssetGuid());
    const fs::path pak = CookScene(root, 1.6f, true);
    if (pak.empty()) return;
    constexpr u32 kFrames = 24;
    SetEnvironmentVariableForTest("AETHER_RENDER_VSM", "");
    const SceneRun with = RunScene(backend, root, pak, kWidth, kHeight, kFrames);
    SetEnvironmentVariableForTest("AETHER_RENDER_VSM", "0");
    const SceneRun without = RunScene(backend, root, pak, kWidth, kHeight, kFrames);
    SetEnvironmentVariableForTest("AETHER_RENDER_VSM", "");
    AETHER_CHECK(with.last.enabled && with.max_resident > 0);
    AETHER_CHECK(with.suppressed > 0); // pages under the figure are left to the cascades
    AETHER_CHECK(with.drawn_by_frame.size() == kFrames);
    if (with.drawn_by_frame.size() != kFrames || with.pixels.size() != without.pixels.size() || with.pixels.empty()) return;
    u32 late_pages = 0;
    for (u32 frame = kFrames - 8; frame < kFrames; ++frame) late_pages += with.drawn_by_frame[frame];
    AETHER_CHECK(late_pages == 0); // the figure animates all the while, and no page is redrawn for it
    const double difference = MeanDifference(with.pixels, without.pixels);
    std::printf("    %s: %u pages left to the cascades under the figure, %u drawn in the last 8 frames; mean difference from the cascades %.3f\n", Name(backend), with.suppressed, late_pages, difference);
    AETHER_CHECK(difference < 1.0);
    std::error_code ignored;
    fs::remove_all(root, ignored);
}

AETHER_TEST(Vsm_ADeformingCasterIsLeftToTheCascades_D3D12) { Vsm_ADeformingCasterIsLeftToTheCascades(rhi::Backend::D3D12); }
AETHER_TEST(Vsm_ADeformingCasterIsLeftToTheCascades_Vulkan) { Vsm_ADeformingCasterIsLeftToTheCascades(rhi::Backend::Vulkan); }

// Ray-traced sun shadows (AETHER_RENDER_RT_SHADOWS=1) through the whole renderer: the scene shader traces a ray per pixel against the rigid
// casters' acceleration structure instead of reading the cascades. The picture matches the cascades' (same shadows, hard edges), the structure
// is built once for a still scene, and rebuilt as a caster moves. Vulkan needs its ray tracing extensions asked for as the device is made.
static SceneRun RunRayTraced(rhi::Backend backend, const fs::path& root, const fs::path& pak, u32 frames,
                             const std::function<void(player::Game&, u32)>& before_frame = {}) {
    SetEnvironmentVariableForTest("AETHER_RENDER_RT_SHADOWS", "1");
    SetEnvironmentVariableForTest("AETHER_VK_RAY_TRACING", "1");
    const SceneRun run = RunScene(backend, root, pak, kWidth, kHeight, frames, before_frame);
    SetEnvironmentVariableForTest("AETHER_RENDER_RT_SHADOWS", "");
    SetEnvironmentVariableForTest("AETHER_VK_RAY_TRACING", "");
    return run;
}

static void RtShadows_MatchTheCascadesAndRebuildOnlyWhenACasterMoves(rhi::Backend backend) {
    if (!rhi::IsBackendAvailable(backend)) return;
    const fs::path root = fs::temp_directory_path() / "aether_rt_scene" / assets::ToString(assets::NewAssetGuid());
    const fs::path pak = CookScene(root, 1.6f);
    if (pak.empty()) return;
    constexpr u32 kFrames = 28, kMoveAt = 14;
    SetEnvironmentVariableForTest("AETHER_RENDER_VSM", "0");
    SetEnvironmentVariableForTest("AETHER_RENDER_RT_SHADOWS", "");
    const SceneRun cascades = RunScene(backend, root, pak, kWidth, kHeight, kFrames);
    SetEnvironmentVariableForTest("AETHER_RENDER_VSM", "");
    const SceneRun still = RunRayTraced(backend, root, pak, kFrames);
    if (!still.rt.enabled) {
        std::printf("    %s: no ray tracing here, skipped\n", Name(backend));
        std::error_code ignored;
        fs::remove_all(root, ignored);
        return;
    }
    AETHER_CHECK(still.rt.instances >= 3); // the floor, the two boxes and the cube
    AETHER_CHECK(still.rt.rebuilds >= 1 && still.rt.rebuilds <= 2); // built when the casters appeared, not every frame
    AETHER_CHECK(still.pixels.size() == cascades.pixels.size() && !still.pixels.empty());
    if (still.pixels.size() != cascades.pixels.size() || still.pixels.empty()) return;
    const double difference = MeanDifference(still.pixels, cascades.pixels);
    std::printf("    %s: %u casters in the structure, %u builds; mean difference from the cascades %.3f\n", Name(backend), still.rt.instances,
                still.rt.rebuilds, difference);
    AETHER_CHECK(difference < 0.2); // the same shadows (without them the frame is 0.48 away)
    AETHER_CHECK(difference > 0.0); // and it is not the cascades under another name

    const auto move = [](player::Game& game, u32 frame) {
        if (frame < kMoveAt) return;
        const std::vector<Entity> movers = FindEntitiesWithTag(game.GetWorld(), "Mover");
        if (movers.size() != 1) return;
        if (Transform* transform = game.GetWorld().GetComponent<Transform>(movers.front())) {
            transform->position.x = 0.3f + 0.05f * static_cast<f32>(frame - kMoveAt);
        }
    };
    SetEnvironmentVariableForTest("AETHER_RENDER_VSM", "0");
    const SceneRun cascades_moved = RunScene(backend, root, pak, kWidth, kHeight, kFrames, move);
    SetEnvironmentVariableForTest("AETHER_RENDER_VSM", "");
    const SceneRun moved = RunRayTraced(backend, root, pak, kFrames, move);
    AETHER_CHECK(moved.rt.rebuilds >= still.rt.rebuilds + (kFrames - kMoveAt) - 2); // rebuilt about every frame the cube moves
    if (moved.pixels.size() != cascades_moved.pixels.size() || moved.pixels.empty()) return;
    const double moved_difference = MeanDifference(moved.pixels, cascades_moved.pixels);
    std::printf("    %s: the cube moving: %u builds; mean difference from the cascades %.3f\n", Name(backend), moved.rt.rebuilds, moved_difference);
    AETHER_CHECK(moved_difference < 0.25); // the shadow is where the cube is, not where it was
    std::error_code ignored;
    fs::remove_all(root, ignored);
}

AETHER_TEST(RtShadows_MatchTheCascadesAndRebuildOnlyWhenACasterMoves_D3D12) {
    RtShadows_MatchTheCascadesAndRebuildOnlyWhenACasterMoves(rhi::Backend::D3D12);
}
AETHER_TEST(RtShadows_MatchTheCascadesAndRebuildOnlyWhenACasterMoves_Vulkan) {
    RtShadows_MatchTheCascadesAndRebuildOnlyWhenACasterMoves(rhi::Backend::Vulkan);
}

// The renderer's own GPU timing: each frame is timed by stage on the GPU and read back when its submission has finished.
static void GpuTiming_TheRendererReportsGpuTimePerStage(rhi::Backend backend) {
    if (!rhi::IsBackendAvailable(backend)) return;
    const fs::path root = fs::temp_directory_path() / "aether_gpu_timing" / assets::ToString(assets::NewAssetGuid());
    const fs::path pak = CookScene(root, 1.6f);
    if (pak.empty()) return;
    const auto started = std::chrono::steady_clock::now();
    const SceneRun run = RunScene(backend, root, pak, kWidth, kHeight, 30);
    const f32 run_ms = std::chrono::duration<f32, std::milli>(std::chrono::steady_clock::now() - started).count();
    if (!run.gpu.valid) {
        std::printf("    %s: no GPU timing here, skipped\n", Name(backend));
        return;
    }
    std::printf("    %s: the GPU took %.3f ms a frame: shadows %.3f, the scene %.3f, post and the rest %.3f\n", Name(backend), run.gpu.frame_ms,
                run.gpu.shadows_ms, run.gpu.scene_ms, run.gpu.post_ms);
    // One frame's GPU time cannot be longer than the whole 30-frame run took on the clock. The bound used to be a fixed 100 ms,
    // which a software rasterizer on a shared CI runner does not meet (137 ms there) although its timings are sound.
    AETHER_CHECK(run.gpu.frame_ms > 0.0f && run.gpu.frame_ms < run_ms);
    AETHER_CHECK(run.gpu.shadows_ms >= 0.0f && run.gpu.scene_ms > 0.0f && run.gpu.post_ms >= 0.0f);
    AETHER_CHECK(std::fabs(run.gpu.frame_ms - (run.gpu.shadows_ms + run.gpu.scene_ms + run.gpu.post_ms)) < 0.01f);
    std::error_code ignored;
    fs::remove_all(root, ignored);
}

AETHER_TEST(GpuTiming_TheRendererReportsGpuTimePerStage_D3D12) { GpuTiming_TheRendererReportsGpuTimePerStage(rhi::Backend::D3D12); }
AETHER_TEST(GpuTiming_TheRendererReportsGpuTimePerStage_Vulkan) { GpuTiming_TheRendererReportsGpuTimePerStage(rhi::Backend::Vulkan); }
