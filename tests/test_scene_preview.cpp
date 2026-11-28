#include "core/scene_preview.h"
#include "ui/image_texture.h"
#include "workspace/editor_workspace.h"
#if defined(AETHER_HAS_EDITOR_VULKAN)
#include "host/live_game_view.h"
#include "gameplay/gameplay_debugger_document.h"
#include "host/live_viewport_panel.h"
#include "host/scene_shading.h"
#endif
#include "world/scene_view_panel.h"
#include "content/content_drag.h"
#include "world/content_drop.h"
#include "world/scene_preview_panel.h"

#include "aether/core/log.h"
#include "aether/scene/components.h"
#include "aether/assets/png_writer.h"
#include "aether/assets/asset_database.h"
#include "aether/renderer/components.h"
#include "aether/renderer/material_instance.h"
#include "aether/renderer/material.h"
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

// The templates' controller scripts steer the MainCamera around the player while the game runs; a check that reads the camera's pose wants
// the camera as the document left it. Destroying the scripted entities leaves the rest of the scene standing.
usize StripScripts(SceneDocument& document) {
    std::vector<Entity> scripted;
    for (const Entity entity : document.Entities()) {
        if (document.GetWorld().HasComponent<ScriptComponent>(entity)) scripted.push_back(entity);
    }
    for (const Entity entity : scripted) AETHER_CHECK(document.DestroyEntity(entity));
    return scripted.size();
}

// TAA jitters the projection differently every frame, so two renders of an identical scene still differ by a fraction of a percent of their
// pixels. A check that compares frames for a standstill sets AETHER_RENDER_TAA=0; the previous value goes back when this leaves scope.
class ScopedTaaOff {
public:
    ScopedTaaOff() {
        if (const char* prior = std::getenv("AETHER_RENDER_TAA")) {
            had_prior_ = true;
            prior_ = prior;
        }
#ifdef _WIN32
        _putenv_s("AETHER_RENDER_TAA", "0");
#else
        setenv("AETHER_RENDER_TAA", "0", 1);
#endif
    }
    ~ScopedTaaOff() {
#ifdef _WIN32
        _putenv_s("AETHER_RENDER_TAA", had_prior_ ? prior_.c_str() : "");
#else
        if (had_prior_) setenv("AETHER_RENDER_TAA", prior_.c_str(), 1);
        else unsetenv("AETHER_RENDER_TAA");
#endif
    }
    ScopedTaaOff(const ScopedTaaOff&) = delete;
    ScopedTaaOff& operator=(const ScopedTaaOff&) = delete;
private:
    bool had_prior_ = false;
    std::string prior_;
};

} // namespace

#if defined(AETHER_HAS_WINDOW) && (defined(_WIN32) || defined(AETHER_HAS_VULKAN))
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

#endif

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

#if defined(AETHER_HAS_WINDOW) && (defined(_WIN32) || defined(AETHER_HAS_VULKAN))
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

#endif

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

AETHER_TEST(LiveViewport_UnlitNormalsAndOrthographicViewsDrawDifferently) {
    std::unique_ptr<gfx::rhi::IDevice> device = TryDevice();
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    const stdfs::path project = MakeProject("ViewModes");
    stdfs::path cooked;
    std::string error;
    AETHER_CHECK(CookForPreview(project, "Scenes/Main.ascene", project.parent_path() / "Saved" / "ViewModeTest", &cooked, &error));
    LiveViewport view(*device);
    constexpr u32 kWidth = 160, kHeight = 90;
    AETHER_CHECK(view.Load(cooked, "Scenes/Main.ascene", kWidth, kHeight, &error));
    SceneViewCamera camera;
    auto draw = [&](player::SceneRenderer::ViewMode mode, SceneViewProjection projection) {
        camera.projection = projection;
        view.SetViewMode(mode);
        for (int frame = 0; frame < 3; ++frame) {
            AETHER_CHECK(view.Frame(0.0f, camera.Eye(), camera.Rotation(), camera.fov_degrees,
                                    projection == SceneViewProjection::Orthographic ? Projection::Orthographic : Projection::Perspective, camera.ortho_height));
        }
        return std::vector<u8>(view.Pixels());
    };
    const std::vector<u8> lit = draw(player::SceneRenderer::ViewMode::Lit, SceneViewProjection::Perspective);
    const std::vector<u8> unlit = draw(player::SceneRenderer::ViewMode::Unlit, SceneViewProjection::Perspective);
    const std::vector<u8> normals = draw(player::SceneRenderer::ViewMode::Normals, SceneViewProjection::Perspective);
    const std::vector<u8> ortho = draw(player::SceneRenderer::ViewMode::Lit, SceneViewProjection::Orthographic);
    // Each look is a different picture: no light on the surfaces, the normals as colours, and the same lit scene in parallel projection.
    AETHER_CHECK(DifferingPixels(lit, unlit) > kWidth * kHeight / 10);
    AETHER_CHECK(DifferingPixels(lit, normals) > kWidth * kHeight / 10);
    AETHER_CHECK(DifferingPixels(unlit, normals) > kWidth * kHeight / 10);
    AETHER_CHECK(DifferingPixels(lit, ortho) > kWidth * kHeight / 10);
}

// The gate's headless screenshots: each look and an orthographic view, rendered with no window and written as a PNG a person can open, with each
// checked to be a real picture rather than a flat or empty frame and not the same picture as another. The files go under the test project's
// Saved/Screenshots, which the run removes with its scratch dir; set AETHER_PARITY_OUT (as the lit-parity captures do) to keep them.
AETHER_TEST(LiveViewport_HeadlessScreenshotsOfEachViewModeAndAnOrthographicView) {
    std::unique_ptr<gfx::rhi::IDevice> device = TryDevice();
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    const stdfs::path project = MakeProject("Screenshots");
    stdfs::path cooked;
    std::string error;
    AETHER_CHECK(CookForPreview(project, "Scenes/Main.ascene", project.parent_path() / "Saved" / "ScreenshotTest", &cooked, &error));
    LiveViewport view(*device);
    constexpr u32 kWidth = 320, kHeight = 180;
    AETHER_CHECK(view.Load(cooked, "Scenes/Main.ascene", kWidth, kHeight, &error));
    const char* keep = std::getenv("AETHER_PARITY_OUT");
    const stdfs::path out = (keep && *keep) ? stdfs::path(keep) : project.parent_path() / "Saved" / "Screenshots";
    stdfs::create_directories(out);
    SceneViewCamera camera;
    auto shot = [&](const char* name, player::SceneRenderer::ViewMode mode, SceneViewProjection projection) {
        camera.projection = projection;
        view.SetViewMode(mode);
        for (int frame = 0; frame < 3; ++frame) {
            AETHER_CHECK(view.Frame(0.0f, camera.Eye(), camera.Rotation(), camera.fov_degrees,
                                    projection == SceneViewProjection::Orthographic ? Projection::Orthographic : Projection::Perspective, camera.ortho_height));
        }
        const std::vector<u8>& rgba = view.Pixels();
        std::vector<u8> rgb;
        rgb.reserve(static_cast<std::size_t>(kWidth) * kHeight * 3);
        for (usize i = 0; i + 3 < rgba.size(); i += 4) {
            rgb.push_back(rgba[i]);
            rgb.push_back(rgba[i + 1]);
            rgb.push_back(rgba[i + 2]);
        }
        const std::vector<u8> png = assets::EncodePngRgb8(kWidth, kHeight, rgb);
        std::ofstream(out / name, std::ios::binary).write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        std::set<u32> colours;
        for (usize i = 0; i + 3 < rgba.size(); i += 4 * 31) colours.insert(u32(rgba[i]) << 16 | u32(rgba[i + 1]) << 8 | rgba[i + 2]);
        AETHER_CHECK(png.size() > 2000 && png[0] == 0x89 && png[1] == 'P'); // a real PNG of a scene, not a flat fill
        AETHER_CHECK(colours.size() > 20);
        return rgba;
    };
    const std::vector<u8> lit = shot("lit.png", player::SceneRenderer::ViewMode::Lit, SceneViewProjection::Perspective);
    const std::vector<u8> unlit = shot("unlit.png", player::SceneRenderer::ViewMode::Unlit, SceneViewProjection::Perspective);
    const std::vector<u8> normals = shot("normals.png", player::SceneRenderer::ViewMode::Normals, SceneViewProjection::Perspective);
    camera.Look(SceneViewOrientation::Top);
    camera.ortho_height = 24.0f;
    const std::vector<u8> ortho = shot("ortho_top.png", player::SceneRenderer::ViewMode::Lit, SceneViewProjection::Orthographic);
    AETHER_CHECK(DifferingPixels(lit, unlit) > kWidth * kHeight / 10);
    AETHER_CHECK(DifferingPixels(lit, normals) > kWidth * kHeight / 10);
    AETHER_CHECK(DifferingPixels(unlit, normals) > kWidth * kHeight / 10);
    AETHER_CHECK(DifferingPixels(lit, ortho) > kWidth * kHeight / 10);
    for (const char* name : {"lit.png", "unlit.png", "normals.png", "ortho_top.png"}) AETHER_CHECK(stdfs::file_size(out / name) > 2000);
    std::printf("    screenshots: %s\n", out.string().c_str());
}

// A scene with no camera is drawn anyway: the viewport puts the editor's own camera in it, as the Scene View has one of its own.
AETHER_TEST(LiveViewport_ASceneWithNoCameraGetsTheEditorCamera) {
    std::unique_ptr<gfx::rhi::IDevice> device = TryDevice();
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    const stdfs::path project = MakeProject("NoCamera");
    ScenePanel scene(project.parent_path() / "Content");
    AETHER_CHECK(scene.OpenScene("Scenes/Main.ascene", true));
    SceneDocument& document = scene.Document();
    std::vector<Entity> cameras;
    for (const Entity entity : document.Entities()) {
        if (document.GetWorld().HasComponent<Camera>(entity)) cameras.push_back(entity);
    }
    for (const Entity entity : cameras) AETHER_CHECK(document.RemoveComponent(entity, GetComponentId<Camera>()));
    stdfs::path cooked;
    std::string error;
    AETHER_CHECK(CookForPreview(project, "Scenes/Main.ascene", project.parent_path() / "Saved" / "NoCameraTest", &cooked, &error));
    LiveViewport view(*device);
    const std::vector<u8> bytes = SaveSceneToMemory(document.GetWorld());
    AETHER_CHECK(view.Load(cooked, "Scenes/Main.ascene", 96, 54, &error, &bytes));
    const usize entities = view.EntityCount();
    SceneViewCamera camera;
    AETHER_CHECK(view.Frame(0.0f, camera.Eye(), camera.Rotation(), camera.fov_degrees));
    AETHER_CHECK(view.EntityCount() == entities + 1); // the camera the viewport made, once
    AETHER_CHECK(view.Frame(0.0f, camera.Eye(), camera.Rotation(), camera.fov_degrees));
    AETHER_CHECK(view.EntityCount() == entities + 1);
    std::set<u32> colours;
    for (usize i = 0; i + 3 < view.Pixels().size(); i += 4 * 7) colours.insert(u32(view.Pixels()[i]) << 16 | u32(view.Pixels()[i + 1]) << 8 | view.Pixels()[i + 2]);
    AETHER_CHECK(colours.size() > 5);
}

// Simulate's camera contract, below the editor: a free-camera frame borrows the game's camera for the picture and puts it back, so the
// game keeps simulating from its own camera; an ordinary frame does take it, as Play does.
AETHER_TEST(LiveViewport_FreeCameraPutsTheGamesCameraBack) {
    std::unique_ptr<gfx::rhi::IDevice> device = TryDevice();
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    const stdfs::path project = MakeProject("FreeCamera", "blank");
    ScenePanel scene(project.parent_path() / "Content");
    AETHER_CHECK(scene.OpenScene("Scenes/Main.ascene", true));
    SceneDocument& document = scene.Document();
    const Entity camera_entity = FindActiveCamera(document.GetWorld(), document.Guids());
    AETHER_CHECK(!camera_entity.IsNull());
    if (camera_entity.IsNull()) return;
    const EntityGuid camera_guid = document.GetWorld().GetComponent<IdComponent>(camera_entity)->guid;
    const Vec3 authored = document.GetWorld().GetComponent<Transform>(camera_entity)->position;
    stdfs::path cooked;
    std::string error;
    AETHER_CHECK(CookForPreview(project, "Scenes/Main.ascene", project.parent_path() / "Saved" / "FreeCameraTest", &cooked, &error));
    const std::vector<u8> bytes = SaveSceneToMemory(document.GetWorld());

    LiveViewport view(*device);
    AETHER_CHECK(view.Load(cooked, "Scenes/Main.ascene", 96, 54, &error, &bytes));
    SceneViewCamera editor;
    editor.target = Vec3(3.0f, 1.0f, -2.0f);
    editor.distance = 9.0f;
    const Vec3 eye = editor.Eye();
    AETHER_CHECK((eye - authored).Length() > 1.0f); // the two cameras differ, or the checks below prove nothing

    AETHER_CHECK(view.Frame(0.0f, eye, editor.Rotation(), editor.fov_degrees)); // Play: the game's camera becomes the view's
    Vec3 game_camera;
    AETHER_CHECK(view.PositionOf(camera_guid, game_camera) && (game_camera - eye).Length() < 1e-3f);

    AETHER_CHECK(view.SetScene(bytes, &error)); // a fresh game: the camera is back where the scene puts it
    AETHER_CHECK(view.PositionOf(camera_guid, game_camera) && (game_camera - authored).Length() < 1e-3f);
    AETHER_CHECK(view.Frame(0.0f, eye, editor.Rotation(), editor.fov_degrees, Projection::Perspective, 0.0f, true)); // Simulate
    AETHER_CHECK(view.PositionOf(camera_guid, game_camera) && (game_camera - authored).Length() < 1e-3f); // the borrowed pose did not stick
    AETHER_CHECK(view.Frame(0.0f, eye, editor.Rotation(), editor.fov_degrees, Projection::Perspective, 0.0f, true));
    AETHER_CHECK(view.PositionOf(camera_guid, game_camera) && (game_camera - authored).Length() < 1e-3f); // nor the second time
}

// The Game view's frame, below the editor: the scene is drawn from its own camera, exactly as the game has it, and nothing moves that
// camera -- there is no editor pose in the call at all. A normal frame at an editor eye is a different picture.
AETHER_TEST(LiveViewport_FrameGameCameraDrawsFromTheScenesOwnCamera) {
    std::unique_ptr<gfx::rhi::IDevice> device = TryDevice();
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    const stdfs::path project = MakeProject("GameCamera");
    ScenePanel scene(project.parent_path() / "Content");
    AETHER_CHECK(scene.OpenScene("Scenes/Main.ascene", true));
    SceneDocument& document = scene.Document();
    AETHER_CHECK(StripScripts(document) > 0); // the template's controller follows the player with the camera; these checks want it still
    const Entity camera_entity = FindActiveCamera(document.GetWorld(), document.Guids());
    AETHER_CHECK(!camera_entity.IsNull());
    if (camera_entity.IsNull()) return;
    const EntityGuid camera_guid = document.GetWorld().GetComponent<IdComponent>(camera_entity)->guid;
    const Vec3 authored = document.GetWorld().GetComponent<Transform>(camera_entity)->position;
    stdfs::path cooked;
    std::string error;
    AETHER_CHECK(CookForPreview(project, "Scenes/Main.ascene", project.parent_path() / "Saved" / "GameCameraTest", &cooked, &error));
    const std::vector<u8> bytes = SaveSceneToMemory(document.GetWorld());
    LiveViewport view(*device);
    AETHER_CHECK(view.Load(cooked, "Scenes/Main.ascene", 96, 54, &error, &bytes));

    AETHER_CHECK(view.FrameGameCamera(0.0f));
    std::set<u32> colours;
    for (usize i = 0; i + 3 < view.Pixels().size(); i += 4 * 7) colours.insert(u32(view.Pixels()[i]) << 16 | u32(view.Pixels()[i + 1]) << 8 | u32(view.Pixels()[i + 2]));
    AETHER_CHECK(colours.size() > 5); // a scene, not a flat fill
    const std::vector<u8> game_picture = view.Pixels();
    Vec3 game_camera;
    AETHER_CHECK(view.PositionOf(camera_guid, game_camera) && (game_camera - authored).Length() < 1e-3f); // the game's own camera, untouched

    SceneViewCamera editor;
    editor.target = Vec3(3.0f, 1.0f, -2.0f);
    editor.distance = 9.0f;
    AETHER_CHECK(view.Frame(0.0f, editor.Eye(), editor.Rotation(), editor.fov_degrees));
    AETHER_CHECK(DifferingPixels(game_picture, view.Pixels()) > 96u * 54u / 10); // the editor's view is not the game's
}

// The Game View tool end to end: Play starts the game's own camera picture, Pause holds it, Step advances exactly one frame, Stop lets
// the game go, and the next session shows the document as it is then.
AETHER_TEST(GameView_PlaysTheSessionFromItsOwnCamera) {
    std::unique_ptr<gfx::rhi::IDevice> device = TryDevice();
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    Context imgui;
    const stdfs::path project = MakeProject("GameViewPlay");
    ScenePanel scene(project.parent_path() / "Content");
    AETHER_CHECK(scene.OpenScene("Scenes/Main.ascene", true));
    SceneDocument& document = scene.Document();
    AETHER_CHECK(StripScripts(document) > 0); // the controller would move the camera; this check poses it
    const Entity camera_entity = FindActiveCamera(document.GetWorld(), document.Guids());
    AETHER_CHECK(!camera_entity.IsNull());
    if (camera_entity.IsNull()) return;
    // High above the floor, looking down: a picture no editor orbit view would give.
    AETHER_CHECK(document.SetPosition(camera_entity, Vec3(0.0f, 12.0f, 0.001f), true));
    AETHER_CHECK(document.SetRotation(camera_entity, Quaternion::FromAxisAngle(Vec3(1.0f, 0.0f, 0.0f), -1.5707963f), true));
    const EntityGuid camera_guid = document.GetWorld().GetComponent<IdComponent>(camera_entity)->guid;

    LiveGameView live(*device, scene, [&] { return project; });
    GameViewPanel panel;
    panel.surface = &live;
    panel.width = 160;
    panel.height = 90;
    auto draw = [&] {
        imgui.Frame();
        ImGui::Begin("game");
        panel.Draw(document, 1.0f / 60.0f);
        ImGui::End();
    };
    AETHER_CHECK(!live.Viewport().Loaded());
    draw(); // editing: nothing is cooked or run
    AETHER_CHECK(!live.Viewport().Loaded() && !panel.LastState().playing);

    document.Play();
    draw();
    AETHER_CHECK(panel.LastState().playing && panel.LastState().has_picture && panel.Running());
    AETHER_CHECK(live.Viewport().Loaded() && live.Picture().Width() == 160 && live.Picture().Height() == 90);
    Vec3 game_camera;
    AETHER_CHECK(live.Viewport().PositionOf(camera_guid, game_camera) && (game_camera - Vec3(0.0f, 12.0f, 0.0f)).Length() < 0.5f);
    const std::vector<u8> top_down = live.Viewport().Pixels();

    document.Pause();
    draw();
    AETHER_CHECK(panel.LastState().paused && !panel.LastState().simulating);
    AETHER_CHECK(DifferingPixels(top_down, live.Viewport().Pixels()) == 0); // paused: the picture stands
    const u64 frames = live.Viewport().FrameCount();
    document.Step();
    draw();
    AETHER_CHECK(live.Viewport().FrameCount() == frames + 1); // Step: exactly one frame of the game
    document.Play(); // Paused -> Playing again, for Stop
    document.Stop(); // Game View is hidden and does not observe the Editing frame.

    // The next session has a different camera, but is paused before the Game View becomes visible.
    const Entity camera_now = document.Guids().Find(document.GetWorld(), camera_guid); // handles do not survive Stop
    AETHER_CHECK(!camera_now.IsNull());
    if (camera_now.IsNull()) return;
    AETHER_CHECK(document.SetPosition(camera_now, Vec3(0.0f, 2.0f, 7.0f), true));
    AETHER_CHECK(document.SetRotation(camera_now, Quaternion::FromAxisAngle(Vec3(1.0f, 0.0f, 0.0f), -0.25f), true));
    document.Play();
    document.Pause();
    draw();
    AETHER_CHECK(panel.Running() && panel.LastState().paused && panel.LastState().has_picture);
    AETHER_CHECK(live.Viewport().FrameCount() == 1); // a paused new session still needs its own first picture
    Vec3 game_camera2;
    const bool found2 = live.Viewport().PositionOf(camera_guid, game_camera2);
    const usize differing = DifferingPixels(top_down, live.Viewport().Pixels());
    AETHER_CHECK(found2 && (game_camera2 - Vec3(0.0f, 2.0f, 7.0f)).Length() < 0.5f); // the session runs the document's camera
    AETHER_CHECK(differing > 160u * 90u / 20); // a different game camera, a different picture
    draw();
    AETHER_CHECK(live.Viewport().FrameCount() == 1 && panel.LastState().has_picture); // keep this session's paused frame

    document.Stop();
    draw();
    AETHER_CHECK(!panel.Running() && !live.Viewport().Loaded() && panel.LastState().note.find("not running") != std::string::npos);
    AETHER_CHECK(!live.Picture().Valid());

    // Force a real first-frame upload failure without changing the Vulkan device.
    // There is still an ImageTexture allocation from earlier sessions, but no
    // current ImGui context to accept the new pixels.
    AETHER_CHECK(live.Start(document, 160, 90));
    AETHER_CHECK(!live.Picture().Valid());
    ImGuiContext* context = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(nullptr);
    const bool failed = !live.Frame(0.0f);
    ImGui::SetCurrentContext(context);
    AETHER_CHECK(failed && live.Viewport().FrameCount() == 1 && !live.Picture().Valid());
    AETHER_CHECK(live.Frame(0.0f) && live.Viewport().FrameCount() == 2 && live.Picture().Valid());
    AETHER_CHECK(!live.Frame(0.0f) && live.Viewport().FrameCount() == 2 && live.Picture().Valid());
}

AETHER_TEST(GameView_LivePlayRunsOnStartInThePreviewWorld) {
    std::unique_ptr<gfx::rhi::IDevice> device = TryDevice();
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    if (!player::Game::HasScripting()) {
        std::printf("    (no scripting in this build)\n");
        return;
    }
    const stdfs::path project = MakeProject("GameViewOnStart", "blank");
    const stdfs::path content = project.parent_path() / "Content";
    stdfs::create_directories(content / "Scripts");
    const char* script_path = "Scripts/PreviewStart.luau";
    std::ofstream(content / script_path) << R"(
local PreviewStart = {}
function PreviewStart:OnStart()
    local transform = self.entity:Get("Transform")
    transform.position = transform.position + vector.create(7, 0, 0)
end
return PreviewStart
)";
    assets::AssetDatabase database(content);
    database.Scan();
    const assets::AssetRecord* script = database.FindByPath(script_path);
    AETHER_CHECK(script != nullptr);
    if (!script) return;

    ScenePanel scene(content);
    if (!scene.OpenScene("Scenes/Main.ascene", true)) { AETHER_CHECK(false); return; }
    SceneDocument& document = scene.Document();
    const Entity actor = document.CreateEntity("Start probe");
    const EntityGuid guid = document.GetWorld().GetComponent<IdComponent>(actor)->guid;
    ScriptComponent component;
    component.script.guid = script->guid;
    document.GetWorld().AddComponent(actor, component);
    std::string error;
    if (!document.Save({}, &error)) { AETHER_CHECK(false); return; }

    LiveGameView live(*device, scene, [&] { return project; });
    for (int session = 0; session < 2; ++session) {
        document.Play();
        const bool started = live.Start(document, 128, 72);
        AETHER_CHECK(started);
        if (!started) return;
        Vec3 position;
        AETHER_CHECK(live.Viewport().PositionOf(guid, position));
        AETHER_CHECK(std::abs(position.x - 7.0f) < 1e-3f);
        AETHER_CHECK(live.Viewport().RuntimeErrors().empty());

        // The start callback ran in the preview copy, not in the author's scene.
        const Entity authored = document.Guids().Find(document.GetWorld(), guid);
        const Transform* transform = document.GetWorld().GetComponent<Transform>(authored);
        AETHER_CHECK(transform != nullptr && std::abs(transform->position.x) < 1e-3f);
        live.Stop();
        document.Stop();
    }
}

AETHER_TEST(GameView_LiveGameplaySourceTracksTheCurrentSession) {
    std::unique_ptr<gfx::rhi::IDevice> device = TryDevice();
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    const stdfs::path project = MakeProject("GameViewDebugger", "blank");
    const stdfs::path content = project.parent_path() / "Content";
    ScenePanel scene(content);
    if (!scene.OpenScene("Scenes/Main.ascene", true)) { AETHER_CHECK(false); return; }
    SceneDocument& document = scene.Document();
    const Entity actor = document.CreateEntity("Gameplay probe");
    gas::AttributeSet attrs;
    AETHER_CHECK(attrs.Define("LiveProbeCharge", 42));
    document.GetWorld().AddComponent(actor, attrs);

    LiveGameView live(*device, scene, [&] { return project; });
    AETHER_CHECK(live.GameplaySource(document).world == nullptr);
    document.Play();
    if (!live.Start(document, 128, 72)) { AETHER_CHECK(false); return; }

    // A second preview can be the process's active system, but it must not receive debugger edits.
    World unrelated;
    gas::AttributeSystem unrelated_attributes(unrelated);
    document.Pause();
    const auto current = live.GameplaySource(document);
    AETHER_CHECK(current.world != nullptr && current.world != &document.GetWorld());
    AETHER_CHECK(current.attributes != nullptr && current.attributes != &unrelated_attributes &&
                 current.effects != nullptr && current.abilities != nullptr && current.effect_library != nullptr &&
                 current.ability_library != nullptr);
    if (!current.world || !current.attributes) return;
    GameplayDebuggerDocument debugger;
    debugger.SetWorld(current.world);
    debugger.SetSystems(current.attributes, current.effects, current.abilities);
    debugger.Rebuild();
    AETHER_CHECK(debugger.Entities().size() == 1);
    if (debugger.Entities().size() != 1) return;
    const Entity running_actor = debugger.Entities().front().entity;
    AETHER_CHECK(current.attributes->GetBase(running_actor, "LiveProbeCharge") == 42);
    debugger.SetBase(running_actor, "LiveProbeCharge", 17);
    AETHER_CHECK(current.attributes->GetBase(running_actor, "LiveProbeCharge") == 17);
    AETHER_CHECK(!current.attributes->Events().empty() && current.attributes->Events().back().new_value == 17);
    AETHER_CHECK(document.GetWorld().GetComponent<gas::AttributeSet>(actor)->Get("LiveProbeCharge") == 42);
    debugger.SetWorld(nullptr);
    debugger.SetSystems(nullptr, nullptr, nullptr);

    document.Stop();
    AETHER_CHECK(live.GameplaySource(document).world == nullptr); // before hidden Game View has serviced teardown
    document.Play();
    AETHER_CHECK(live.GameplaySource(document).world == nullptr); // never attach to the prior preview
    live.Stop();
    AETHER_CHECK(live.GameplaySource(document).world == nullptr);
    document.Stop();
}

#if defined(AETHER_HAS_EDITOR_VULKAN)
// The Scene View's shaded look: the bridge cooks and runs the open scene the first time it is asked, draws at the Scene View's camera, and keeps what
// the document has with patches, as the Live Viewport does.
AETHER_TEST(LiveSceneShading_CooksOnFirstUseAndFollowsTheDocument) {
    std::unique_ptr<gfx::rhi::IDevice> device = TryDevice();
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    Context imgui;
    const stdfs::path project = MakeProject("ShadedSceneView");
    ScenePanel scene(project.parent_path() / "Content");
    AETHER_CHECK(scene.OpenScene("Scenes/Main.ascene", true));
    LiveSceneShading shading(*device, scene, [&] { return project; });
    AETHER_CHECK(!shading.Ready() && !shading.Picture().Valid());
    SceneViewCamera camera;
    AETHER_CHECK(shading.Advance(scene.Document(), camera, 160, 90, SceneViewMode::Lit, 1.0f));
    AETHER_CHECK(shading.Ready() && shading.Picture().Width() == 160 && shading.Picture().Height() == 90);
    AETHER_CHECK(shading.Viewport().FrameCount() >= 1);
    const std::vector<u8> lit = shading.Viewport().Pixels();
    AETHER_CHECK(shading.Advance(scene.Document(), camera, 160, 90, SceneViewMode::Unlit, 1.0f));
    AETHER_CHECK(DifferingPixels(lit, shading.Viewport().Pixels()) > 160u * 90u / 10);
    // A moved object is patched into the running scene: the next frame shows it where the document has it, without the scene starting over.
    const u64 reloads = shading.Viewport().SceneReloads();
    usize moved = 0;
    for (const Entity entity : scene.Document().Entities()) {
        if (!scene.Document().GetWorld().HasComponent<ModelRenderer>(entity)) continue;
        scene.Document().SetPosition(entity, Vec3(200.0f, 0.0f, 0.0f), true);
        ++moved;
    }
    AETHER_CHECK(moved > 0);
    AETHER_CHECK(shading.Advance(scene.Document(), camera, 160, 90, SceneViewMode::Lit, 1.0f));
    AETHER_CHECK(shading.Viewport().SceneReloads() == reloads && shading.Viewport().PatchCount() > 0);
    AETHER_CHECK(DifferingPixels(lit, shading.Viewport().Pixels()) > 160u * 90u / 10);
    shading.Stop();
    AETHER_CHECK(!shading.Ready());
}

// The Stage 5 gate's screenshots: the Scene View over a real play session -- idle (editing), playing and paused -- rendered with no
// window through the same SceneViewPanel the shell hosts, each written as a PNG a person can open. Playing is checked to be a
// different picture from idle (the game runs: the template's pickups spin) and paused to be the same picture as the frame it paused on.
// The files go under the test project's Saved/Screenshots; set AETHER_PARITY_OUT to keep them, as the other capture tests do.
AETHER_TEST(SceneView_PieScreenshotsOfIdlePlayingAndPaused) {
    std::unique_ptr<gfx::rhi::IDevice> device = TryDevice();
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    Context imgui;
    const stdfs::path project = MakeProject("PieScreenshots");
    ScopedTaaOff taa_off; // the paused captures are compared for a standstill; the jitter would be noise
    ScenePanel scene(project.parent_path() / "Content");
    AETHER_CHECK(scene.OpenScene("Scenes/Main.ascene", true));
    SceneDocument& document = scene.Document();
    LiveSceneShading shading(*device, scene, [&] { return project; });
    SceneViewPanel panel;
    panel.shading = &shading;
    panel.mode = SceneViewMode::Lit;
    auto draw = [&] {
        imgui.Frame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(480.0f, 320.0f));
        ImGui::Begin("pie", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar);
        panel.Draw(document);
        ImGui::End();
    };
    const char* keep = std::getenv("AETHER_PARITY_OUT");
    const stdfs::path out = (keep && *keep) ? stdfs::path(keep) : project.parent_path() / "Saved" / "Screenshots";
    stdfs::create_directories(out);
    // The shading throttles its draws, so the pixels right after an arbitrary draw can be the previous frame's. A shot follows one the
    // renderer really took: the game is ticked once more at most, and each picture is the state at the moment it is captured.
    auto fresh = [&] {
        const u64 frames = shading.Viewport().FrameCount();
        for (int attempt = 0; attempt < 8 && shading.Viewport().FrameCount() == frames; ++attempt) draw();
    };
    auto shot = [&](const char* name) {
        const ImageTexture& picture = shading.Picture();
        AETHER_CHECK(picture.Valid() && picture.Width() > 0 && picture.Height() > 0);
        if (!picture.Valid()) return std::vector<u8>{};
        const std::vector<u8>& rgba = shading.Viewport().Pixels();
        std::vector<u8> rgb;
        rgb.reserve(static_cast<std::size_t>(picture.Width()) * picture.Height() * 3);
        for (usize i = 0; i + 3 < rgba.size(); i += 4) {
            rgb.push_back(rgba[i]);
            rgb.push_back(rgba[i + 1]);
            rgb.push_back(rgba[i + 2]);
        }
        const std::vector<u8> png = assets::EncodePngRgb8(picture.Width(), picture.Height(), rgb);
        std::ofstream(out / name, std::ios::binary).write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        std::set<u32> colours;
        for (usize i = 0; i + 3 < rgba.size(); i += 4 * 31) colours.insert(u32(rgba[i]) << 16 | u32(rgba[i + 1]) << 8 | rgba[i + 2]);
        AETHER_CHECK(png.size() > 2000 && png[0] == 0x89 && png[1] == 'P'); // a real PNG of the scene, not a flat fill
        AETHER_CHECK(colours.size() > 20);
        return shading.Viewport().Pixels();
    };

    draw();
    draw();
    AETHER_CHECK(!panel.LastPie().active);
    fresh();
    const std::vector<u8> idle = shot("pie_idle.png");

    document.Play();
    for (int frame = 0; frame < 45; ++frame) draw(); // three quarters of a second: the pickups spin 67 degrees
    AETHER_CHECK(panel.LastPie().active && !panel.LastPie().paused);
    AETHER_CHECK(panel.LastPie().banner_text.find("Playing") == 0);
    fresh();
    const std::vector<u8> playing = shot("pie_playing.png");
    const usize motion = DifferingPixels(idle, playing);
    AETHER_CHECK(motion > 200); // the game runs in the picture: the pickups turned

    document.Pause();
    draw();
    fresh();
    AETHER_CHECK(panel.LastPie().paused);
    AETHER_CHECK(panel.LastPie().banner_text.find("Paused") == 0);
    const std::vector<u8> paused = shot("pie_paused.png");
    AETHER_CHECK(DifferingPixels(playing, paused) == 0); // the frozen picture is the one it paused on, not a restarted scene
    fresh();
    AETHER_CHECK(DifferingPixels(paused, shading.Viewport().Pixels()) == 0); // and it stands: nothing moves while paused

    document.Stop();
    draw();
    AETHER_CHECK(!panel.LastPie().active);
    AETHER_CHECK(shading.Picture().Valid()); // the editor's own view comes back
    std::printf("    screenshots: %s\n", out.string().c_str());
}

// The Stage 5 gate's determinism check: the same scene, started twice from the same cooked bytes and advanced the same N steps, is in the
// same state. The state is the world as a scene file (SnapshotScene), not the picture: the renderer's lighting settles on its own schedule,
// so early frames are legitimately darker, while the simulation must not depend on how long a frame took. A shorter run is checked to
// differ, so the equal comparison means the game really moved and landed in the same place both times.
AETHER_TEST(LiveViewport_TheSameStepsReachTheSameState) {
    std::unique_ptr<gfx::rhi::IDevice> device = TryDevice();
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    const stdfs::path project = MakeProject("StepDeterminism");
    stdfs::path cooked;
    std::string error;
    AETHER_CHECK(CookForPreview(project, "Scenes/Main.ascene", project.parent_path() / "Saved" / "StepDeterminism", &cooked, &error));
    constexpr u32 kWidth = 320, kHeight = 180;
    SceneViewCamera camera;
    camera.target = Vec3(0.0f, 1.0f, 0.0f);
    camera.distance = 9.0f;
    LiveViewport view(*device);
    auto run = [&](int steps) {
        AETHER_CHECK(view.Load(cooked, "Scenes/Main.ascene", kWidth, kHeight, &error)); // a fresh game from the same state
        for (int step = 0; step < steps; ++step) {
            AETHER_CHECK(view.Frame(1.0f / 60.0f, camera.Eye(), camera.Rotation(), camera.fov_degrees));
        }
        return view.SnapshotScene();
    };
    const std::vector<u8> ninety = run(90);
    const std::vector<u8> thirty = run(30);
    const std::vector<u8> ninety_again = run(90);
    AETHER_CHECK(!ninety.empty());
    auto differing = [](const std::vector<u8>& a, const std::vector<u8>& b) {
        usize count = a.size() > b.size() ? a.size() - b.size() : b.size() - a.size();
        for (usize i = 0; i < std::min(a.size(), b.size()); ++i) count += a[i] != b[i];
        return count;
    };
    std::printf("    the 30-step state differs in %zu bytes; the second 90-step run in %zu\n", differing(ninety, thirty), differing(ninety, ninety_again));
    AETHER_CHECK(differing(ninety, thirty) > 0);        // the steps moved the game
    AETHER_CHECK(differing(ninety, ninety_again) == 0); // the same steps reach the same state
}
#endif

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

AETHER_TEST(LiveViewportPanel_SaysWhatItMakesOfAMaterialGraphItCannotDrawYet) {
    std::unique_ptr<gfx::rhi::IDevice> device = TryDevice();
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    Context imgui;
    const stdfs::path project = MakeProject("LiveGraph");
    const stdfs::path content = project.parent_path() / "Content";
    stdfs::create_directories(content / "Materials");
    auto write = [&](const char* name, const nlohmann::json& json) { std::ofstream(content / "Materials" / name) << json.dump(); };
    mat::Material tint; // a graph that is valid: a tint and a roughness into the outputs
    tint.parameters.push_back({"Tint", mat::PinType::Float3, {1, 0.8f, 0.6f, 0}, "", "Surface"});
    tint.parameters.push_back({"Roughness", mat::PinType::Float, {0.4f, 0, 0, 0}, "", ""});
    const mat::NodeId out = tint.Add("Material.Output", {}, 600, 0);
    const mat::NodeId color = tint.Add("Param.Vector:Tint");
    const mat::NodeId rough = tint.Add("Param.Scalar:Roughness");
    tint.Connect(color, "value", out, "BaseColor").Connect(rough, "value", out, "Roughness");
    write("Tint.amat", mat::MaterialToJson(tint));
    mat::Material broken; // no Material Output: not a graph the game can run
    broken.Add("Const.Float", {{"value", 0.5f}});
    write("Broken.amat", mat::MaterialToJson(broken));
    write("Garbage.amat", nlohmann::json("not a graph"));

    ScenePanel scene(project.parent_path() / "Content");
    AETHER_CHECK(scene.OpenScene("Scenes/Main.ascene", true));
    SceneDocument& document = scene.Document();
    Entity model = kNullEntity;
    for (const Entity entity : document.Entities()) {
        if (document.GetWorld().HasComponent<ModelRenderer>(entity)) model = entity;
    }
    AETHER_CHECK(model != kNullEntity);
    if (model == kNullEntity) return;
    const ComponentId overrides = GetComponentId<MaterialParameters>();
    AETHER_CHECK(document.AddComponent(model, overrides));
    auto use = [&](const char* path) { AETHER_CHECK(document.SetComponentField(model, overrides, "material", reflect::Any(std::string(path)), true)); };
    LiveViewportPanel panel(*device, scene, [&] { return project; });
    panel.width = 160;
    panel.height = 90;
    panel.reload_interval = 0.0f;
    use("Materials/Tint.amat");
    AETHER_CHECK(panel.Start());
    std::vector<player::GraphMaterialReport> reports = panel.GraphMaterials();
    AETHER_CHECK(reports.size() == 1 && reports[0].found && reports[0].compiles && reports[0].parameters > 0 && reports[0].live_nodes == 2 && reports[0].permutation_key != 0);
    AETHER_CHECK(reports.size() == 1 && reports[0].drawable && reports[0].message.find("drawn") != std::string::npos);

    use("Materials/Broken.amat");
    AETHER_CHECK(panel.Start());
    reports = panel.GraphMaterials();
    AETHER_CHECK(reports.size() == 1 && reports[0].found && !reports[0].compiles && !reports[0].message.empty());
    use("Materials/Garbage.amat");
    AETHER_CHECK(panel.Start());
    reports = panel.GraphMaterials();
    AETHER_CHECK(reports.size() == 1 && reports[0].found && !reports[0].compiles && reports[0].message.find("readable") != std::string::npos);
    use("Materials/Missing.amat");
    AETHER_CHECK(panel.Start());
    reports = panel.GraphMaterials();
    AETHER_CHECK(reports.size() == 1 && !reports[0].found && !reports[0].compiles && reports[0].message.find("Not in the package") != std::string::npos);
    use("Materials/Skin.amati");
    AETHER_CHECK(panel.Start());
    reports = panel.GraphMaterials();
    AETHER_CHECK(reports.size() == 1 && !reports[0].compiles && reports[0].message.find("Not in the package") != std::string::npos);
    use("");
    AETHER_CHECK(panel.GraphMaterials().empty());                                 // no graph named: nothing said
}

AETHER_TEST(LiveViewportPanel_AMaterialGraphIsDrawnInPlaceOfTheModelsSurface) {
    std::unique_ptr<gfx::rhi::IDevice> device = TryDevice();
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    Context imgui;
    const stdfs::path project = MakeProject("LiveGraphDraw");
    const stdfs::path content = project.parent_path() / "Content";
    stdfs::create_directories(content / "Materials");
    auto write = [&](const char* name, const mat::Material& material) { std::ofstream(content / "Materials" / name) << mat::MaterialToJson(material).dump(); };
    mat::Material red; // a red surface: a Tint parameter into BaseColor, a Roughness parameter, nothing else
    red.parameters.push_back({"Tint", mat::PinType::Float3, {0.9f, 0.05f, 0.05f, 0}, "", "Surface"});
    red.parameters.push_back({"Roughness", mat::PinType::Float, {0.6f, 0, 0, 0}, "", ""});
    {
        const mat::NodeId out = red.Add("Material.Output", {}, 600, 0);
        const mat::NodeId color = red.Add("Param.Vector:Tint");
        const mat::NodeId rough = red.Add("Param.Scalar:Roughness");
        red.Connect(color, "value", out, "BaseColor").Connect(rough, "value", out, "Roughness");
    }
    write("Red.amat", red);
    // A graph that samples a texture asset of the project: a solid blue picture, found by the GUID the project's asset database gave it.
    stdfs::create_directories(content / "textures");
    std::vector<u8> rgb(4 * 4 * 3);
    for (usize i = 0; i < rgb.size(); i += 3) {
        rgb[i] = 10;
        rgb[i + 1] = 10;
        rgb[i + 2] = 240;
    }
    const std::vector<u8> png = assets::EncodePngRgb8(4, 4, rgb);
    std::ofstream(content / "textures" / "blue.png", std::ios::binary).write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    assets::AssetDatabase database(content);
    database.Scan();
    const assets::AssetRecord* blue = database.FindByPath("textures/blue.png");
    AETHER_CHECK(blue != nullptr);
    if (blue == nullptr) return;
    mat::Material textured;
    textured.parameters.push_back({"Albedo", mat::PinType::Texture, {}, assets::ToString(blue->guid), "Surface"});
    {
        const mat::NodeId out = textured.Add("Material.Output", {}, 600, 0);
        const mat::NodeId tex = textured.Add("Param.Texture:Albedo");
        const mat::NodeId sample = textured.Add("Texture.Sample");
        textured.Connect(tex, "value", sample, "texture").Connect(sample, "rgb", out, "BaseColor");
    }
    write("Textured.amat", textured);
    mat::Material unlit; // a shading model the renderer does not draw with a graph yet
    unlit.shading = mat::ShadingModel::Unlit;
    {
        const mat::NodeId out = unlit.Add("Material.Output", {}, 600, 0);
        const mat::NodeId color = unlit.Add("Const.Float3", {{"value", {0.0f, 1.0f, 0.0f}}});
        unlit.Connect(color, "value", out, "BaseColor");
    }
    write("Unlit.amat", unlit);

    ScenePanel scene(content);
    AETHER_CHECK(scene.OpenScene("Scenes/Main.ascene", true));
    SceneDocument& document = scene.Document();
    std::vector<Entity> models;
    for (const Entity entity : document.Entities()) {
        if (document.GetWorld().HasComponent<ModelRenderer>(entity)) models.push_back(entity);
    }
    AETHER_CHECK(models.size() >= 2);
    const ComponentId overrides = GetComponentId<MaterialParameters>();
    for (const Entity entity : models) AETHER_CHECK(document.AddComponent(entity, overrides));
    auto use = [&](const char* path) {
        for (const Entity entity : models) AETHER_CHECK(document.SetComponentField(entity, overrides, "material", reflect::Any(std::string(path)), true));
    };
    LiveViewportPanel panel(*device, scene, [&] { return project; });
    panel.width = 160;
    panel.height = 90;
    panel.reload_interval = 0.0f;
    auto picture = [&](const char* path) {
        use(path);
        AETHER_CHECK(panel.Start());
        for (int i = 0; i < 6; ++i) panel.Advance(0.2f);
        return panel.Viewport().Pixels();
    };
    auto reds = [](const std::vector<u8>& pixels) {
        usize count = 0;
        for (usize i = 0; i + 3 < pixels.size(); i += 4) count += pixels[i] > pixels[i + 1] + 50 && pixels[i] > pixels[i + 2] + 50;
        return count;
    };
    const std::vector<u8> own = picture("");
    const std::vector<u8> drawn = picture("Materials/Red.amat");
    AETHER_CHECK(panel.GraphMaterials().size() == 1 && panel.GraphMaterials()[0].drawable);
    AETHER_CHECK(reds(own) < 160u * 90u / 100);                     // the scene's own surfaces are grey and blue and tan, not red
    AETHER_CHECK(reds(drawn) > 160u * 90u / 3);                      // every model is the graph's red now: most of the picture (the floor is a model)
    AETHER_CHECK(DifferingPixels(own, drawn) > 160u * 90u / 3);
    const std::vector<u8> sampled = picture("Materials/Textured.amat");
    AETHER_CHECK(panel.GraphMaterials().size() == 1 && panel.GraphMaterials()[0].drawable && panel.GraphMaterials()[0].textures == 1);
    auto blues = [](const std::vector<u8>& pixels) {
        usize count = 0;
        for (usize i = 0; i + 3 < pixels.size(); i += 4) count += pixels[i + 2] > pixels[i] + 50 && pixels[i + 2] > pixels[i + 1] + 50;
        return count;
    };
    AETHER_CHECK(blues(sampled) > 160u * 90u / 3);                    // the texture's blue, read through the scene's own texture table
    AETHER_CHECK(blues(own) < 160u * 90u / 4);                        // (the player's blue pillar is a small part of the scene's own picture)
    const std::vector<u8> skipped = picture("Materials/Unlit.amat");
    AETHER_CHECK(panel.GraphMaterials().size() == 1 && panel.GraphMaterials()[0].compiles && !panel.GraphMaterials()[0].drawable);
    AETHER_CHECK(DifferingPixels(own, skipped) < 160u * 90u / 50);   // not drawn yet: the model's own surface, as before
}

AETHER_TEST(LiveViewportPanel_ASavedMaterialGraphShowsWithoutPressingReload) {
    std::unique_ptr<gfx::rhi::IDevice> device = TryDevice();
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    Context imgui;
    const stdfs::path project = MakeProject("LiveGraphEdit");
    const stdfs::path content = project.parent_path() / "Content";
    stdfs::create_directories(content / "Materials");
    auto tinted = [](Vec4 tint) { // a graph of one colour parameter into BaseColor
        mat::Material m;
        m.parameters.push_back({"Tint", mat::PinType::Float3, tint, "", "Surface"});
        const mat::NodeId out = m.Add("Material.Output", {}, 600, 0);
        const mat::NodeId color = m.Add("Param.Vector:Tint");
        m.Connect(color, "value", out, "BaseColor");
        return m;
    };
    auto save = [&](const char* name, const mat::Material& material) { std::ofstream(content / "Materials" / name, std::ios::trunc) << mat::MaterialToJson(material).dump(); };
    save("Look.amat", tinted({0.9f, 0.05f, 0.05f, 0}));
    save("Later.amat", tinted({0.05f, 0.1f, 0.95f, 0}));

    ScenePanel scene(content);
    AETHER_CHECK(scene.OpenScene("Scenes/Main.ascene", true));
    SceneDocument& document = scene.Document();
    std::vector<Entity> models;
    for (const Entity entity : document.Entities()) {
        if (document.GetWorld().HasComponent<ModelRenderer>(entity)) models.push_back(entity);
    }
    const ComponentId overrides = GetComponentId<MaterialParameters>();
    for (const Entity entity : models) AETHER_CHECK(document.AddComponent(entity, overrides));
    auto use = [&](const char* path) {
        for (const Entity entity : models) AETHER_CHECK(document.SetComponentField(entity, overrides, "material", reflect::Any(std::string(path)), true));
    };
    use("Materials/Look.amat");
    LiveViewportPanel panel(*device, scene, [&] { return project; });
    panel.width = 160;
    panel.height = 90;
    panel.reload_interval = 0.0f;
    AETHER_CHECK(panel.Start());
    auto settle = [&] { for (int i = 0; i < 6; ++i) panel.Advance(0.2f); return panel.Viewport().Pixels(); };
    auto dominant = [](const std::vector<u8>& pixels, int channel) {
        usize count = 0;
        for (usize i = 0; i + 3 < pixels.size(); i += 4) {
            const int c = pixels[i + static_cast<usize>(channel)], a = pixels[i + static_cast<usize>((channel + 1) % 3)], b = pixels[i + static_cast<usize>((channel + 2) % 3)];
            count += c > a + 50 && c > b + 50;
        }
        return count;
    };
    const u64 reloads = panel.Viewport().SceneReloads();
    const usize frame = 160u * 90u;
    std::vector<u8> picture = settle();
    AETHER_CHECK(dominant(picture, 0) > frame / 3);                                    // the graph's red

    // Saved with another colour: green, with no Reload pressed and the scene not loaded again.
    save("Look.amat", tinted({0.05f, 0.9f, 0.05f, 0}));
    picture = settle();
    AETHER_CHECK(dominant(picture, 1) > frame / 3 && dominant(picture, 0) < frame / 10);
    AETHER_CHECK(panel.Viewport().SceneReloads() == reloads);

    // Saved broken (no output): it says so, and the models show their own surfaces; saved right again, it is back.
    mat::Material broken;
    broken.Add("Const.Float", {{"value", 0.5f}});
    save("Look.amat", broken);
    picture = settle();
    AETHER_CHECK(dominant(picture, 1) < frame / 10);
    AETHER_CHECK(panel.GraphMaterials().size() == 1 && !panel.GraphMaterials()[0].compiles);
    save("Look.amat", tinted({0.9f, 0.05f, 0.05f, 0}));
    picture = settle();
    AETHER_CHECK(dominant(picture, 0) > frame / 3);
    AETHER_CHECK(panel.GraphMaterials().size() == 1 && panel.GraphMaterials()[0].drawable);

    // A graph named only after the cook, which the package never had: handed to the game as it is, and shown.
    use("Materials/Later.amat");
    picture = settle();
    AETHER_CHECK(dominant(picture, 2) > frame / 3);
    AETHER_CHECK(panel.Viewport().SceneReloads() == reloads); // naming it is a patched edit of the scene
}

AETHER_TEST(LiveViewportPanel_ObjectsGiveAGraphTheirOwnValuesAndAnInstanceIsDrawnWithItsParent) {
    std::unique_ptr<gfx::rhi::IDevice> device = TryDevice();
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    Context imgui;
    const stdfs::path project = MakeProject("LiveGraphValues");
    const stdfs::path content = project.parent_path() / "Content";
    stdfs::create_directories(content / "Materials");
    auto tinted = [](Vec4 tint, mat::ShadingModel shading = mat::ShadingModel::DefaultLit) {
        mat::Material m;
        m.shading = shading;
        m.parameters.push_back({"Tint", mat::PinType::Float3, tint, "", "Surface"});
        const mat::NodeId out = m.Add("Material.Output", {}, 600, 0);
        const mat::NodeId color = m.Add("Param.Vector:Tint");
        m.Connect(color, "value", out, "BaseColor");
        return m;
    };
    auto save = [&](const char* name, const mat::Material& material) { std::ofstream(content / "Materials" / name, std::ios::trunc) << mat::MaterialToJson(material).dump(); };
    save("Base.amat", tinted({0.9f, 0.05f, 0.05f, 0}));
    mat::MaterialInstance teal; // the same graph with another colour
    teal.parent = "Materials/Base.amat";
    teal.SetValue("Tint", Vec4(0.05f, 0.8f, 0.75f, 0.0f));
    std::string error;
    AETHER_CHECK(mat::SaveInstance(teal, content / "Materials" / "Teal.amati", &error));

    ScenePanel scene(content);
    AETHER_CHECK(scene.OpenScene("Scenes/Main.ascene", true));
    SceneDocument& document = scene.Document();
    std::vector<Entity> models;
    Entity floor = kNullEntity;
    for (const Entity entity : document.Entities()) {
        const ModelRenderer* renderer = document.GetWorld().GetComponent<ModelRenderer>(entity);
        if (renderer == nullptr) continue;
        models.push_back(entity);
        if (std::string(renderer->asset_path).find("Floor") != std::string::npos) floor = entity;
    }
    AETHER_CHECK(floor != kNullEntity && models.size() >= 2);
    if (floor == kNullEntity) return;
    const ComponentId overrides = GetComponentId<MaterialParameters>();
    for (const Entity entity : models) AETHER_CHECK(document.AddComponent(entity, overrides));
    auto use = [&](const char* path) {
        for (const Entity entity : models) AETHER_CHECK(document.SetComponentField(entity, overrides, "material", reflect::Any(std::string(path)), true));
    };
    use("Materials/Base.amat");
    LiveViewportPanel panel(*device, scene, [&] { return project; });
    panel.width = 160;
    panel.height = 90;
    panel.reload_interval = 0.0f;
    AETHER_CHECK(panel.Start());
    auto settle = [&] { for (int i = 0; i < 6; ++i) panel.Advance(0.2f); return panel.Viewport().Pixels(); };
    const usize frame = 160u * 90u;
    auto red = [](const std::vector<u8>& p) { usize n = 0; for (usize i = 0; i + 3 < p.size(); i += 4) n += p[i] > p[i + 1] + 50 && p[i] > p[i + 2] + 50; return n; };
    auto green = [](const std::vector<u8>& p) { usize n = 0; for (usize i = 0; i + 3 < p.size(); i += 4) n += p[i + 1] > p[i] + 50 && p[i + 1] > p[i + 2] + 50; return n; };
    auto cyan = [](const std::vector<u8>& p) { usize n = 0; for (usize i = 0; i + 3 < p.size(); i += 4) n += p[i + 1] > p[i] + 50 && p[i + 2] > p[i] + 50; return n; };
    std::vector<u8> picture = settle();
    AETHER_CHECK(red(picture) > frame / 3 && green(picture) < 8);

    // One object gives the graph its own Tint: the floor goes green (a floor is most of the picture) while the rest keep the graph's red, from one shader.
    MaterialParameterValue tint;
    tint.name = "Tint";
    tint.value = Vec4(0.05f, 0.9f, 0.05f, 0.0f);
    AETHER_CHECK(document.SetComponentField(floor, overrides, "values", reflect::Any(std::vector<MaterialParameterValue>{tint}), true));
    picture = settle();
    AETHER_CHECK(green(picture) > frame / 3);
    AETHER_CHECK(red(picture) >= 8);                                   // the pillar and the cubes still use the graph's own
    AETHER_CHECK(panel.Viewport().SceneReloads() == 0);                // a patched edit of the scene, not a reload

    // An instance of the graph (a .amati, with its parent cooked beside it): the instance's colour.
    AETHER_CHECK(document.SetComponentField(floor, overrides, "values", reflect::Any(std::vector<MaterialParameterValue>{}), true));
    use("Materials/Teal.amati");
    AETHER_CHECK(panel.Start());
    picture = settle();
    AETHER_CHECK(cyan(picture) > frame / 3 && panel.GraphMaterials().size() == 1 && panel.GraphMaterials()[0].drawable);

    // The parent graph saved as something the renderer cannot draw (unlit): the instance under it falls back with it, with no Reload; saved right again, it is back.
    save("Base.amat", tinted({0.9f, 0.05f, 0.05f, 0}, mat::ShadingModel::Unlit));
    picture = settle();
    AETHER_CHECK(cyan(picture) < frame / 10);
    save("Base.amat", tinted({0.9f, 0.05f, 0.05f, 0}));
    picture = settle();
    AETHER_CHECK(cyan(picture) > frame / 3);
}

AETHER_TEST(LiveViewportPanel_AGraphsTexturesAreNotTakenForTheModelsOwnSlots) {
    std::unique_ptr<gfx::rhi::IDevice> device = TryDevice();
    if (!device) {
        std::printf("    (no Vulkan device: install a driver, or Mesa's lavapipe, to run this)\n");
        return;
    }
    Context imgui;
    const stdfs::path project = MakeProject("LiveGraphSlots");
    const stdfs::path content = project.parent_path() / "Content";
    stdfs::create_directories(content / "Materials");
    stdfs::create_directories(content / "textures");
    auto solid = [&](const char* name, u8 r, u8 g, u8 b) {
        std::vector<u8> rgb(4 * 4 * 3);
        for (usize i = 0; i < rgb.size(); i += 3) { rgb[i] = r; rgb[i + 1] = g; rgb[i + 2] = b; }
        const std::vector<u8> png = assets::EncodePngRgb8(4, 4, rgb);
        std::ofstream(content / "textures" / name, std::ios::binary).write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    };
    solid("blue.png", 10, 10, 240);
    solid("flat.png", 128, 128, 255);   // what a normal map of a flat surface looks like
    solid("tilted.png", 255, 255, 128); // and one tilted hard: read as the model's normal map it would change every highlight
    assets::AssetDatabase database(content);
    database.Scan();
    auto guid = [&](const char* path) { const assets::AssetRecord* r = database.FindByPath(path); return r != nullptr ? assets::ToString(r->guid) : std::string(); };
    // Two textures: the blue one is the colour; the second is sampled and multiplied by zero, so it is a live texture in the graph that changes nothing by itself.
    auto two = [&](const char* second) {
        mat::Material m;
        m.parameters.push_back({"Colour", mat::PinType::Texture, {}, guid("textures/blue.png"), "Surface"});
        m.parameters.push_back({"Other", mat::PinType::Texture, {}, guid(second), "Surface"});
        for (int i = 0; i < 6; ++i) m.parameters.push_back({"Gain" + std::to_string(i), mat::PinType::Float, {1.1f, 0, 0, 0}, "", ""}); // fill the words a draw carries, so the factor word the model's normal map strength reads is not zero
        const mat::NodeId out = m.Add("Material.Output", {}, 600, 0);
        const mat::NodeId t1 = m.Add("Param.Texture:Colour"), s1 = m.Add("Texture.Sample"), t2 = m.Add("Param.Texture:Other"), s2 = m.Add("Texture.Sample");
        const mat::NodeId zero = m.Add("Const.Float", {{"value", 0.0f}}), scaled = m.Add("Math.Multiply"), sum = m.Add("Math.Add");
        m.Connect(t1, "value", s1, "texture").Connect(t2, "value", s2, "texture").Connect(s2, "rgb", scaled, "a").Connect(zero, "value", scaled, "b");
        m.Connect(s1, "rgb", sum, "a").Connect(scaled, "result", sum, "b").Connect(sum, "result", out, "BaseColor");
        return m;
    };
    std::ofstream(content / "Materials" / "Flat.amat") << mat::MaterialToJson(two("textures/flat.png")).dump();
    std::ofstream(content / "Materials" / "Tilted.amat") << mat::MaterialToJson(two("textures/tilted.png")).dump();

    ScenePanel scene(content);
    AETHER_CHECK(scene.OpenScene("Scenes/Main.ascene", true));
    SceneDocument& document = scene.Document();
    std::vector<Entity> models;
    for (const Entity entity : document.Entities()) {
        if (document.GetWorld().HasComponent<ModelRenderer>(entity)) models.push_back(entity);
    }
    const ComponentId overrides = GetComponentId<MaterialParameters>();
    for (const Entity entity : models) AETHER_CHECK(document.AddComponent(entity, overrides));
    LiveViewportPanel panel(*device, scene, [&] { return project; });
    panel.width = 160;
    panel.height = 90;
    panel.reload_interval = 0.0f;
    auto picture = [&](const char* path) {
        for (const Entity entity : models) AETHER_CHECK(document.SetComponentField(entity, overrides, "material", reflect::Any(std::string(path)), true));
        AETHER_CHECK(panel.Start());
        for (int i = 0; i < 6; ++i) panel.Advance(0.2f);
        return panel.Viewport().Pixels();
    };
    const std::vector<u8> flat = picture("Materials/Flat.amat");
    AETHER_CHECK(panel.GraphMaterials().size() == 1 && panel.GraphMaterials()[0].drawable && panel.GraphMaterials()[0].textures == 2);
    const std::vector<u8> tilted = picture("Materials/Tilted.amat");
    AETHER_CHECK(DifferingPixels(flat, tilted) < 160u * 90u / 100);       // the second texture does nothing to the lighting: it is not the model's normal map
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
