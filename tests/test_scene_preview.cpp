#include "core/scene_preview.h"
#include "ui/image_texture.h"
#include "workspace/editor_workspace.h"
#include "world/scene_preview_panel.h"

#include "aether/core/log.h"
#include "aether/platform/filesystem.h"
#include "aether/templates/templates.h"
#include "test_framework.h"

#include <algorithm>
#include <filesystem>
#include <imgui.h>
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
