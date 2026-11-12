#include "core/scene_preview.h"
#include "ui/image_texture.h"
#include "workspace/editor_workspace.h"
#if defined(AETHER_HAS_EDITOR_VULKAN)
#include "host/live_viewport_panel.h"
#endif
#include "content/content_drag.h"
#include "world/content_drop.h"
#include "world/scene_preview_panel.h"

#include "aether/core/log.h"
#include "aether/scene/components.h"
#include "aether/assets/png_writer.h"
#include "aether/renderer/components.h"
#include "aether/renderer/material_instance.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/hierarchy.h"
#include "aether/scene/prefab.h"
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
#include <fstream>
#include <imgui.h>
#include <imgui_internal.h> // the drop target the mouse is over
#include <memory>
#include "aether/scene/serialization.h"
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

// An entity with a character controller, which the running game sets up once when it starts (so putting one in is a reload's job), or kNullEntity in a build
// without physics. The entity and the component are both in the document when this returns.
Entity MakeWalker(SceneDocument& document, const char* name) {
    const ComponentId character = FindComponentIdByName("CharacterMovement");
    if (character == kInvalidComponentId) return kNullEntity;
    const Entity walker = document.CreateEntity(name);
    return document.AddComponent(walker, character) ? walker : kNullEntity;
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
AETHER_TEST(LiveViewportPanel_AMovedObjectIsPatchedInPlaceAndOnlyAChangeToTheSceneReloads) {
    std::unique_ptr<gfx::rhi::IDevice> device = TryDevice();
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    Context imgui;
    const stdfs::path project = MakeProject("LivePatch");
    ScenePanel scene(project.parent_path() / "Content");
    AETHER_CHECK(scene.OpenScene("Scenes/Main.ascene", true));
    LiveViewportPanel panel(*device, scene, [&] { return project; });
    panel.width = 160;
    panel.height = 90;
    panel.reload_interval = 0.0f;
    AETHER_CHECK(panel.Start() && panel.Advance(1.0f));
    SceneDocument& document = scene.Document();
    const std::vector<u8> before = panel.Viewport().Pixels();
    const u64 reloads = panel.Viewport().SceneReloads();
    AETHER_CHECK(panel.Viewport().PatchCount() == 0);

    // Every model in the scene, so a move shows clearly: they are the pillar and the cubes.
    std::vector<Entity> models;
    std::vector<Vec3> homes;
    for (const Entity entity : document.Entities()) {
        const Transform* transform = document.GetWorld().GetComponent<Transform>(entity);
        if (transform && document.GetWorld().HasComponent<ModelRenderer>(entity)) {
            models.push_back(entity);
            homes.push_back(transform->position);
        }
    }
    AETHER_CHECK(models.size() >= 2);
    if (models.size() < 2) return;
    const Vec3 lift(0.0f, 30.0f, 0.0f);
    auto move_all = [&](const Vec3& by, bool as_command) {
        for (usize i = 0; i < models.size(); ++i) {
            if (as_command) {
                document.SetPosition(models[i], homes[i] + by, true);
            } else if (Transform* transform = document.GetWorld().GetComponent<Transform>(models[i])) {
                transform->position = homes[i] + by; // what a gizmo drag does, before it records anything
            }
        }
        if (!as_command) document.TouchTransforms();
    };

    // The document says what kind of change it was: a move is not structural; a Touch or a new entity is.
    const u64 structure = document.StructureRevision(), revision = document.Revision();
    move_all(lift, true);
    AETHER_CHECK(document.Revision() > revision && document.StructureRevision() == structure);
    document.TouchTransforms();
    AETHER_CHECK(document.StructureRevision() == structure);

    // The move is in the picture, and the scene was not loaded again.
    AETHER_CHECK(panel.Advance(0.2f));
    AETHER_CHECK(panel.Viewport().PatchCount() == 1 && panel.Viewport().SceneReloads() == reloads);
    AETHER_CHECK(DifferingPixels(before, panel.Viewport().Pixels()) > 160u * 90u / 50);
    move_all(Vec3(0, 0, 0), true);                                                       // and back
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().PatchCount() == 2 && panel.Viewport().SceneReloads() == reloads);
    AETHER_CHECK(DifferingPixels(before, panel.Viewport().Pixels()) < 160u * 90u / 50);

    // A gizmo drag writes into the components and says so; that is patched too.
    move_all(lift, false);
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().PatchCount() == 3 && panel.Viewport().SceneReloads() == reloads);
    AETHER_CHECK(DifferingPixels(before, panel.Viewport().Pixels()) > 160u * 90u / 50);
    move_all(Vec3(0, 0, 0), false);
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().PatchCount() == 4);

    // A change that is not only a move loads the scene again (here a Touch(), and then a model that carries a physics body or a script, which the running
    // game builds when it loads), and a move after it is patched against the new baseline.
    document.Touch();
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads + 1 && panel.Viewport().PatchCount() == 4);
    document.DestroyEntity(models[1]);
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads + 2 && panel.Viewport().PatchCount() == 4);
    document.SetPosition(models[0], homes[0] + Vec3(1.0f, 0.0f, 0.0f), true);
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().PatchCount() == 5 && panel.Viewport().SceneReloads() == reloads + 2);
    document.Undo();                                                                     // undoing a move is patched like the move
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads + 2 && panel.Viewport().PatchCount() == 6);
    document.Undo();                                                                     // bringing back a model with a physics body is not: the scene loads again
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads + 3 && panel.Viewport().PatchCount() == 6);

    // An entity that carries something the game sets up once when it starts (a character controller here) is not added to it: that is a reload's job, and the
    // patch refuses, having changed nothing.
    if (MakeWalker(document, "Extra") != kNullEntity) AETHER_CHECK(!panel.Viewport().PatchTransforms(document.GetWorld()) && panel.Viewport().PatchCount() == 6);
}

AETHER_TEST(LiveViewportPanel_ALightEditShowsWithoutReloadingTheScene) {
    std::unique_ptr<gfx::rhi::IDevice> device = TryDevice();
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    Context imgui;
    const stdfs::path project = MakeProject("LiveLight");
    ScenePanel scene(project.parent_path() / "Content");
    AETHER_CHECK(scene.OpenScene("Scenes/Main.ascene", true));
    LiveViewportPanel panel(*device, scene, [&] { return project; });
    panel.width = 160;
    panel.height = 90;
    panel.reload_interval = 0.0f;
    AETHER_CHECK(panel.Start() && panel.Advance(1.0f));
    SceneDocument& document = scene.Document();
    const u64 reloads = panel.Viewport().SceneReloads();
    u64 reloads_after_add = 0;
    Entity camera = kNullEntity, sky = kNullEntity;
    for (const Entity entity : document.Entities()) {
        if (document.GetWorld().HasComponent<Camera>(entity)) camera = entity;
        if (document.GetWorld().HasComponent<SkyLight>(entity)) sky = entity;
    }
    AETHER_CHECK(camera != kNullEntity && sky != kNullEntity);
    if (camera == kNullEntity || sky == kNullEntity) return;
    // The template has a sky and no sun: add one (a new entity with a light is patched in), and then edit it.
    const Entity sun = document.CreateEntity("Sun");
    AETHER_CHECK(document.AddComponent(sun, GetComponentId<DirectionalLight>()));
    document.SetRotation(sun, Quaternion::FromAxisAngle(Vec3(1, 0, 0), -0.9f), true);
    const usize entities = panel.Viewport().EntityCount();
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().PatchCount() == 1 && panel.Viewport().SceneReloads() == reloads); // an entity and its light, patched in
    AETHER_CHECK(panel.Viewport().EntityCount() == entities + 1);
    for (int i = 0; i < 6; ++i) panel.Advance(0.2f); // the renderer settles (exposure, history) before the picture is taken as the reference
    const std::vector<u8> lit = panel.Viewport().Pixels();
    const ComponentId light = GetComponentId<DirectionalLight>();
    reloads_after_add = panel.Viewport().SceneReloads();
    // The sun's intensity to nothing: the picture goes dark, and it is a patch, not a reload.
    AETHER_CHECK(document.SetComponentField(sun, light, "intensity", reflect::Any(0.0f), true));
    AETHER_CHECK(panel.Advance(0.2f));
    AETHER_CHECK(panel.Viewport().PatchCount() == 2 && panel.Viewport().SceneReloads() == reloads_after_add);
    const std::vector<u8> dark = panel.Viewport().Pixels();
    AETHER_CHECK(DifferingPixels(lit, dark) > 160u * 90u / 10);
    // And back: the picture is what it was.
    AETHER_CHECK(document.SetComponentField(sun, light, "intensity", reflect::Any(3.0f), true));
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().PatchCount() == 3 && panel.Viewport().SceneReloads() == reloads_after_add);
    for (int i = 0; i < 6; ++i) panel.Advance(0.2f);
    AETHER_CHECK(DifferingPixels(lit, panel.Viewport().Pixels()) * 5 < DifferingPixels(lit, dark)); // much nearer the lit picture than the dark one

    // The colour, the shadow switch and where the light stands are patched too (each one edit).
    AETHER_CHECK(document.SetComponentField(sun, light, "color", reflect::Any(Vec3(1.0f, 0.1f, 0.1f)), true));
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().PatchCount() == 4 && panel.Viewport().SceneReloads() == reloads_after_add);
    AETHER_CHECK(DifferingPixels(lit, panel.Viewport().Pixels()) > 160u * 90u / 10);
    AETHER_CHECK(document.SetComponentField(sun, light, "color", reflect::Any(Vec3(1.0f, 1.0f, 1.0f)), true));
    AETHER_CHECK(document.SetComponentField(sun, light, "cast_shadows", reflect::Any(false), true));
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().PatchCount() == 5 && panel.Viewport().SceneReloads() == reloads_after_add);
    AETHER_CHECK(document.SetComponentField(sun, light, "cast_shadows", reflect::Any(true), true));
    AETHER_CHECK(document.SetRotation(sun, Quaternion::FromAxisAngle(Vec3(1, 0, 0), -0.4f), true)); // turning the sun moves the shadows
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().PatchCount() == 6 && panel.Viewport().SceneReloads() == reloads_after_add);

    // The sky's own light (what the template scene has) is patched the same way.
    for (int i = 0; i < 6; ++i) panel.Advance(0.2f);
    const std::vector<u8> before_sky = panel.Viewport().Pixels();
    const u64 patches = panel.Viewport().PatchCount();
    AETHER_CHECK(document.SetComponentField(sky, GetComponentId<SkyLight>(), "intensity", reflect::Any(0.0f), true));
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().PatchCount() == patches + 1 && panel.Viewport().SceneReloads() == reloads_after_add);
    AETHER_CHECK(DifferingPixels(before_sky, panel.Viewport().Pixels()) > 160u * 90u / 10);

    // The camera is read from the world every frame, so editing it is a patch too (the viewport's own camera overrides its view and field of view, so the picture
    // does not change).
    AETHER_CHECK(document.SetComponentField(camera, GetComponentId<Camera>(), "fov_degrees", reflect::Any(70.0f), true));
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().PatchCount() == patches + 2 && panel.Viewport().SceneReloads() == reloads_after_add); // the sky's edit above was the first
    // A component the game sets up once when it starts (a character controller) is not patched: the scene loads again, for the edit of it and for the undo.
    if (const Entity walker = MakeWalker(document, "Walker"); walker != kNullEntity) {
        const ComponentId character = FindComponentIdByName("CharacterMovement");
        AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads_after_add + 1);
        AETHER_CHECK(document.SetComponentField(walker, character, "walk_speed", reflect::Any(5.0f), true));
        AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads_after_add + 2);
        AETHER_CHECK(document.Undo());
        AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads_after_add + 3);
    }
}

AETHER_TEST(LiveViewportPanel_AddingAndRemovingObjectsDoesNotRebuildTheWorld) {
    std::unique_ptr<gfx::rhi::IDevice> device = TryDevice();
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    Context imgui;
    const stdfs::path project = MakeProject("LiveLifecycle");
    ScenePanel scene(project.parent_path() / "Content");
    AETHER_CHECK(scene.OpenScene("Scenes/Main.ascene", true));
    LiveViewportPanel panel(*device, scene, [&] { return project; });
    panel.width = 160;
    panel.height = 90;
    panel.reload_interval = 0.0f;
    AETHER_CHECK(panel.Start());
    for (int i = 0; i < 6; ++i) panel.Advance(0.2f); // the renderer settles before the picture is taken as the reference
    SceneDocument& document = scene.Document();
    const std::vector<u8> before = panel.Viewport().Pixels();
    const u64 reloads = panel.Viewport().SceneReloads();
    const usize count = panel.Viewport().EntityCount();
    std::string model;
    for (const Entity entity : document.Entities()) {
        const ModelRenderer* renderer = document.GetWorld().GetComponent<ModelRenderer>(entity);
        if (renderer && model.empty()) model = renderer->asset_path;
    }
    AETHER_CHECK(!model.empty());
    if (model.empty()) return;

    // A model added to the scene is in the running game a moment later, and the scene was not loaded again.
    const Entity boulder = document.CreateEntity("Boulder", SceneEntityKind::Model, model);
    document.SetPosition(boulder, Vec3(0.0f, 2.0f, 0.0f), true);
    document.SetScale(boulder, Vec3(6.0f, 6.0f, 6.0f), true);
    AETHER_CHECK(panel.Advance(0.2f));
    AETHER_CHECK(panel.Viewport().PatchCount() == 1 && panel.Viewport().SceneReloads() == reloads);
    AETHER_CHECK(panel.Viewport().EntityCount() == count + 1);
    for (int i = 0; i < 4; ++i) panel.Advance(0.2f);
    const std::vector<u8> with_boulder = panel.Viewport().Pixels();
    AETHER_CHECK(DifferingPixels(before, with_boulder) > 160u * 90u / 50);

    // A copy of it, and a model placed from the Content Browser, are patched in too (one entity each).
    AETHER_CHECK(document.DuplicateEntity(boulder, Vec3(0.0f, 0.0f, 0.0f)) != kNullEntity);
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().PatchCount() == 2 && panel.Viewport().SceneReloads() == reloads && panel.Viewport().EntityCount() == count + 2);
    const Entity placed = PlaceModelAsset(document, model, Vec3(3.0f, 0.0f, 0.0f));
    AETHER_CHECK(placed != kNullEntity);
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().PatchCount() == 3 && panel.Viewport().SceneReloads() == reloads && panel.Viewport().EntityCount() == count + 3);

    // Taking them away again removes them from the running game, and the picture is what it was.
    document.DestroyEntity(placed);
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().PatchCount() == 4 && panel.Viewport().EntityCount() == count + 2);
    for (const Entity entity : document.Entities()) {
        if (document.Name(entity) == "Boulder Copy" || entity == boulder) document.DestroyEntity(entity);
    }
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads && panel.Viewport().EntityCount() == count);
    for (int i = 0; i < 6; ++i) panel.Advance(0.2f);
    AETHER_CHECK(DifferingPixels(before, panel.Viewport().Pixels()) * 5 < DifferingPixels(before, with_boulder));

    // A prefab instance (the root and a child tied by a parent link) is patched in whole.
    {
        const stdfs::path content = project.parent_path() / "Content";
        PrefabData crate;
        PrefabEntity top;
        top.id = 1;
        PrefabEntity lid;
        lid.id = 2;
        lid.parent = 1;
        crate.entities = {top, lid};
        stdfs::create_directories(content / "Prefabs");
        AETHER_CHECK(SavePrefab(crate, content / "Prefabs" / "Crate.aprefab"));
        const u64 patches = panel.Viewport().PatchCount();
        AETHER_CHECK(PlacePrefabAsset(document, content, "Prefabs/Crate.aprefab", Vec3(1, 0, 1)) != kNullEntity);
        AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().PatchCount() == patches + 1 && panel.Viewport().SceneReloads() == reloads && panel.Viewport().EntityCount() == count + 2);
    }

    // An object dropped on an entity in the hierarchy goes in under it as one create, so it is patched in too, parent link and all.
    {
        const Entity holder = document.CreateEntity("Holder");
        AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads);
        const u64 patches = panel.Viewport().PatchCount();
        const usize entities = panel.Viewport().EntityCount();
        bool placed = false;
        PlaceContentAsset(document, project.parent_path() / "Content", model, Vec3(0.0f, 0.0f, 0.0f), &placed, holder);
        AETHER_CHECK(placed && panel.Advance(0.2f) && panel.Viewport().PatchCount() == patches + 1 && panel.Viewport().SceneReloads() == reloads);
        AETHER_CHECK(panel.Viewport().EntityCount() == entities + 1);
    }

    // A component added to, and taken from, an entity that stays: a light on an empty entity lights the scene, and removing it puts it back.
    const Entity lamp = document.CreateEntity("Lamp");
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads);
    for (int i = 0; i < 6; ++i) panel.Advance(0.2f);
    const std::vector<u8> no_lamp = panel.Viewport().Pixels();
    AETHER_CHECK(document.AddComponent(lamp, GetComponentId<DirectionalLight>()));
    document.SetRotation(lamp, Quaternion::FromAxisAngle(Vec3(1, 0, 0), -0.9f), true);
    const u64 before_lamp = panel.Viewport().PatchCount();
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().PatchCount() == before_lamp + 1 && panel.Viewport().SceneReloads() == reloads);
    for (int i = 0; i < 6; ++i) panel.Advance(0.2f);
    const std::vector<u8> lamp_on = panel.Viewport().Pixels();
    AETHER_CHECK(DifferingPixels(no_lamp, lamp_on) > 160u * 90u / 50);
    AETHER_CHECK(document.RemoveComponent(lamp, GetComponentId<DirectionalLight>()));
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().PatchCount() == before_lamp + 2 && panel.Viewport().SceneReloads() == reloads);
    for (int i = 0; i < 6; ++i) panel.Advance(0.2f);
    AETHER_CHECK(DifferingPixels(no_lamp, panel.Viewport().Pixels()) * 5 < DifferingPixels(no_lamp, lamp_on));

    // A second camera is read from the world every frame like any other object, so it is patched in too, and out again.
    const usize with_camera = panel.Viewport().EntityCount();
    const u64 patches_before = panel.Viewport().PatchCount();
    document.CreateEntity("Second Camera", SceneEntityKind::Camera);
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads && panel.Viewport().PatchCount() == patches_before + 1);
    AETHER_CHECK(panel.Viewport().EntityCount() == with_camera + 1);
    AETHER_CHECK(document.Undo());
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads && panel.Viewport().EntityCount() == with_camera);
    // What the game sets up once when it starts (a character controller) is not patched in: the scene loads again, and so does the undo.
    if (MakeWalker(document, "Walker") != kNullEntity) {
        AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads + 1);
        AETHER_CHECK(document.Undo());
        AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads + 2);
    }
}

AETHER_TEST(LiveViewportPanel_AMaterialOverrideShowsWithoutReloadingTheScene) {
    std::unique_ptr<gfx::rhi::IDevice> device = TryDevice();
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    Context imgui;
    const stdfs::path project = MakeProject("LiveMaterial");
    ScenePanel scene(project.parent_path() / "Content");
    AETHER_CHECK(scene.OpenScene("Scenes/Main.ascene", true));
    LiveViewportPanel panel(*device, scene, [&] { return project; });
    panel.width = 160;
    panel.height = 90;
    panel.reload_interval = 0.0f;
    AETHER_CHECK(panel.Start());
    for (int i = 0; i < 6; ++i) panel.Advance(0.2f);
    SceneDocument& document = scene.Document();
    const std::vector<u8> before = panel.Viewport().Pixels();
    const u64 reloads = panel.Viewport().SceneReloads();
    std::vector<Entity> models;
    for (const Entity entity : document.Entities()) {
        if (document.GetWorld().HasComponent<ModelRenderer>(entity)) models.push_back(entity);
    }
    AETHER_CHECK(models.size() >= 2);
    const ComponentId overrides = GetComponentId<MaterialParameters>();
    auto set_values = [&](const std::vector<MaterialParameterValue>& values) {
        for (const Entity entity : models) {
            AETHER_CHECK(document.SetComponentField(entity, overrides, "values", reflect::Any(values), true));
        }
    };

    // A component added to every model with a red base colour: the models turn red in the picture, and the scene was not loaded again.
    for (const Entity entity : models) AETHER_CHECK(document.AddComponent(entity, overrides));
    MaterialParameterValue red;
    red.name = "BaseColor";
    red.value = Vec4(1.0f, 0.0f, 0.0f, 1.0f);
    set_values({red});
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads && panel.Viewport().PatchCount() >= 1);
    for (int i = 0; i < 4; ++i) panel.Advance(0.2f);
    const std::vector<u8> reddened = panel.Viewport().Pixels();
    AETHER_CHECK(DifferingPixels(before, reddened) > 160u * 90u / 50);
    // Changing the value changes the picture again; taking the component away puts the model's own surface back.
    red.value = Vec4(0.0f, 1.0f, 0.0f, 1.0f);
    set_values({red});
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads);
    for (int i = 0; i < 4; ++i) panel.Advance(0.2f);
    AETHER_CHECK(DifferingPixels(reddened, panel.Viewport().Pixels()) > 160u * 90u / 50);
    for (const Entity entity : models) AETHER_CHECK(document.RemoveComponent(entity, overrides));
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads);
    for (int i = 0; i < 6; ++i) panel.Advance(0.2f);
    AETHER_CHECK(DifferingPixels(before, panel.Viewport().Pixels()) * 5 < DifferingPixels(before, reddened));

    // A texture the cook does not have yet is counted, Reload cooks it, and then the texture is on the models. (A path that is not a file is left out.)
    const stdfs::path content = project.parent_path() / "Content";
    stdfs::create_directories(content / "textures");
    std::vector<u8> rgb(4 * 4 * 3);
    for (usize i = 0; i < rgb.size(); i += 3) {
        rgb[i] = 10;
        rgb[i + 1] = 10;
        rgb[i + 2] = 240;
    }
    const std::vector<u8> png = assets::EncodePngRgb8(4, 4, rgb);
    std::ofstream(content / "textures" / "blue.png", std::ios::binary).write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    MaterialParameterValue texture;
    texture.name = "BaseColorTexture";
    texture.is_texture = true;
    texture.texture = "textures/blue.png";
    MaterialParameterValue missing = texture;
    missing.name = "Other";
    missing.texture = "textures/not_there.png";
    for (const Entity entity : models) AETHER_CHECK(document.AddComponent(entity, overrides));
    set_values({texture, missing});
    AETHER_CHECK(panel.UncookedModels() == 1);                               // the file there, not the typo
    AETHER_CHECK(panel.Start() && panel.UncookedModels() == 0);
    for (int i = 0; i < 6; ++i) panel.Advance(0.2f);
    const std::vector<u8> textured = panel.Viewport().Pixels();
    AETHER_CHECK(DifferingPixels(before, textured) > 160u * 90u / 50);

    // Clearing the slot (an empty path) gives the model's own texture back, without a reload.
    const u64 reloads_before_clear = panel.Viewport().SceneReloads();
    texture.texture.clear();
    set_values({texture});
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads_before_clear);
    for (int i = 0; i < 6; ++i) panel.Advance(0.2f);
    AETHER_CHECK(DifferingPixels(before, panel.Viewport().Pixels()) * 5 < DifferingPixels(before, textured));
}

AETHER_TEST(LiveViewportPanel_UndoAndRedoOfPatchableEditsAreNotReloads) {
    std::unique_ptr<gfx::rhi::IDevice> device = TryDevice();
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    Context imgui;
    const stdfs::path project = MakeProject("LiveUndo");
    ScenePanel scene(project.parent_path() / "Content");
    AETHER_CHECK(scene.OpenScene("Scenes/Main.ascene", true));
    LiveViewportPanel panel(*device, scene, [&] { return project; });
    panel.width = 160;
    panel.height = 90;
    panel.reload_interval = 0.0f;
    AETHER_CHECK(panel.Start());
    SceneDocument& document = scene.Document();
    auto settle = [&] { for (int i = 0; i < 6; ++i) panel.Advance(0.2f); };
    settle();
    const u64 reloads = panel.Viewport().SceneReloads();
    const usize count = panel.Viewport().EntityCount();
    std::string model;
    Entity sky = kNullEntity;
    for (const Entity entity : document.Entities()) {
        const ModelRenderer* renderer = document.GetWorld().GetComponent<ModelRenderer>(entity);
        if (renderer && model.empty()) model = renderer->asset_path;
        if (document.GetWorld().HasComponent<SkyLight>(entity)) sky = entity;
    }
    AETHER_CHECK(!model.empty() && sky != kNullEntity);
    if (model.empty() || sky == kNullEntity) return;
    document.Commands().Clear();
    const std::vector<u8> start = panel.Viewport().Pixels();
    u64 patches = panel.Viewport().PatchCount();
    auto patched = [&](const char* what) {
        const bool ok = panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads && panel.Viewport().PatchCount() == patches + 1;
        if (!ok) std::fprintf(stderr, "    (not patched: %s)\n", what);
        patches = panel.Viewport().PatchCount();
        return ok;
    };

    // A light edit, undone and redone: each is a patch, the picture goes dark and comes back.
    AETHER_CHECK(document.SetComponentField(sky, GetComponentId<SkyLight>(), "intensity", reflect::Any(0.0f), true));
    AETHER_CHECK(patched("the edit"));
    settle();
    const std::vector<u8> dark = panel.Viewport().Pixels();
    AETHER_CHECK(DifferingPixels(start, dark) > 160u * 90u / 10);
    AETHER_CHECK(document.Undo());
    AETHER_CHECK(patched("undo of the edit"));
    settle();
    AETHER_CHECK(DifferingPixels(start, panel.Viewport().Pixels()) * 5 < DifferingPixels(start, dark));
    AETHER_CHECK(document.Redo());
    AETHER_CHECK(patched("redo of the edit"));
    settle();
    AETHER_CHECK(DifferingPixels(dark, panel.Viewport().Pixels()) * 5 < DifferingPixels(start, dark));
    AETHER_CHECK(document.Undo());
    AETHER_CHECK(patched("undo, to put it back"));

    // An object added and undone, and redone: it leaves the running game and comes back.
    document.CreateEntity("Boulder", SceneEntityKind::Model, model);
    AETHER_CHECK(patched("the new object") && panel.Viewport().EntityCount() == count + 1);
    AETHER_CHECK(document.Undo());
    AETHER_CHECK(patched("undo of the new object") && panel.Viewport().EntityCount() == count);
    AETHER_CHECK(document.Redo());
    AETHER_CHECK(patched("redo of the new object") && panel.Viewport().EntityCount() == count + 1);

    // A placed model is a create and a move in one entry: one undo, one patch.
    const Entity placed = PlaceModelAsset(document, model, Vec3(2.0f, 0.0f, 0.0f));
    AETHER_CHECK(placed != kNullEntity && patched("the placed model") && panel.Viewport().EntityCount() == count + 2);
    AETHER_CHECK(document.Undo());
    AETHER_CHECK(patched("undo of the placed model") && panel.Viewport().EntityCount() == count + 1);

    // Two edits grouped as one step (a transaction) are undone together and patched together, the components they touch included.
    const Entity sun = document.CreateEntity("Sun");
    AETHER_CHECK(document.AddComponent(sun, GetComponentId<DirectionalLight>()));
    AETHER_CHECK(patched("the sun"));
    settle();
    const std::vector<u8> before_group = panel.Viewport().Pixels();
    document.Commands().BeginTransaction("Dim the sky");
    AETHER_CHECK(document.SetComponentField(sky, GetComponentId<SkyLight>(), "intensity", reflect::Any(0.0f), true));
    AETHER_CHECK(document.SetComponentField(sky, GetComponentId<SkyLight>(), "color", reflect::Any(Vec3(0.0f, 0.0f, 1.0f)), true));
    document.Commands().EndTransaction();
    AETHER_CHECK(patched("the grouped edit"));
    settle();
    const std::vector<u8> grouped = panel.Viewport().Pixels();
    AETHER_CHECK(DifferingPixels(before_group, grouped) > 160u * 90u / 10);
    AETHER_CHECK(document.Undo());
    AETHER_CHECK(patched("undo of the grouped edit"));
    settle();
    AETHER_CHECK(DifferingPixels(before_group, panel.Viewport().Pixels()) * 5 < DifferingPixels(before_group, grouped)); // both fields are back

    // A component added to an object and undone: taken off again.
    const Entity lamp = document.CreateEntity("Lamp");
    AETHER_CHECK(patched("the lamp"));
    AETHER_CHECK(document.AddComponent(lamp, GetComponentId<PointLight>()));
    AETHER_CHECK(patched("the lamp's light"));
    AETHER_CHECK(document.Undo());
    AETHER_CHECK(patched("undo of the lamp's light"));

    // What the game sets up once when it starts still loads the scene again, on undo too: an entity with a character controller.
    if (MakeWalker(document, "Walker") != kNullEntity) {
        AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads + 1);
        AETHER_CHECK(document.Undo());
        AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads + 2);
    }
}

AETHER_TEST(LiveViewportPanel_ReparentingAnObjectIsPatchedAndOnlyAnEntityWithACharacterReloads) {
    std::unique_ptr<gfx::rhi::IDevice> device = TryDevice();
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    Context imgui;
    const stdfs::path project = MakeProject("LiveReparent");
    ScenePanel scene(project.parent_path() / "Content");
    AETHER_CHECK(scene.OpenScene("Scenes/Main.ascene", true));
    LiveViewportPanel panel(*device, scene, [&] { return project; });
    panel.width = 160;
    panel.height = 90;
    panel.reload_interval = 0.0f;
    AETHER_CHECK(panel.Start());
    SceneDocument& document = scene.Document();
    auto settle = [&] { for (int i = 0; i < 6; ++i) panel.Advance(0.2f); };
    settle();
    const u64 reloads = panel.Viewport().SceneReloads();
    std::string model;
    for (const Entity entity : document.Entities()) {
        const ModelRenderer* renderer = document.GetWorld().GetComponent<ModelRenderer>(entity);
        if (renderer && model.empty()) model = renderer->asset_path;
    }
    AETHER_CHECK(!model.empty());
    if (model.empty()) return;
    u64 patches = panel.Viewport().PatchCount();
    auto patched = [&](const char* what) {
        const bool ok = panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads && panel.Viewport().PatchCount() == patches + 1;
        if (!ok) std::fprintf(stderr, "    (not patched: %s)\n", what);
        patches = panel.Viewport().PatchCount();
        return ok;
    };

    // A holder far above the scene, and a big boulder at the origin that fills the view.
    const Entity holder = document.CreateEntity("Holder");
    document.SetPosition(holder, Vec3(0.0f, 40.0f, 0.0f), true);
    const Entity boulder = document.CreateEntity("Boulder", SceneEntityKind::Model, model);
    document.SetPosition(boulder, Vec3(0.0f, 2.0f, 0.0f), true);
    document.SetScale(boulder, Vec3(6.0f, 6.0f, 6.0f), true);
    AETHER_CHECK(patched("the two new objects"));
    settle();
    const std::vector<u8> at_origin = panel.Viewport().Pixels();
    document.Commands().Clear();

    // Put the boulder under the holder: it keeps its own transform, so it is now forty metres up, out of the picture. A patch, not a reload.
    AETHER_CHECK(document.ReparentEntity(boulder, holder));
    AETHER_CHECK(patched("the reparent"));
    settle();
    const std::vector<u8> lifted = panel.Viewport().Pixels();
    AETHER_CHECK(DifferingPixels(at_origin, lifted) > 160u * 90u / 50);
    // Undoing it, and doing it again, are patches too, and the picture follows each.
    AETHER_CHECK(document.Undo());
    AETHER_CHECK(patched("undo of the reparent"));
    settle();
    AETHER_CHECK(DifferingPixels(at_origin, panel.Viewport().Pixels()) * 5 < DifferingPixels(at_origin, lifted));
    AETHER_CHECK(document.Redo());
    AETHER_CHECK(patched("redo of the reparent"));
    settle();
    AETHER_CHECK(DifferingPixels(lifted, panel.Viewport().Pixels()) * 5 < DifferingPixels(at_origin, lifted));
    // Taking it out from under the holder removes its Parent in the running game: back at the origin.
    AETHER_CHECK(document.ReparentEntity(boulder, kNullEntity));
    AETHER_CHECK(patched("unparenting"));
    settle();
    AETHER_CHECK(DifferingPixels(at_origin, panel.Viewport().Pixels()) * 5 < DifferingPixels(at_origin, lifted));

    // An entity with a character controller was set up when the game started, so moving it under another still loads the scene again.
    if (const Entity walker = MakeWalker(document, "Walker"); walker != kNullEntity) {
        AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads + 1);
        AETHER_CHECK(document.ReparentEntity(walker, holder));
        AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads + 2);
    }
}

AETHER_TEST(LiveViewportPanel_ARealDragFromARowIsPlacedOnTheGroundAndPatchedIntoTheGame) {
    std::unique_ptr<gfx::rhi::IDevice> device = TryDevice();
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    Context imgui;
    const stdfs::path project = MakeProject("LiveDrop");
    ScenePanel scene(project.parent_path() / "Content");
    AETHER_CHECK(scene.OpenScene("Scenes/Main.ascene", true));
    LiveViewportPanel panel(*device, scene, [&] { return project; });
    panel.width = 160;
    panel.height = 90;
    panel.reload_interval = 0.0f;
    AETHER_CHECK(panel.Start());
    SceneDocument& document = scene.Document();
    for (int i = 0; i < 4; ++i) panel.Advance(0.2f);
    std::string model;
    for (const Entity entity : document.Entities()) {
        const ModelRenderer* renderer = document.GetWorld().GetComponent<ModelRenderer>(entity);
        if (renderer && model.empty()) model = renderer->asset_path;
    }
    AETHER_CHECK(!model.empty());
    if (model.empty()) return;
    document.Commands().Clear();

    // The panel is drawn in a window below a row that drags `model`, through real mouse events, a frame at a time.
    auto step = [&](const ImVec2& mouse, bool down) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddMouseButtonEvent(0, down);
        imgui.Frame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1400, 900));
        ImGui::Begin("live", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);
        ImGui::SetCursorScreenPos(ImVec2(10, 10));
        ImGui::Button("row", ImVec2(120, 24));
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            ImGui::SetDragDropPayload(kContentPathPayload, model.c_str(), model.size() + 1);
            ImGui::TextUnformatted(model.c_str());
            ImGui::EndDragDropSource();
        }
        panel.Draw(1.0f / 30.0f);
        ImGui::End();
    };
    const ImVec2 source(20, 20);
    step(ImVec2(0, 0), false);
    step(source, false);
    step(source, true);
    step(ImVec2(source.x + 30.0f, source.y), true);
    // Carry the mouse down the window until the picture is a place to drop: its rectangle is where the drop lands.
    ImRect view;
    bool found = false;
    for (f32 y = 80.0f; y < 880.0f && !found; y += 10.0f) {
        step(ImVec2(700.0f, y), true);
        const ImGuiContext& g = *ImGui::GetCurrentContext();
        if (g.DragDropTargetId != 0 && g.DragDropTargetRect.GetWidth() > 200.0f) {
            view = g.DragDropTargetRect;
            found = true;
        }
    }
    AETHER_CHECK(found);
    if (!found) return;
    // Let go over where a known point of the ground is in the picture (found with the view's own camera and the rectangle just measured), so the right answer
    // is known without asking the code under test.
    const Vec3 ground_point(2.0f, 0.0f, 3.0f);
    const ImVec2 view_size(view.GetWidth(), view.GetHeight());
    ImVec2 release(-1, -1);
    AETHER_CHECK(ProjectToViewport(panel.Camera().ViewProjection(view_size.x / view_size.y), ground_point, view.Min, view_size, release));
    AETHER_CHECK(release.x > view.Min.x && release.x < view.Max.x && release.y > view.Min.y && release.y < view.Max.y);
    const usize before = document.Entities().size();
    const usize in_game = panel.Viewport().EntityCount();
    const u64 reloads = panel.Viewport().SceneReloads();
    const u64 patches = panel.Viewport().PatchCount();
    step(release, true);  // over the picture, still holding
    AETHER_CHECK(document.Entities().size() == before);                                       // nothing is placed until the mouse is let go
    step(release, false);
    AETHER_CHECK(document.Entities().size() == before + 1);
    const Entity placed = document.Selection().empty() ? kNullEntity : document.Selection()[0];
    AETHER_CHECK(placed != kNullEntity);
    if (placed == kNullEntity) return;
    // It is on the ground where the mouse let go, not at the camera's target.
    const Transform* at = document.GetWorld().GetComponent<Transform>(placed);
    AETHER_CHECK(at != nullptr && (at->position - ground_point).Length() < 0.1f);
    AETHER_CHECK(at != nullptr && (at->position - panel.Camera().target).Length() > 0.5f);
    // And it reaches the running game as a patch, no reload.
    for (int i = 0; i < 3; ++i) step(release, false);
    AETHER_CHECK(panel.Viewport().SceneReloads() == reloads && panel.Viewport().PatchCount() == patches + 1 && panel.Viewport().EntityCount() == in_game + 1);
    // One undo takes it out again, and the game follows that too.
    AETHER_CHECK(document.Undo() && document.Entities().size() == before);
    for (int i = 0; i < 3; ++i) step(release, false);
    AETHER_CHECK(panel.Viewport().SceneReloads() == reloads && panel.Viewport().EntityCount() == in_game);
}

AETHER_TEST(LiveViewportPanel_PhysicsBodiesAreAddedChangedAndRemovedWithoutReloading) {
    std::unique_ptr<gfx::rhi::IDevice> device = TryDevice();
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    Context imgui;
    const stdfs::path project = MakeProject("LivePhysics");
    ScenePanel scene(project.parent_path() / "Content");
    AETHER_CHECK(scene.OpenScene("Scenes/Main.ascene", true));
    LiveViewportPanel panel(*device, scene, [&] { return project; });
    panel.width = 160;
    panel.height = 90;
    panel.reload_interval = 0.0f;
    panel.run_game = true; // the physics scene follows the world when the game steps
    const ComponentId box = FindComponentIdByName("BoxCollider"), body = FindComponentIdByName("RigidBody"), character = FindComponentIdByName("CharacterMovement");
    if (box == kInvalidComponentId || body == kInvalidComponentId || character == kInvalidComponentId) {
        std::printf("    (no physics in this build)\n");
        return;
    }
    AETHER_CHECK(panel.Start());
    SceneDocument& document = scene.Document();
    for (int i = 0; i < 4; ++i) panel.Advance(0.2f);
    const usize bodies = panel.Viewport().PhysicsBodyCount();
    const u64 reloads = panel.Viewport().SceneReloads();
    AETHER_CHECK(bodies > 0);
    document.Commands().Clear();
    u64 patches = panel.Viewport().PatchCount();
    auto patched = [&](const char* what) {
        const bool ok = panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads && panel.Viewport().PatchCount() == patches + 1;
        if (!ok) std::fprintf(stderr, "    (not patched: %s)\n", what);
        patches = panel.Viewport().PatchCount();
        panel.Advance(0.2f); // a second step: the physics scene follows the world on the step after the change
        return ok;
    };

    // An entity with a collider: the game gets a body for it, and the scene was not loaded again.
    const Entity block = document.CreateEntity("Block");
    document.SetPosition(block, Vec3(0.0f, 5.0f, 0.0f), true);
    AETHER_CHECK(document.AddComponent(block, box));
    AETHER_CHECK(patched("a collider on a new entity"));
    AETHER_CHECK(panel.Viewport().PhysicsBodyCount() == bodies + 1);
    // A rigid body on it rebuilds that body (still one), a changed collider rebuilds it again: patches, and no extra bodies.
    AETHER_CHECK(document.AddComponent(block, body));
    AETHER_CHECK(patched("a rigid body added"));
    AETHER_CHECK(panel.Viewport().PhysicsBodyCount() == bodies + 1);
    AETHER_CHECK(document.SetComponentField(block, box, "is_trigger", reflect::Any(true), true));
    AETHER_CHECK(patched("a collider setting"));
    AETHER_CHECK(panel.Viewport().PhysicsBodyCount() == bodies + 1);
    // Undo and redo of that follow, and taking the entity away takes its body away.
    AETHER_CHECK(document.Undo());
    AETHER_CHECK(patched("undo of the setting"));
    AETHER_CHECK(document.Undo());
    AETHER_CHECK(patched("undo of the rigid body"));
    AETHER_CHECK(panel.Viewport().PhysicsBodyCount() == bodies + 1);
    AETHER_CHECK(document.Redo() && document.Redo());
    AETHER_CHECK(patched("redo"));
    AETHER_CHECK(document.DestroyEntity(block));
    AETHER_CHECK(patched("the entity gone"));
    AETHER_CHECK(panel.Viewport().PhysicsBodyCount() == bodies);
    AETHER_CHECK(document.Undo());
    AETHER_CHECK(patched("undo of the delete"));
    AETHER_CHECK(panel.Viewport().PhysicsBodyCount() == bodies + 1);

    // A character is not a body the physics scene follows: it sets up its own controller when the game starts, so that loads the scene again.
    Entity again = kNullEntity; // the undo of the delete made it again, under a new handle
    for (const Entity entity : document.Entities()) {
        if (document.Name(entity) == "Block") again = entity;
    }
    AETHER_CHECK(again != kNullEntity && document.AddComponent(again, character));
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads + 1);
}

AETHER_TEST(LiveViewportPanel_AnEditDoesNotPutAFallingBodyBackWhereTheDocumentHasIt) {
    std::unique_ptr<gfx::rhi::IDevice> device = TryDevice();
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    Context imgui;
    const stdfs::path project = MakeProject("LiveFalling");
    ScenePanel scene(project.parent_path() / "Content");
    AETHER_CHECK(scene.OpenScene("Scenes/Main.ascene", true));
    LiveViewportPanel panel(*device, scene, [&] { return project; });
    panel.width = 160;
    panel.height = 90;
    panel.reload_interval = 0.0f;
    panel.run_game = true; // physics steps, so a dynamic body falls
    const ComponentId box = FindComponentIdByName("BoxCollider"), body = FindComponentIdByName("RigidBody");
    if (box == kInvalidComponentId || body == kInvalidComponentId) {
        std::printf("    (no physics in this build)\n");
        return;
    }
    AETHER_CHECK(panel.Start());
    SceneDocument& document = scene.Document();
    for (int i = 0; i < 3; ++i) panel.Advance(0.2f);
    const u64 reloads = panel.Viewport().SceneReloads();
    document.Commands().Clear();

    // A dynamic body high above the scene, put in live, and let fall for a second or so of game time.
    const Entity faller = document.CreateEntity("Faller");
    document.SetPosition(faller, Vec3(0.0f, 30.0f, 0.0f), true);
    AETHER_CHECK(document.AddComponent(faller, box));
    AETHER_CHECK(document.AddComponent(faller, body));
    const EntityGuid guid = document.GetWorld().GetComponent<IdComponent>(faller)->guid;
    for (int i = 0; i < 12; ++i) panel.Advance(0.2f);
    Vec3 fallen;
    AETHER_CHECK(panel.Viewport().PositionOf(guid, fallen));
    AETHER_CHECK(fallen.y < 28.0f && fallen.y > 5.0f);                                            // it has fallen (the document still says 30), and not landed
    // Something unrelated is edited: the body keeps falling from where it is, and is not put back at 30.
    const Entity marker = document.CreateEntity("Marker");
    document.SetPosition(marker, Vec3(5.0f, 1.0f, 5.0f), true);
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads);
    Vec3 later;
    AETHER_CHECK(panel.Viewport().PositionOf(guid, later));
    AETHER_CHECK(later.y < fallen.y + 0.01f && later.y > fallen.y - 3.0f);
    const f32 above_ground = later.y;
    // The body's own Transform edited in the document is a change like any other: that does reach the game.
    document.SetPosition(faller, Vec3(0.0f, 60.0f, 0.0f), true);
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads);
    AETHER_CHECK(panel.Viewport().PositionOf(guid, later));
    AETHER_CHECK(later.y > above_ground + 20.0f);
}

AETHER_TEST(LiveViewportPanel_AScriptIsMadeRestartedAndEndedWithoutReloadingTheScene) {
    std::unique_ptr<gfx::rhi::IDevice> device = TryDevice();
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    if (!player::Game::HasScripting()) {
        std::printf("    (no scripting in this build)\n");
        return;
    }
    Context imgui;
    const stdfs::path project = MakeProject("LiveScript");
    ScenePanel scene(project.parent_path() / "Content");
    AETHER_CHECK(scene.OpenScene("Scenes/Main.ascene", true));
    LiveViewportPanel panel(*device, scene, [&] { return project; });
    panel.width = 160;
    panel.height = 90;
    panel.reload_interval = 0.0f;
    AETHER_CHECK(panel.Start());
    SceneDocument& document = scene.Document();
    auto settle = [&] { for (int i = 0; i < 4; ++i) panel.Advance(0.2f); };
    settle();
    const ComponentId script = GetComponentId<ScriptComponent>();
    assets::AssetRef<assets::ScriptAsset> class_ref;
    for (const Entity entity : document.Entities()) {
        if (const ScriptComponent* component = document.GetWorld().GetComponent<ScriptComponent>(entity)) class_ref = component->script;
    }
    AETHER_CHECK(class_ref.IsSet());
    if (!class_ref.IsSet()) return;
    const u64 reloads = panel.Viewport().SceneReloads();
    const usize instances = panel.Viewport().ScriptInstanceCount();
    AETHER_CHECK(instances >= 1);
    document.Commands().Clear();

    // A new object with a script: made by the game's lifecycle, with no reload.
    const Entity scripted = document.CreateEntity("Scripted");
    AETHER_CHECK(document.AddComponent(scripted, script));
    AETHER_CHECK(document.SetComponentField(scripted, script, "script", reflect::Any(class_ref), true));
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads);
    settle();
    AETHER_CHECK(panel.Viewport().SceneReloads() == reloads && panel.Viewport().ScriptInstanceCount() == instances + 1);

    // Editing its settings restarts that script alone: still one instance of it, still no reload.
    const u64 restarts = panel.Viewport().ScriptRestarts();
    ScriptProperty speed;
    speed.name = "speed";
    speed.value = "9";
    AETHER_CHECK(document.SetComponentField(scripted, script, "properties", reflect::Any(std::vector<ScriptProperty>{speed}), true));
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads && panel.Viewport().ScriptRestarts() == restarts + 1);
    settle();
    AETHER_CHECK(panel.Viewport().SceneReloads() == reloads && panel.Viewport().ScriptInstanceCount() == instances + 1);

    // Taking the object away ends its script, and undo brings both back.
    AETHER_CHECK(document.DestroyEntity(scripted));
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads);
    settle();
    AETHER_CHECK(panel.Viewport().SceneReloads() == reloads && panel.Viewport().ScriptInstanceCount() == instances);
    AETHER_CHECK(document.Undo());
    AETHER_CHECK(panel.Advance(0.2f) && panel.Viewport().SceneReloads() == reloads);
    settle();
    AETHER_CHECK(panel.Viewport().SceneReloads() == reloads && panel.Viewport().ScriptInstanceCount() == instances + 1);
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
