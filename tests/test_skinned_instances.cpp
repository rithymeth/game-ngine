#include "aether/cook/cooker.h"
#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"
#include "aether/player/game.h"
#include "aether/player/scene_renderer.h"
#include "aether/player/user_paths.h"
#include "aether/reflection/serialize.h"
#include "test_framework.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace aether;
namespace fs = std::filesystem;
namespace rhi = aether::gfx::rhi;

// Entities that share a skinned model each show their own pose. The renderer used to write every entity's deformed vertices into the
// model's one per-frame buffer before the GPU drew any of them, so all of them showed the pose written last.

namespace {

reflect::Json Attribute(const char* name, int value) {
    return {{"$v", 1}, {"name", name}, {"base", value}, {"current", value}, {"min", 0}, {"max", 100}};
}

reflect::Json SceneEntity(reflect::Json components) { return {{"guid", assets::ToString(assets::NewAssetGuid())}, {"components", std::move(components)}}; }

reflect::Json Character(f32 x, bool dying) {
    reflect::Json components = {
        {"Transform", {{"$v", 1}, {"position", {x, 0.0f, 0.0f}}, {"rotation", {0.0f, 0.0f, 0.0f, 1.0f}}, {"scale", {0.08f, 0.08f, 0.08f}}}},
        {"ModelRenderer", {{"$v", 1}, {"model", ""}, {"asset_path", "models/kael.gltf"}}}};
    // The scene renderer plays Enemy_Idle on non-player entities, and Enemy_Attack at EnemyAnimationTime when EnemyAnimationState is 1.
    if (dying) {
        components["AttributeSet"] = {{"$v", 1}, {"attributes", {Attribute("EnemyAnimationState", 1), Attribute("EnemyAnimationTime", 5)}}};
    }
    return SceneEntity(std::move(components));
}

// The rows a bright pixel (the character's bone cubes on the dark background) reaches in columns [from, to), as the top and bottom.
bool BrightRows(const std::vector<u8>& rgba, u32 width, u32 height, u32 from, u32 to, u32& top, u32& bottom) {
    top = height;
    bottom = 0;
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = from; x < to; ++x) {
            const u8* p = &rgba[(static_cast<usize>(y) * width + x) * 4];
            if (static_cast<int>(p[0]) + p[1] + p[2] > 240) {
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    }
    return bottom > top;
}

} // namespace

static void Skinned_EntitiesSharingAModelShowTheirOwnPoses(rhi::Backend backend) {
    if (!rhi::IsBackendAvailable(backend)) return;
    std::unique_ptr<rhi::IDevice> device;
    try {
        device = rhi::CreateDevice(backend, false);
    } catch (const std::exception&) {
    }
    if (!device) return;

    // A project: Kael's rig (a skinned model with a Walk and a Death clip, renamed to the clips the scene renderer plays on enemies),
    // a camera, a sun, and two characters of the one model: one walking, one at the end of its Death clip.
    const fs::path root = fs::temp_directory_path() / "aether_skinned_instances" / assets::ToString(assets::NewAssetGuid());
    fs::create_directories(root / "Content/models");
    fs::create_directories(root / "Content/Scenes");
    const fs::path games = fs::path(AETHER_REPO_ASSETS_DIR).parent_path() / "games/AETHER-01";
    std::ifstream rig_file(games / "Content/Characters/Kael/Kael_Rig.gltf");
    AETHER_CHECK(rig_file.good());
    if (!rig_file.good()) return;
    reflect::Json rig = reflect::Json::parse(rig_file);
    for (reflect::Json& animation : rig["animations"]) {
        if (animation["name"] == "Walk") animation["name"] = "Enemy_Idle";
        else if (animation["name"] == "Death") animation["name"] = "Enemy_Attack";
    }
    std::ofstream(root / "Content/models/kael.gltf") << rig.dump();
    reflect::Json project = reflect::Json::parse(std::ifstream(games / "AETHER-01.aproject"));
    project["name"] = "SkinnedInstances";
    project["startup_scene"] = "Scenes/Main.ascene";
    project["always_cook"] = reflect::Json::array();
    std::ofstream(root / "SkinnedInstances.aproject") << project.dump(2);
    const reflect::Json camera = SceneEntity({{"Transform", {{"$v", 1}, {"position", {0.0f, 0.2f, 0.9f}}, {"rotation", {0.0f, 0.0f, 0.0f, 1.0f}}}},
                                         {"Camera", {{"$v", 1}, {"projection", "Perspective"}, {"fov_degrees", 40.0f}, {"near_plane", 0.02f},
                                                     {"far_plane", 50.0f}, {"ortho_height", 10.0f}, {"priority", 0}}}});
    const reflect::Json sun = SceneEntity({{"Transform", {{"$v", 1}, {"position", {0.0f, 0.0f, 0.0f}}, {"rotation", {-0.3262f, 0.2826f, 0.1028f, 0.8962f}}}},
                                      {"DirectionalLight", {{"$v", 1}, {"color", {1.0f, 0.95f, 0.88f}}, {"intensity", 3.0f}, {"cast_shadows", false}}}});
    const reflect::Json scene = {{"$type", "Scene"}, {"$version", 1}, {"entities", {camera, sun, Character(-0.18f, false), Character(0.18f, true)}}};
    std::ofstream(root / "Content/Scenes/Main.ascene") << scene.dump(2);

    cook::CookOptions options;
    options.project_file = root / "SkinnedInstances.aproject";
    options.output_dir = root / "cooked";
    const auto cooked = cook::Cook(options);
    AETHER_CHECK(cooked.ok);
    if (!cooked.ok) return;
    player::GamePackage package;
    std::string error;
    AETHER_CHECK(package.Mount(cooked.pak_file.string(), 0, &error) && package.LoadManifest(&error));
    player::Game game(package);
    game.SetUserPaths(player::ResolveUserPaths("SkinnedInstances", root / "user"));
    AETHER_CHECK(game.LoadStartupScene(&error));
    game.BeginPlay();

    constexpr u32 kWidth = 640, kHeight = 360;
    std::unique_ptr<rhi::ISwapChain> target = device->CreateSwapChain(nullptr, kWidth, kHeight, 2);
    std::vector<u8> pixels;
    {
        player::SceneRenderer renderer(*device, *target, package);
        std::unique_ptr<rhi::ICommandList> cmd = device->CreateCommandList();
        for (u32 frame = 0; frame < 6; ++frame) {
            game.Tick(1.0f / 60.0f);
            target->AcquireNextImage();
            cmd->Reset();
            renderer.Draw(game, *cmd, frame % 2);
            cmd->Close();
            const u64 fence = device->Submit(*cmd, target.get());
            device->WaitForFence(fence);
            if (frame == 5) AETHER_CHECK(target->ReadBack(pixels) && pixels.size() == kWidth * kHeight * 4);
            target->Present(false);
        }
        device->WaitForFence(device->Submit(*cmd, nullptr));
    }
    if (pixels.size() != kWidth * kHeight * 4) return;

    // The walking character stands upright; the one at the end of its Death clip has collapsed: less tall. Before the fix both showed the
    // pose written last.
    u32 walking_top = 0, walking_bottom = 0, dying_top = 0, dying_bottom = 0;
    AETHER_CHECK(BrightRows(pixels, kWidth, kHeight, 0, kWidth / 2, walking_top, walking_bottom));
    AETHER_CHECK(BrightRows(pixels, kWidth, kHeight, kWidth / 2, kWidth, dying_top, dying_bottom));
    const u32 walking_height = walking_bottom - walking_top, dying_height = dying_bottom - dying_top;
    std::printf("    walking %u px tall, dying %u px tall\n", walking_height, dying_height);
    AETHER_CHECK(walking_height > 20);
    AETHER_CHECK(dying_height + dying_height / 4 < walking_height);
    std::error_code ignored;
    fs::remove_all(root, ignored);
}

AETHER_TEST(Skinned_EntitiesSharingAModelShowTheirOwnPoses_D3D12) { Skinned_EntitiesSharingAModelShowTheirOwnPoses(rhi::Backend::D3D12); }
AETHER_TEST(Skinned_EntitiesSharingAModelShowTheirOwnPoses_Vulkan) { Skinned_EntitiesSharingAModelShowTheirOwnPoses(rhi::Backend::Vulkan); }
