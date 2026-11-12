#include "../player/src/material_overrides.h"

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
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace aether;
using namespace aether::assets::procedural;
namespace fs = std::filesystem;
namespace rhi = aether::gfx::rhi;

// A MaterialParameters component on an entity changes that entity's surface in the player (and so in the editor's Live Viewport): player/src/material_overrides.h.

AETHER_TEST(MaterialOverrides_TheNamedParametersReplaceTheModelsSurface) {
    MaterialParameters overrides;
    player::SurfaceValues surface;
    surface.color[0] = 0.8f; surface.color[1] = 0.7f; surface.color[2] = 0.6f; surface.color[3] = 1.0f;
    surface.metallic = 0.1f;
    surface.roughness = 0.9f;
    surface.emissive[0] = 0.0f; surface.emissive[1] = 0.0f; surface.emissive[2] = 0.0f;

    // Nothing set: the model's own surface, and no texture.
    AETHER_CHECK(player::ApplyMaterialOverrides(overrides, surface) == nullptr);
    AETHER_CHECK(surface.color[0] == 0.8f && surface.metallic == 0.1f && surface.roughness == 0.9f);

    overrides.SetVectorParameter(player::kOverrideBaseColor, Vec3(1.0f, 0.0f, 0.5f), 0.25f);
    overrides.SetScalarParameter(player::kOverrideMetallic, 1.0f);
    overrides.SetScalarParameter(player::kOverrideRoughness, 0.2f);
    overrides.SetVectorParameter(player::kOverrideEmissive, Vec3(0.0f, 2.0f, 0.0f), 1.0f);
    overrides.SetScalarParameter("SomethingElse", 7.0f); // a name the player does not know is left alone
    AETHER_CHECK(player::ApplyMaterialOverrides(overrides, surface) == nullptr);
    AETHER_CHECK(surface.color[0] == 1.0f && surface.color[1] == 0.0f && surface.color[2] == 0.5f && surface.color[3] == 0.25f);
    AETHER_CHECK(surface.metallic == 1.0f && surface.roughness == 0.2f);
    AETHER_CHECK(surface.emissive[1] == 2.0f); // emission can go above 1: it is HDR

    // A texture parameter is returned as the path to draw, and a texture under another name is not.
    overrides.SetTextureParameter(player::kOverrideBaseColorTexture, "textures/brick.png");
    overrides.SetTextureParameter("NormalTexture", "textures/other.png");
    const std::string* path = player::ApplyMaterialOverrides(overrides, surface);
    AETHER_CHECK(path != nullptr && *path == "textures/brick.png");
}

AETHER_TEST(MaterialOverrides_OutOfRangeIsClampedAndNotANumberIsIgnored) {
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 inf = std::numeric_limits<f32>::infinity();
    player::SurfaceValues baseline;
    baseline.metallic = 0.3f;
    baseline.roughness = 0.6f;
    player::SurfaceValues surface = baseline;
    MaterialParameters overrides;
    overrides.SetScalarParameter(player::kOverrideMetallic, 5.0f);   // above 1: 1
    overrides.SetScalarParameter(player::kOverrideRoughness, -2.0f); // below 0: 0
    overrides.SetVectorParameter(player::kOverrideBaseColor, Vec3(-1.0f, 3.0f, 0.5f), 9.0f);
    player::ApplyMaterialOverrides(overrides, surface);
    AETHER_CHECK(surface.metallic == 1.0f && surface.roughness == 0.0f);
    AETHER_CHECK(surface.color[0] == 0.0f && surface.color[1] == 3.0f && surface.color[3] == 1.0f); // a colour may be above 1, never below 0; alpha is 0 to 1

    // A value that is not a number leaves the model's, whole: not zero, not clamped.
    surface = baseline;
    MaterialParameters broken;
    broken.SetScalarParameter(player::kOverrideMetallic, nan);
    broken.SetScalarParameter(player::kOverrideRoughness, inf);
    broken.SetVectorParameter(player::kOverrideBaseColor, Vec3(1.0f, nan, 1.0f), 1.0f);
    broken.SetVectorParameter(player::kOverrideEmissive, Vec3(inf, 0.0f, 0.0f), 1.0f);
    player::ApplyMaterialOverrides(broken, surface);
    AETHER_CHECK(surface.metallic == 0.3f && surface.roughness == 0.6f);
    AETHER_CHECK(surface.color[0] == 1.0f && surface.color[1] == 1.0f && surface.emissive[0] == 0.0f);
}

namespace {

reflect::Json SceneEntity(reflect::Json components) { return {{"guid", assets::ToString(assets::NewAssetGuid())}, {"components", std::move(components)}}; }

reflect::Json TransformJson(std::initializer_list<f32> position, std::initializer_list<f32> rotation = {0, 0, 0, 1}) {
    return {{"$v", 1}, {"position", position}, {"rotation", rotation}, {"scale", {1.0f, 1.0f, 1.0f}}};
}

// A 2 m box in front of the camera on a plain grey material, with `parameters` (a MaterialParameters component, or null for none) on it, and a plain
// blue 4 x 4 texture in the content folder for the texture case.
fs::path CookBoxScene(const fs::path& root, const reflect::Json& parameters) {
    fs::create_directories(root / "Content/models");
    fs::create_directories(root / "Content/textures");
    fs::create_directories(root / "Content/Scenes");
    const fs::path games = fs::path(AETHER_REPO_ASSETS_DIR).parent_path() / "games/AETHER-01";
    reflect::Json project = reflect::Json::parse(std::ifstream(games / "AETHER-01.aproject"));
    project["name"] = "OverrideScene";
    project["startup_scene"] = "Scenes/Main.ascene";
    project["always_cook"] = reflect::Json::array({"textures/blue.png"});
    std::ofstream(root / "OverrideScene.aproject") << project.dump(2);

    std::vector<u8> rgb(4 * 4 * 3);
    for (usize i = 0; i < rgb.size(); i += 3) {
        rgb[i] = 10;
        rgb[i + 1] = 10;
        rgb[i + 2] = 240;
    }
    const std::vector<u8> png = assets::EncodePngRgb8(4, 4, rgb);
    std::ofstream(root / "Content/textures/blue.png", std::ios::binary).write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));

    MeshBuilder box;
    box.Box(Vec3(-1.0f, -1.0f, -1.0f), Vec3(1.0f, 1.0f, 1.0f));
    ProceduralMesh mesh;
    assets::MaterialData grey;
    grey.base_color = Vec4(0.6f, 0.6f, 0.6f, 1.0f);
    grey.roughness = 0.9f;
    grey.metallic = 0.0f;
    mesh.materials.push_back(grey);
    mesh.mesh.primitives.push_back(box.Finish(Shading::Flat, UvMode::BoxProjected, 0));
    const std::vector<u8> glb = EncodeGlb(mesh);
    std::ofstream(root / "Content/models/box.glb", std::ios::binary).write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));

    reflect::Json model = {{"Transform", TransformJson({0, 0, 0})}, {"ModelRenderer", {{"$v", 1}, {"model", ""}, {"asset_path", "models/box.glb"}}}};
    if (!parameters.is_null()) model["MaterialParameters"] = parameters;
    const reflect::Json entities = {
        SceneEntity({{"Transform", TransformJson({0.0f, 0.0f, 5.0f})},
                     {"Camera", {{"$v", 1}, {"projection", "Perspective"}, {"fov_degrees", 50.0f}, {"near_plane", 0.1f}, {"far_plane", 100.0f},
                                 {"ortho_height", 10.0f}, {"priority", 0}}}}),
        SceneEntity({{"Transform", TransformJson({0, 0, 0}, {-0.26f, 0.9f, 0.1f, 0.34f})},
                     {"DirectionalLight", {{"$v", 1}, {"color", {1.0f, 0.95f, 0.88f}}, {"intensity", 3.0f}, {"cast_shadows", false}}}}),
        SceneEntity({{"Transform", TransformJson({0, 0, 0})},
                     {"SkyLight", {{"$v", 1}, {"color", {1.0f, 1.0f, 1.0f}}, {"intensity", 1.0f}, {"environment", ""}, {"environment_rotation_degrees", 0.0f}}}}),
        SceneEntity(std::move(model))};
    const reflect::Json scene = {{"$type", "Scene"}, {"$version", 1}, {"entities", entities}};
    std::ofstream(root / "Content/Scenes/Main.ascene") << scene.dump(2);
    cook::CookOptions options;
    options.project_file = root / "OverrideScene.aproject";
    options.output_dir = root / "cooked";
    const auto cooked = cook::Cook(options);
    AETHER_CHECK(cooked.ok);
    return cooked.ok ? cooked.pak_file : fs::path();
}

reflect::Json Parameters(std::initializer_list<reflect::Json> values) {
    return {{"$v", 1}, {"material", ""}, {"values", reflect::Json(values)}, {"revision", 0}};
}
reflect::Json Vector(const char* name, f32 x, f32 y, f32 z, f32 w) {
    return {{"$v", 1}, {"name", name}, {"value", {x, y, z, w}}, {"texture", ""}, {"is_texture", false}};
}
reflect::Json Scalar(const char* name, f32 value) { return Vector(name, value, 0, 0, 0); }
reflect::Json Texture(const char* name, const char* path) {
    return {{"$v", 1}, {"name", name}, {"value", {0.0f, 0.0f, 0.0f, 0.0f}}, {"texture", path}, {"is_texture", true}};
}

struct Colour { double r = 0, g = 0, b = 0; };

// The scene's frame, and the mean colour of the middle of it (the box's front face fills it).
Colour CentreOf(rhi::Backend backend, const fs::path& root, const fs::path& pak_file, bool* ran) {
    constexpr u32 width = 160, height = 90;
    std::vector<u8> pixels;
    std::unique_ptr<rhi::IDevice> device;
    try {
        device = rhi::CreateDevice(backend, false);
    } catch (const std::exception&) {
    }
    *ran = device != nullptr;
    if (!device) return {};
    player::GamePackage package;
    std::string error;
    AETHER_CHECK(package.Mount(pak_file.string(), 0, &error) && package.LoadManifest(&error));
    player::Game game(package);
    game.SetUserPaths(player::ResolveUserPaths("OverrideScene", root / "user"));
    AETHER_CHECK(game.LoadStartupScene(&error));
    game.BeginPlay();
    game.Tick(1.0f / 60.0f);
    std::unique_ptr<rhi::ISwapChain> target = device->CreateSwapChain(nullptr, width, height, 2);
    {
        player::SceneRenderer renderer(*device, *target, package);
        std::unique_ptr<rhi::ICommandList> cmd = device->CreateCommandList();
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
    Colour total;
    usize count = 0;
    for (u32 y = height / 2 - 6; y < height / 2 + 6; ++y) {
        for (u32 x = width / 2 - 6; x < width / 2 + 6; ++x) {
            const usize i = (static_cast<usize>(y) * width + x) * 4;
            if (i + 2 >= pixels.size()) continue;
            total.r += pixels[i];
            total.g += pixels[i + 1];
            total.b += pixels[i + 2];
            ++count;
        }
    }
    if (count > 0) {
        total.r /= static_cast<double>(count);
        total.g /= static_cast<double>(count);
        total.b /= static_cast<double>(count);
    }
    return total;
}

} // namespace

static void MaterialOverrides_ShowOnTheModelInTheRenderedPicture(rhi::Backend backend) {
    const fs::path root = fs::temp_directory_path() / "aether_material_overrides";
    fs::remove_all(root);
    bool ran = false;
    const Colour plain = CentreOf(backend, root / "plain", CookBoxScene(root / "plain", nullptr), &ran);
    if (!ran) {
        std::printf("    (no %s device on this machine)\n", backend == rhi::Backend::Vulkan ? "Vulkan" : "D3D12");
        return;
    }
    AETHER_CHECK(std::fabs(plain.r - plain.b) < 40.0 && plain.r > 30.0); // the model's own grey, lit: no colour cast

    // A base colour: red, with the same lighting, makes the face red.
    const Colour red = CentreOf(backend, root / "red", CookBoxScene(root / "red", Parameters({Vector("BaseColor", 1.0f, 0.0f, 0.0f, 1.0f)})), &ran);
    AETHER_CHECK(red.r > red.g + 40.0 && red.r > red.b + 40.0);

    // Emission adds light of its own: a green glow lifts green well above the plain face and keeps it the strongest channel (the tone curve pulls a bright
    // glow toward white, so red and blue rise too).
    const Colour glow = CentreOf(backend, root / "glow", CookBoxScene(root / "glow", Parameters({Vector("Emissive", 0.0f, 3.0f, 0.0f, 1.0f)})), &ran);
    AETHER_CHECK(glow.g > plain.g + 60.0 && glow.g > glow.r + 15.0 && glow.g > glow.b + 15.0);

    // Metallic with low roughness drops the diffuse grey for a reflection of the lit sky: the face is brighter than the diffuse one, and no longer bluish.
    const Colour metal = CentreOf(backend, root / "metal", CookBoxScene(root / "metal", Parameters({Scalar("Metallic", 1.0f), Scalar("Roughness", 0.15f)})), &ran);
    AETHER_CHECK(metal.r + metal.g + metal.b > (plain.r + plain.g + plain.b) * 1.1);
    AETHER_CHECK(metal.b - metal.r < plain.b - plain.r - 8.0);

    // A texture, by the content path of an image in the package, is drawn in place of the model's own: a blue image makes the face blue.
    const Colour blue = CentreOf(backend, root / "blue", CookBoxScene(root / "blue", Parameters({Texture("BaseColorTexture", "textures/blue.png")})), &ran);
    AETHER_CHECK(blue.b > blue.r + 40.0 && blue.b > blue.g + 40.0);

    // A parameter under a name the player does not know changes nothing.
    const Colour other = CentreOf(backend, root / "other", CookBoxScene(root / "other", Parameters({Scalar("Wobble", 4.0f)})), &ran);
    AETHER_CHECK(std::fabs(other.r - plain.r) < 6.0 && std::fabs(other.g - plain.g) < 6.0 && std::fabs(other.b - plain.b) < 6.0);
    fs::remove_all(root);
}
AETHER_TEST(MaterialOverrides_ShowOnTheModelInTheRenderedPicture_D3D12) { MaterialOverrides_ShowOnTheModelInTheRenderedPicture(rhi::Backend::D3D12); }
AETHER_TEST(MaterialOverrides_ShowOnTheModelInTheRenderedPicture_Vulkan) { MaterialOverrides_ShowOnTheModelInTheRenderedPicture(rhi::Backend::Vulkan); }
