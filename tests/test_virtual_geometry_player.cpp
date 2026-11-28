#include "aether/assets/procedural_mesh.h"
#include "aether/cook/cooker.h"
#include "aether/gfx/rhi/command_list.h"
#include "aether/gfx/rhi/device.h"
#include "aether/player/game.h"
#include "aether/player/scene_renderer.h"
#include "aether/player/user_paths.h"
#include "../player/src/cluster_model.h"
#include "aether/cook/mesh_cook.h"
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

// Virtual geometry, stage 4, through the whole renderer (docs/design/VIRTUAL_GEOMETRY.md): a lit sphere of 82,000 triangles, drawn the plain way and through the cluster
// path (AETHER_VIRTUAL_GEOMETRY=1) with the GPU choosing the clusters. At a cut of 0 pixels the clusters are the mesh and the picture must be the same; at 1 pixel the
// draw sends far fewer triangles and the picture is nearly the same; off, nothing changes.

namespace {

reflect::Json SceneEntity(reflect::Json components) { return {{"guid", assets::ToString(assets::NewAssetGuid())}, {"components", std::move(components)}}; }

reflect::Json TransformJson(std::initializer_list<f32> position, std::initializer_list<f32> rotation = {0, 0, 0, 1}, std::initializer_list<f32> scale = {1.0f, 1.0f, 1.0f}) {
    return {{"$v", 1}, {"position", position}, {"rotation", rotation}, {"scale", scale}};
}

void SetEnvironmentVariableForTest(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    if (*value != '\0') setenv(name, value, 1);
    else unsetenv(name);
#endif
}

// A sphere of 20 * 4^6 = 81,920 triangles and a radius of 1 m (times the scale asked), 9 m from the camera (which looks straight at it, down -Z) unless `z` moves it, lit
// by the sun and the sky.
fs::path CookSphereScene(const fs::path& root, std::initializer_list<f32> scale = {1.0f, 1.0f, 1.0f}, f32 z = 0.0f, f32 fov_degrees = 50.0f, bool clusters = false) {
    fs::create_directories(root / "Content/models");
    fs::create_directories(root / "Content/Scenes");
    const fs::path games = fs::path(AETHER_REPO_ASSETS_DIR).parent_path() / "games/AETHER-01";
    reflect::Json project = reflect::Json::parse(std::ifstream(games / "AETHER-01.aproject"));
    project["name"] = "ClusterScene";
    project["startup_scene"] = "Scenes/Main.ascene";
    project["always_cook"] = reflect::Json::array();
    std::ofstream(root / "ClusterScene.aproject") << project.dump(2);
    MeshBuilder sphere;
    sphere.IcoSphere(6);
    ProceduralMesh mesh;
    assets::MaterialData material;
    material.base_color = Vec4(0.7f, 0.45f, 0.3f, 1.0f);
    material.roughness = 0.5f;
    material.metallic = 0.0f;
    mesh.materials.push_back(material);
    mesh.mesh.primitives.push_back(sphere.Finish(Shading::Smooth, UvMode::Given, 0));
    const std::vector<u8> glb = EncodeGlb(mesh);
    std::ofstream(root / "Content/models/ball.glb", std::ios::binary).write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
    const reflect::Json entities = {
        SceneEntity({{"Transform", TransformJson({0.0f, 0.0f, 9.0f})},
                     {"Camera", {{"$v", 1}, {"projection", "Perspective"}, {"fov_degrees", fov_degrees}, {"near_plane", 0.1f}, {"far_plane", 100.0f}, {"ortho_height", 10.0f}, {"priority", 0}}}}),
        SceneEntity({{"Transform", TransformJson({0, 0, 0}, {-0.26f, 0.9f, 0.1f, 0.34f})},
                     {"DirectionalLight", {{"$v", 1}, {"color", {1.0f, 0.95f, 0.88f}}, {"intensity", 3.0f}, {"cast_shadows", false}}}}),
        SceneEntity({{"Transform", TransformJson({0, 0, 0})},
                     {"SkyLight", {{"$v", 1}, {"color", {1.0f, 1.0f, 1.0f}}, {"intensity", 1.0f}, {"environment", ""}, {"environment_rotation_degrees", 0.0f}}}}),
        SceneEntity({{"Transform", TransformJson({0, 0, z}, {0, 0, 0, 1}, scale)}, {"ModelRenderer", {{"$v", 1}, {"model", ""}, {"asset_path", "models/ball.glb"}}}})};
    const reflect::Json scene = {{"$type", "Scene"}, {"$version", 1}, {"entities", entities}};
    std::ofstream(root / "Content/Scenes/Main.ascene") << scene.dump(2);
    cook::CookOptions options;
    options.project_file = root / "ClusterScene.aproject";
    options.output_dir = root / "cooked";
    options.cook_clusters = clusters;   // aether_cook --clusters: each mesh's cluster hierarchy goes into the pak
    const auto cooked = cook::Cook(options);
    AETHER_CHECK(cooked.ok);
    return cooked.ok ? cooked.pak_file : fs::path();
}

struct Frame {
    std::vector<u8> pixels;
    player::SceneRenderer::VirtualGeometryStats stats;
    bool supported = false; // the device could draw through clusters
};

// The scene drawn a few frames (the renderer's own resources settle in the first), the last one read, with the environment as the caller set it.
Frame Render(const fs::path& root, const fs::path& pak_file, u32 width, u32 height) {
    Frame frame;
    std::unique_ptr<rhi::IDevice> device;
    try {
        device = rhi::CreateDevice(rhi::Backend::Vulkan, false);
    } catch (const std::exception&) {
    }
    if (!device) return frame;
    frame.supported = device->SupportsIndirectDraw() && device->SupportsIndirectCount() && device->SupportsStorageBufferRead();
    player::GamePackage package;
    std::string error;
    AETHER_CHECK(package.Mount(pak_file.string(), 0, &error) && package.LoadManifest(&error));
    player::Game game(package);
    game.SetUserPaths(player::ResolveUserPaths("ClusterScene", root / "user"));
    AETHER_CHECK(game.LoadStartupScene(&error));
    game.BeginPlay();
    game.Tick(1.0f / 60.0f);
    std::unique_ptr<rhi::ISwapChain> target = device->CreateSwapChain(nullptr, width, height, 2);
    {
        player::SceneRenderer renderer(*device, *target, package);
        std::unique_ptr<rhi::ICommandList> cmd = device->CreateCommandList();
        for (u32 index = 0; index < 5; ++index) {
            target->AcquireNextImage();
            cmd->Reset();
            renderer.Draw(game, *cmd, index % 2);
            cmd->Close();
            device->WaitForFence(device->Submit(*cmd, target.get()));
            if (index == 4) {
                AETHER_CHECK(target->ReadBack(frame.pixels) && frame.pixels.size() == static_cast<usize>(width) * height * 4);
                frame.stats = renderer.VirtualGeometry();
            }
            target->Present(false);
        }
    }
    return frame;
}

// The mean over the pixels of the largest channel difference, and how many pixels differ at all.
struct Difference {
    double mean = 0.0;
    usize differing = 0;
};
Difference Compare(const std::vector<u8>& a, const std::vector<u8>& b) {
    Difference d;
    if (a.size() != b.size() || a.empty()) {
        d.mean = 1e9;
        return d;
    }
    double total = 0.0;
    for (usize i = 0; i < a.size(); i += 4) {
        int worst = 0;
        for (usize c = 0; c < 3; ++c) worst = std::max(worst, std::abs(static_cast<int>(a[i + c]) - static_cast<int>(b[i + c])));
        total += worst;
        d.differing += worst != 0 ? 1 : 0;
    }
    d.mean = total / static_cast<double>(a.size() / 4);
    return d;
}

constexpr u32 kW = 640, kH = 360;
constexpr u64 kSphereTriangles = 81920;

} // namespace

AETHER_TEST(VirtualGeometryPlayer_OffByDefaultAndTheDrawIsTheMeshAtZeroPixels) {
    if (!rhi::IsBackendAvailable(rhi::Backend::Vulkan)) return;
    SetEnvironmentVariableForTest("AETHER_RENDER_TAA", "0"); // no sub-pixel jitter between frames: what differs is the clusters
    const fs::path root = fs::temp_directory_path() / "aether_vg_scene" / assets::ToString(assets::NewAssetGuid());
    const fs::path pak = CookSphereScene(root);
    if (pak.empty()) return;

    SetEnvironmentVariableForTest("AETHER_VIRTUAL_GEOMETRY", "");
    const Frame plain = Render(root, pak, kW, kH);
    if (plain.pixels.empty()) return;
    AETHER_CHECK(!plain.stats.enabled && plain.stats.primitives == 0 && plain.stats.draws == 0);

    SetEnvironmentVariableForTest("AETHER_VIRTUAL_GEOMETRY", "1");
    SetEnvironmentVariableForTest("AETHER_VIRTUAL_GEOMETRY_PIXELS", "0");
    const Frame exact = Render(root, pak, kW, kH);
    SetEnvironmentVariableForTest("AETHER_VIRTUAL_GEOMETRY", "");
    SetEnvironmentVariableForTest("AETHER_VIRTUAL_GEOMETRY_PIXELS", "");
    if (!exact.supported) return; // this device cannot draw through clusters: the plain draw is what it did
    AETHER_CHECK(exact.stats.enabled && exact.stats.primitives == 1 && exact.stats.draws == 1);
    // Every triangle the camera can see is drawn once (the back half is culled by the cull on the GPU, the rest is the mesh): the picture is the plain one.
    AETHER_CHECK(exact.stats.triangles_full == kSphereTriangles && exact.stats.triangles_drawn > 0 && exact.stats.triangles_drawn <= kSphereTriangles);
    const Difference same = Compare(plain.pixels, exact.pixels);
    AETHER_CHECK(same.differing == 0);
}

AETHER_TEST(VirtualGeometryPlayer_OnePixelDrawsFarFewerTrianglesAndTheSamePicture) {
    if (!rhi::IsBackendAvailable(rhi::Backend::Vulkan)) return;
    SetEnvironmentVariableForTest("AETHER_RENDER_TAA", "0");
    const fs::path root = fs::temp_directory_path() / "aether_vg_scene" / assets::ToString(assets::NewAssetGuid());
    const fs::path pak = CookSphereScene(root);
    if (pak.empty()) return;
    SetEnvironmentVariableForTest("AETHER_VIRTUAL_GEOMETRY", "");
    const Frame plain = Render(root, pak, kW, kH);
    if (plain.pixels.empty()) return;
    SetEnvironmentVariableForTest("AETHER_VIRTUAL_GEOMETRY", "1");
    SetEnvironmentVariableForTest("AETHER_VIRTUAL_GEOMETRY_PIXELS", "1");
    const Frame cut = Render(root, pak, kW, kH);
    SetEnvironmentVariableForTest("AETHER_VIRTUAL_GEOMETRY", "");
    SetEnvironmentVariableForTest("AETHER_VIRTUAL_GEOMETRY_PIXELS", "");
    if (!cut.supported) return;
    AETHER_CHECK(cut.stats.draws == 1 && cut.stats.triangles_full == kSphereTriangles);
    // The sphere is about 190 pixels across: at a pixel of error most of its 82,000 triangles are smaller than the picture can tell apart.
    AETHER_CHECK(cut.stats.triangles_drawn > 0 && cut.stats.triangles_drawn < kSphereTriangles / 4);
    const Difference near = Compare(plain.pixels, cut.pixels);
    AETHER_CHECK(near.mean < 0.5);
}

AETHER_TEST(VirtualGeometryPlayer_ASmallerModelIsCutHarderBecauseItCoversFewerPixels) {
    if (!rhi::IsBackendAvailable(rhi::Backend::Vulkan)) return;
    SetEnvironmentVariableForTest("AETHER_RENDER_TAA", "0");
    const fs::path root = fs::temp_directory_path() / "aether_vg_scene" / assets::ToString(assets::NewAssetGuid());
    const fs::path big_pak = CookSphereScene(root / "big", {2.0f, 2.0f, 2.0f});
    const fs::path small_pak = CookSphereScene(root / "small", {0.5f, 0.5f, 0.5f});
    if (big_pak.empty() || small_pak.empty()) return;
    SetEnvironmentVariableForTest("AETHER_VIRTUAL_GEOMETRY", "1");
    SetEnvironmentVariableForTest("AETHER_VIRTUAL_GEOMETRY_PIXELS", "1");
    const Frame big = Render(root / "big", big_pak, kW, kH);
    const Frame small = Render(root / "small", small_pak, kW, kH);
    SetEnvironmentVariableForTest("AETHER_VIRTUAL_GEOMETRY", "");
    SetEnvironmentVariableForTest("AETHER_VIRTUAL_GEOMETRY_PIXELS", "");
    if (!big.supported || big.pixels.empty()) return;
    AETHER_CHECK(big.stats.draws == 1 && small.stats.draws == 1);
    // Four times the size is sixteen times the pixels: the one is cut where the other is not, whatever the scale did to the mesh's own sizes.
    AETHER_CHECK(small.stats.triangles_drawn > 0 && small.stats.triangles_drawn < big.stats.triangles_drawn);
}

AETHER_TEST(VirtualGeometryPlayer_WhatItCannotCutRightIsDrawnWhole) {
    if (!rhi::IsBackendAvailable(rhi::Backend::Vulkan)) return;
    SetEnvironmentVariableForTest("AETHER_RENDER_TAA", "0");
    const fs::path root = fs::temp_directory_path() / "aether_vg_scene" / assets::ToString(assets::NewAssetGuid());
    const fs::path stretched_pak = CookSphereScene(root / "stretched", {1.0f, 2.0f, 1.0f}); // an unequal scale: its pixel error is not one number
    const fs::path behind_pak = CookSphereScene(root / "behind", {1.0f, 1.0f, 1.0f}, 30.0f); // behind the camera
    if (stretched_pak.empty() || behind_pak.empty()) return;
    SetEnvironmentVariableForTest("AETHER_VIRTUAL_GEOMETRY", "");
    const Frame plain = Render(root / "stretched", stretched_pak, kW, kH);
    SetEnvironmentVariableForTest("AETHER_VIRTUAL_GEOMETRY", "1");
    SetEnvironmentVariableForTest("AETHER_VIRTUAL_GEOMETRY_PIXELS", "1");
    const Frame stretched = Render(root / "stretched", stretched_pak, kW, kH);
    const Frame behind = Render(root / "behind", behind_pak, kW, kH);
    SetEnvironmentVariableForTest("AETHER_VIRTUAL_GEOMETRY", "");
    SetEnvironmentVariableForTest("AETHER_VIRTUAL_GEOMETRY_PIXELS", "");
    if (!stretched.supported || plain.pixels.empty()) return;
    AETHER_CHECK(stretched.stats.primitives == 1 && stretched.stats.draws == 0); // it has clusters, and this draw does not use them
    AETHER_CHECK(Compare(plain.pixels, stretched.pixels).differing == 0);        // so it is drawn as before
    AETHER_CHECK(behind.stats.draws == 0 && behind.stats.frames == 0);           // nothing the camera sees: no cull is made at all
}

AETHER_TEST(VirtualGeometryPlayer_ClustersOutsideTheViewAreLeftOut) {
    if (!rhi::IsBackendAvailable(rhi::Backend::Vulkan)) return;
    SetEnvironmentVariableForTest("AETHER_RENDER_TAA", "0");
    const fs::path root = fs::temp_directory_path() / "aether_vg_scene" / assets::ToString(assets::NewAssetGuid());
    // The same sphere (4 m in radius, 7 m from the lens) seen through a lens that crops it top and bottom, and through one wide enough to hold all of it. The back-facing
    // clusters go the same way in both (the camera is where it is); what is left out of the first is what is outside its picture.
    const fs::path cropped_pak = CookSphereScene(root / "cropped", {4.0f, 4.0f, 4.0f}, 2.0f, 50.0f);
    const fs::path whole_pak = CookSphereScene(root / "whole", {4.0f, 4.0f, 4.0f}, 2.0f, 110.0f);
    if (cropped_pak.empty() || whole_pak.empty()) return;
    SetEnvironmentVariableForTest("AETHER_VIRTUAL_GEOMETRY", "1");
    SetEnvironmentVariableForTest("AETHER_VIRTUAL_GEOMETRY_PIXELS", "0"); // the finest cut: every cluster that can be seen, and only those
    const Frame cropped = Render(root / "cropped", cropped_pak, kW, kH);
    const Frame whole = Render(root / "whole", whole_pak, kW, kH);
    SetEnvironmentVariableForTest("AETHER_VIRTUAL_GEOMETRY", "");
    SetEnvironmentVariableForTest("AETHER_VIRTUAL_GEOMETRY_PIXELS", "");
    if (!whole.supported || whole.pixels.empty()) return;
    AETHER_CHECK(whole.stats.draws == 1 && cropped.stats.draws == 1);
    AETHER_CHECK(cropped.stats.triangles_drawn > 0 && cropped.stats.triangles_drawn < whole.stats.triangles_drawn * 9 / 10);
}

AETHER_TEST(VirtualGeometryPlayer_ClustersCookedIntoThePakAreReadAndDrawTheSamePicture) {
    if (!rhi::IsBackendAvailable(rhi::Backend::Vulkan)) return;
    SetEnvironmentVariableForTest("AETHER_RENDER_TAA", "0");
    const fs::path root = fs::temp_directory_path() / "aether_vg_scene" / assets::ToString(assets::NewAssetGuid());
    const fs::path plain_pak = CookSphereScene(root / "plain");                                       // no clusters in the pak
    const fs::path cooked_pak = CookSphereScene(root / "cooked", {1.0f, 1.0f, 1.0f}, 0.0f, 50.0f, true); // the same scene cooked with them
    if (plain_pak.empty() || cooked_pak.empty()) return;
    SetEnvironmentVariableForTest("AETHER_VIRTUAL_GEOMETRY", "1");
    SetEnvironmentVariableForTest("AETHER_VIRTUAL_GEOMETRY_PIXELS", "1");
    const Frame built = Render(root / "plain", plain_pak, kW, kH);   // built when the model loads
    const Frame read = Render(root / "cooked", cooked_pak, kW, kH);  // read from the pak
    SetEnvironmentVariableForTest("AETHER_VIRTUAL_GEOMETRY", "");
    const Frame off = Render(root / "cooked", cooked_pak, kW, kH);   // the pak has clusters, the switch is off: they are not used
    SetEnvironmentVariableForTest("AETHER_VIRTUAL_GEOMETRY_PIXELS", "");
    if (!built.supported || built.pixels.empty()) return;
    AETHER_CHECK(built.stats.primitives == 1 && built.stats.primitives_cooked == 0);
    AETHER_CHECK(read.stats.primitives == 1 && read.stats.primitives_cooked == 1);
    // The same algorithm on the same mesh: the same clusters, the same picture.
    AETHER_CHECK(read.stats.triangles_drawn == built.stats.triangles_drawn && read.stats.triangles_drawn > 0);
    AETHER_CHECK(Compare(built.pixels, read.pixels).differing == 0);
    AETHER_CHECK(!off.stats.enabled && off.stats.primitives == 0 && off.stats.draws == 0);
}

AETHER_TEST(VirtualGeometryPlayer_AHierarchyThatDoesNotFitItsMeshIsRefused) {
    if (!rhi::IsBackendAvailable(rhi::Backend::Vulkan)) return;
    std::unique_ptr<rhi::IDevice> device;
    try {
        device = rhi::CreateDevice(rhi::Backend::Vulkan, false);
    } catch (const std::exception&) {
    }
    if (!device) return;
    // A little mesh (a 12 x 12 grid of quads) and its hierarchy.
    std::vector<assets::MeshVertex> vertices;
    std::vector<u32> indices;
    for (u32 z = 0; z <= 12; ++z)
        for (u32 x = 0; x <= 12; ++x) {
            assets::MeshVertex v{};
            v.position[0] = static_cast<f32>(x);
            v.position[2] = static_cast<f32>(z);
            v.normal[1] = 1.0f;
            vertices.push_back(v);
        }
    for (u32 z = 0; z < 12; ++z)
        for (u32 x = 0; x < 12; ++x) {
            const u32 a = z * 13 + x, b = a + 1, c = a + 13, d = c + 1;
            for (const u32 i : {a, c, b, b, c, d}) indices.push_back(i);
        }
    cook::ClusterHierarchy hierarchy;
    AETHER_CHECK(cook::BuildPrimitiveClusters(vertices, indices, cook::ClusterParams{}, hierarchy));
    std::string why;
    {
        player::ClusterModel model;
        AETHER_CHECK(model.Adopt(*device, hierarchy, vertices.size(), indices.size(), &why));   // it fits
        AETHER_CHECK(model.FullTriangles() == indices.size() / 3);
    }
    {
        player::ClusterModel model;
        AETHER_CHECK(!model.Adopt(*device, hierarchy, vertices.size() - 1, indices.size(), &why) && !why.empty()); // the last vertex is gone: an index is past them
    }
    {
        player::ClusterModel model;
        AETHER_CHECK(!model.Adopt(*device, hierarchy, vertices.size(), indices.size() + 3, &why) && !why.empty()); // a mesh with another triangle
    }
    {
        player::ClusterModel model;
        AETHER_CHECK(!model.Adopt(*device, cook::ClusterHierarchy{}, vertices.size(), indices.size(), &why) && !why.empty()); // no clusters
    }
}
