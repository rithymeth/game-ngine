#include "core/scene_preview.h"
#include "ui/image_texture.h"
#include "workspace/editor_workspace.h"
#if defined(AETHER_HAS_EDITOR_VULKAN)
#include "host/live_viewport_panel.h"
#endif
#include "world/scene_preview_panel.h"

#include "aether/core/log.h"
#include "aether/scene/components.h"
#include "aether/scene/hierarchy.h"
#include "aether/scene/script_component.h"
#include "aether/platform/filesystem.h"
#include "aether/player/game.h"
#include "aether/templates/templates.h"
#include "ui/builtin_components.h"
#include "test_framework.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <imgui.h>
#include <memory>
#include <set>

// The rendered preview: the real player renders a project's scene and the picture comes back as pixels. Like the other player tests
// it needs a Vulkan device (a display, or a software driver).

using namespace aether;
using namespace aether::editor;
namespace stdfs = std::filesystem;

namespace {

// A headless ImGui context (no renderer backend, so a texture is registered but never uploaded).
class Context {
public:
    Context() {
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1400, 900);
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        ImGui::NewFrame();
    }
    ~Context() {
        ImGui::EndFrame();
        ImGui::DestroyContext(context_);
    }
    void Frame() { ImGui::EndFrame(); ImGui::NewFrame(); }
private:
    ImGuiContext* context_ = nullptr;
};

stdfs::path MakeProject(const std::string& name, const std::string& template_id = "third_person") {
    RegisterEditorComponents(); // as the editor does at startup: a template scene names components of several modules
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_scene_preview_tests" / name;
    stdfs::remove_all(dir);
    ProjectPaths paths;
    std::string error;
    AETHER_CHECK(templates::CreateProjectFromTemplate(dir, "Demo", template_id, &paths, &error));
    return paths.file;
}

} // namespace

AETHER_TEST(ScenePreview_ARealRenderComesBackAsAPicture) {
    ScenePreview preview(MakeProject("Render"));
    AETHER_CHECK(preview.GetState() == ScenePreview::State::Idle);
    PreviewImage image;
    AETHER_CHECK(!preview.TakeImage(image));                                   // nothing yet
    AETHER_CHECK(preview.Start("Scenes/Main.ascene", 320, 180));
    AETHER_CHECK(!preview.Start("Scenes/Main.ascene", 320, 180));              // one at a time
    preview.Wait();
    AETHER_CHECK(preview.GetState() == ScenePreview::State::Done);
    if (preview.GetState() != ScenePreview::State::Done) { AETHER_LOG_ERROR("Test", "%s", preview.Message().c_str()); return; }
    AETHER_CHECK(preview.TakeImage(image));
    AETHER_CHECK(image.width == 320 && image.height == 180 && image.rgba.size() == 320u * 180u * 4u);
    std::set<u32> colours;                                                     // a lit scene has many colours; an empty or failed frame has one or two
    for (usize i = 0; i + 3 < image.rgba.size(); i += 4 * 97) colours.insert(u32(image.rgba[i]) << 16 | u32(image.rgba[i + 1]) << 8 | image.rgba[i + 2]);
    AETHER_CHECK(colours.size() > 20);
    AETHER_CHECK(!preview.TakeImage(image));                                   // taken once
    AETHER_CHECK(preview.Message() == "Rendered Scenes/Main.ascene");
    AETHER_CHECK(preview.Start("Scenes/Main.ascene", 64, 64));                 // and it can be run again
    preview.Wait();
    AETHER_CHECK(preview.GetState() == ScenePreview::State::Done && preview.TakeImage(image) && image.width == 64 && image.height == 64);
}

AETHER_TEST(ScenePreview_FailuresSayWhy) {
    {   // No player to render with.
        ScenePreview preview(MakeProject("NoPlayer", "blank"));
        preview.player_path = stdfs::temp_directory_path() / "aether_scene_preview_tests" / "no_such_player";
        AETHER_CHECK(preview.Start("Scenes/Main.ascene", 320, 180));
        preview.Wait();
        AETHER_CHECK(preview.GetState() == ScenePreview::State::Failed);
        AETHER_CHECK(preview.Message().find("player") != std::string::npos && preview.Message().find("not found") != std::string::npos);
        PreviewImage image;
        AETHER_CHECK(!preview.TakeImage(image));
    }
    {   // A scene that is not in the project.
        ScenePreview preview(MakeProject("NoScene", "blank"));
        AETHER_CHECK(preview.Start("Scenes/DoesNotExist.ascene", 320, 180));
        preview.Wait();
        AETHER_CHECK(preview.GetState() == ScenePreview::State::Failed);
        AETHER_CHECK(!preview.Message().empty() && preview.Message() != "Rendering");
    }
    {   // A project file that is not there.
        ScenePreview preview(stdfs::temp_directory_path() / "aether_scene_preview_tests" / "nothing" / "Nope.aproject");
        AETHER_CHECK(preview.Start("Scenes/Main.ascene", 320, 180));
        preview.Wait();
        AETHER_CHECK(preview.GetState() == ScenePreview::State::Failed && !preview.Message().empty());
    }
}

AETHER_TEST(ImageTexture_ShowsAPictureAndReplacesIt) {
    Context imgui;
    ImageTexture texture;
    AETHER_CHECK(!texture.Valid());
    AETHER_CHECK(!texture.Set(0, 4, {}));                                    // empty
    AETHER_CHECK(!texture.Set(4, 4, std::vector<u8>(10)));                   // the wrong number of bytes
    AETHER_CHECK(!texture.Set(100000, 1, std::vector<u8>(4)));               // absurdly wide
    AETHER_CHECK(!texture.Valid());
    const std::vector<u8> a(8 * 4 * 4, 200);
    AETHER_CHECK(texture.Set(8, 4, a) && texture.Valid() && texture.Width() == 8 && texture.Height() == 4);
    imgui.Frame();
    AETHER_CHECK(texture.Set(8, 4, std::vector<u8>(8 * 4 * 4, 50)));          // the same size: updated in place
    imgui.Frame();
    AETHER_CHECK(texture.Set(16, 9, std::vector<u8>(16 * 9 * 4, 90)) && texture.Width() == 16 && texture.Height() == 9); // another size: a new texture
    imgui.Frame();
    AETHER_CHECK(!texture.Set(16, 9, std::vector<u8>(3)) && texture.Width() == 16);                                     // a bad picture leaves the old one
    ImGui::Begin("t");
    ImGui::Image(texture.Ref(), ImVec2(32, 18));                              // drawing it does not trip anything
    ImGui::End();
}

AETHER_TEST(ScenePreviewPanel_RendersTheSavedSceneIntoATexture) {
    Context imgui;
    const stdfs::path project = MakeProject("PanelRender", "blank");
    const stdfs::path content = project.parent_path() / "Content";
    ScenePanel scene(content);
    ScenePreviewPanel panel(scene);
    AETHER_CHECK(!panel.Render() && panel.Note() == "No project is open.");   // nothing to render yet
    panel.SetProject(project);
    panel.width = 160;
    panel.height = 90;
    AETHER_CHECK(!panel.Render());                                             // an untitled scene would replace the project's Main: refused,
    AETHER_CHECK(panel.Note().find("already exists") != std::string::npos);    // and it says why
    AETHER_CHECK(scene.OpenScene("Scenes/Main.ascene", true));                  // so the project's own scene is opened first
    scene.Document().CreateEntity("Marker");
    AETHER_CHECK(panel.Render());                                              // saved first, then started
    {   // the player renders what is on disk, so the scene was saved first: the new entity is in the file
        std::string text;
        AETHER_CHECK(fs::ReadFileText((content / scene.Path()).string(), text) && text.find("Marker") != std::string::npos);
    }
    AETHER_CHECK(!panel.Render());                                             // one at a time
    panel.Preview()->Wait();
    AETHER_CHECK(panel.Preview()->GetState() == ScenePreview::State::Done);
    AETHER_CHECK(!panel.Texture().Valid());                                    // not shown until it is taken
    panel.Poll();
    AETHER_CHECK(panel.Texture().Valid() && panel.Texture().Width() == 160 && panel.Texture().Height() == 90);
    ImGui::Begin("p");
    panel.Draw();                                                              // draws the button, the message and the picture
    ImGui::End();
}

AETHER_TEST(EditorWorkspace_TheSceneToolWorksInTheOpenProject) {
    Context imgui;
    EditorWorkspace workspace;
    const stdfs::path project = MakeProject("WorkspaceProject", "blank");
    AETHER_CHECK(workspace.OpenProject(project));
    AETHER_CHECK(workspace.Scene().ContentRoot() == project.parent_path() / "Content");   // so Save and the scene list are the project's
    const usize before = workspace.Scene().Document().Entities().size();
    workspace.Scene().Document().CreateEntity("Unsaved");
    const stdfs::path other = MakeProject("OtherProject", "blank");
    AETHER_CHECK(workspace.OpenProject(other));
    AETHER_CHECK(workspace.Scene().ContentRoot() == other.parent_path() / "Content");
    AETHER_CHECK(workspace.Scene().Document().Entities().size() == before + 1);          // switching projects does not throw away the open scene
}

#if defined(AETHER_HAS_EDITOR_VULKAN)
namespace {

std::unique_ptr<gfx::rhi::IDevice> TryDevice() {
    try {
        return gfx::rhi::CreateDevice(gfx::rhi::Backend::Vulkan, false);
    } catch (const std::exception&) {
        return nullptr;
    }
}

usize DifferingPixels(const std::vector<u8>& a, const std::vector<u8>& b) {
    usize differing = 0;
    for (usize i = 0; i + 3 < std::min(a.size(), b.size()); i += 4) {
        if (std::abs(int(a[i]) - int(b[i])) + std::abs(int(a[i + 1]) - int(b[i + 1])) + std::abs(int(a[i + 2]) - int(b[i + 2])) > 12) ++differing;
    }
    return differing;
}

} // namespace

AETHER_TEST(LiveViewport_RunsTheCookedSceneInProcessAndFollowsTheCamera) {
    std::unique_ptr<gfx::rhi::IDevice> device = TryDevice();
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    const stdfs::path project = MakeProject("Live");
    stdfs::path cooked;
    std::string error;
    AETHER_CHECK(CookForPreview(project, "Scenes/Main.ascene", project.parent_path() / "Saved" / "LiveTest", &cooked, &error));

    LiveViewport view(*device);
    AETHER_CHECK(!view.Loaded() && view.EntityCount() == 0 && view.FrameCount() == 0);
    AETHER_CHECK(!view.Frame(0.016f, Vec3(0, 5, 10), Quaternion::Identity()));      // nothing loaded: nothing to draw
    AETHER_CHECK(!view.Load(stdfs::temp_directory_path() / "aether_scene_preview_tests" / "no_such_cook", "Scenes/Main.ascene", 160, 90, &error) && !error.empty() && !view.Loaded());
    AETHER_CHECK(!view.Load(cooked, "Scenes/DoesNotExist.ascene", 160, 90, &error) && error.find("did not load") != std::string::npos && !view.Loaded());
    AETHER_CHECK(view.Load(cooked, "Scenes/Main.ascene", 160, 90, &error));
    AETHER_CHECK(view.Loaded() && view.Width() == 160 && view.Height() == 90 && view.EntityCount() >= 10);

    SceneViewCamera camera;
    for (int i = 0; i < 4; ++i) AETHER_CHECK(view.Frame(0.016f, camera.Eye(), camera.Rotation(), camera.fov_degrees));
    const std::vector<u8> first = view.Pixels();
    AETHER_CHECK(first.size() == 160u * 90u * 4u);
    std::set<u32> colours;                                                         // a lit scene, not an empty frame
    for (usize i = 0; i + 3 < first.size(); i += 4 * 7) colours.insert(u32(first[i]) << 16 | u32(first[i + 1]) << 8 | first[i + 2]);
    AETHER_CHECK(colours.size() > 20);
    camera.Orbit(1.5f, 0.2f);                                                      // the picture follows the camera
    for (int i = 0; i < 3; ++i) AETHER_CHECK(view.Frame(0.016f, camera.Eye(), camera.Rotation(), camera.fov_degrees));
    AETHER_CHECK(DifferingPixels(first, view.Pixels()) > 160u * 90u / 10);
    AETHER_CHECK(view.FrameCount() == 7);

    AETHER_CHECK(view.Load(cooked, "Scenes/Main.ascene", 96, 54, &error) && view.Width() == 96 && view.FrameCount() == 0); // a second load replaces the first
    view.Unload();
    AETHER_CHECK(!view.Loaded() && view.Pixels().empty() && view.Width() == 0);
}

// The overlay (selection outline, gizmo) is drawn with the Scene View's camera maths over the renderer's picture, so the two must agree: the point
// the overlay camera projects the player's origin to is where the real render has the player's blue pillar.
AETHER_TEST(LiveViewport_TheOverlayCameraLandsOnTheRenderedObject) {
    std::unique_ptr<gfx::rhi::IDevice> device = TryDevice();
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    const stdfs::path project = MakeProject("Alignment");
    ScenePanel scene(project.parent_path() / "Content");
    AETHER_CHECK(scene.OpenScene("Scenes/Main.ascene", true));
    SceneDocument& document = scene.Document();
    Entity player = kNullEntity;
    for (const Entity entity : document.Entities()) {
        if (document.GetWorld().HasComponent<ScriptComponent>(entity)) player = entity;
    }
    AETHER_CHECK(player != kNullEntity);
    if (player == kNullEntity) return;
    stdfs::path cooked;
    std::string error;
    AETHER_CHECK(CookForPreview(project, "Scenes/Main.ascene", project.parent_path() / "Saved" / "AlignTest", &cooked, &error));
    LiveViewport view(*device);
    constexpr u32 kWidth = 320, kHeight = 180;
    AETHER_CHECK(view.Load(cooked, "Scenes/Main.ascene", kWidth, kHeight, &error));
    SceneViewCamera camera; // the default orbit (yaw 0.7, pitch 0.5, 14 m), looking at a point beside the player, so the player is off the middle of the picture
    camera.target = Vec3(4.0f, 0.0f, 3.0f);
    for (int i = 0; i < 3; ++i) AETHER_CHECK(view.Frame(0.0f, camera.Eye(), camera.Rotation(), camera.fov_degrees));
    const Vec3 at = WorldPosition(document.GetWorld(), document.Guids(), player);
    ImVec2 pixel;
    AETHER_CHECK(ProjectToViewport(camera.ViewProjection(static_cast<f32>(kWidth) / static_cast<f32>(kHeight)), at, ImVec2(0, 0), ImVec2(kWidth, kHeight), pixel));
    // A blue pillar stands on that point: some pixel within a few pixels of it, and above it, is clearly blue (the ground and sky are grey and tan).
    const std::vector<u8>& rgba = view.Pixels();
    bool blue = false;
    for (int y = static_cast<int>(pixel.y) - 24; y <= static_cast<int>(pixel.y) + 3 && !blue; ++y) { // about the pillar's own height above its base
        for (int x = static_cast<int>(pixel.x) - 4; x <= static_cast<int>(pixel.x) + 4 && !blue; ++x) {
            if (x < 0 || y < 0 || x >= static_cast<int>(kWidth) || y >= static_cast<int>(kHeight)) continue;
            const u8* p = &rgba[(static_cast<usize>(y) * kWidth + static_cast<usize>(x)) * 4];
            blue = p[2] > p[0] + 40 && p[2] > p[1] + 15;
        }
    }
    AETHER_CHECK(blue);
    // And a point well off to the side of it is not: the check can fail.
    bool blue_elsewhere = false;
    for (int y = 0; y < static_cast<int>(kHeight); ++y) {
        for (int x = 0; x < 40; ++x) {
            const u8* p = &rgba[(static_cast<usize>(y) * kWidth + static_cast<usize>(x)) * 4];
            blue_elsewhere |= p[2] > p[0] + 40 && p[2] > p[1] + 15;
        }
    }
    AETHER_CHECK(!blue_elsewhere);
}

AETHER_TEST(LiveViewportPanel_StartsFromTheOpenSceneAndShowsFrames) {
    std::unique_ptr<gfx::rhi::IDevice> device = TryDevice();
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    Context imgui;
    const stdfs::path project = MakeProject("LivePanel");
    ScenePanel scene(project.parent_path() / "Content");
    LiveViewportPanel none(*device, scene, [] { return stdfs::path(); });
    AETHER_CHECK(!none.Start() && none.Note() == "No project is open.");

    LiveViewportPanel panel(*device, scene, [&] { return project; });
    panel.width = 160;
    panel.height = 90;
    AETHER_CHECK(scene.OpenScene("Scenes/Main.ascene", true));
    std::string main_on_disk;
    AETHER_CHECK(fs::ReadFileText((project.parent_path() / "Content" / "Scenes/Main.ascene").string(), main_on_disk));
    AETHER_CHECK(panel.Start() && panel.Viewport().Loaded() && panel.Note().empty());
    AETHER_CHECK(panel.Advance(1.0f));                                              // a frame is due, drawn and shown
    AETHER_CHECK(panel.Texture().Valid() && panel.Texture().Width() == 160 && panel.Texture().Height() == 90);
    AETHER_CHECK(!panel.Advance(0.001f));                                           // not due again yet: the rate is capped
    AETHER_CHECK(panel.Advance(0.1f) && panel.FramesPerSecond() > 0.0f);
    scene.Document().SetSelection({scene.Document().Entities()[0]});                // with something selected, the outline and the gizmo are drawn too
    ImGui::Begin("live");
    panel.Draw(0.05f);
    ImGui::End();
    std::string after;
    AETHER_CHECK(fs::ReadFileText((project.parent_path() / "Content" / "Scenes/Main.ascene").string(), after) && after == main_on_disk); // nothing was written over the project's scene
    panel.Stop();
    AETHER_CHECK(!panel.Viewport().Loaded() && !panel.Advance(1.0f));
}

// An edit shows without saving: the scene goes to the game from memory, and each change to the document reloads it.
AETHER_TEST(LiveViewportPanel_AnEditShowsWithoutSavingAndAModelNotCookedIsSaid) {
    std::unique_ptr<gfx::rhi::IDevice> device = TryDevice();
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    Context imgui;
    const stdfs::path project = MakeProject("LiveEdit");
    ScenePanel scene(project.parent_path() / "Content");
    AETHER_CHECK(scene.OpenScene("Scenes/Main.ascene", true));
    LiveViewportPanel panel(*device, scene, [&] { return project; });
    panel.width = 160;
    panel.height = 90;
    panel.reload_interval = 0.0f;
    AETHER_CHECK(panel.Start() && panel.Advance(1.0f));
    const std::vector<u8> before = panel.Viewport().Pixels();
    const u64 reloads = panel.Viewport().SceneReloads();
    AETHER_CHECK(!panel.Advance(1.0f) || panel.Viewport().SceneReloads() == reloads);      // nothing changed: no reload

    // Remove every model from the scene (the pillar and the cubes): the picture must lose them without a save.
    SceneDocument& document = scene.Document();
    std::vector<Entity> with_models;
    for (const Entity entity : document.Entities()) {
        if (document.GetWorld().HasComponent<ModelRenderer>(entity)) with_models.push_back(entity);
    }
    AETHER_CHECK(!with_models.empty());
    for (const Entity entity : with_models) document.DestroyEntity(entity);
    AETHER_CHECK(panel.Advance(0.2f));
    AETHER_CHECK(panel.Viewport().SceneReloads() == reloads + 1);
    AETHER_CHECK(DifferingPixels(before, panel.Viewport().Pixels()) > 160u * 90u / 50);   // the models are gone from the picture
    document.Undo();                                                                       // and undo brings them back
    for (usize i = 1; i < with_models.size(); ++i) document.Undo();
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads + 2);
    AETHER_CHECK(DifferingPixels(before, panel.Viewport().Pixels()) < 160u * 90u / 50);

    // A model the cook was not asked for is said, and Reload (a new cook) takes it in.
    AETHER_CHECK(panel.UncookedModels() == 0);
    document.CreateEntity("Rock", SceneEntityKind::Model, "Models/Rock.glb");
    AETHER_CHECK(panel.UncookedModels() == 1);
    AETHER_CHECK(panel.Start() && panel.UncookedModels() == 0);
    // A gizmo drag is seen while it is still going (it writes into the scene without a command, and says so).
    const u64 revision = document.Revision();
    document.Touch();
    AETHER_CHECK(document.Revision() == revision + 1);
}
#endif

AETHER_TEST(GamePackage_AContentOverrideIsReadInPlaceOfTheArchive) {
    player::GamePackage package;
    std::vector<u8> out;
    AETHER_CHECK(!package.ReadContent("Scenes/Live.ascene", out));                 // nothing mounted: nothing there
    package.SetContentOverride("Scenes/Live.ascene", {1, 2, 3});
    AETHER_CHECK(package.ReadContent("Scenes/Live.ascene", out) && out == std::vector<u8>({1, 2, 3}));
    package.SetContentOverride("Scenes/Live.ascene", {9});                          // replaced
    AETHER_CHECK(package.ReadContent("Scenes/Live.ascene", out) && out == std::vector<u8>({9}));
    AETHER_CHECK(!package.ReadContent("Scenes/Other.ascene", out));                 // only the named path
    package.ClearContentOverrides();
    AETHER_CHECK(!package.ReadContent("Scenes/Live.ascene", out));
}
