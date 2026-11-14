#include "workspace/editor_workspace.h"

#include "core/editor_document.h"
#include "core/notifications.h"
#include "core/scene_document.h"
#include "world/scene_panel.h"

#include "test_framework.h"

#include <imgui.h>

#include <string>

// Stage 1 of docs/design/EDITOR_PARITY_AUDIT.md: the status bar. Its content is a snapshot (`StatusBar()`) so the
// questions it answers -- what project, which document and is it dirty, is the game running, is there a build, are
// there unread messages -- are testable without reading pixels.

using namespace aether;
using namespace aether::editor;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1600, 960);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(context_); }

private:
    ImGuiContext* context_ = nullptr;
};

} // namespace

// The project, the active document and its dirty mark come from the workspace's own state, not a second copy.
AETHER_TEST(StatusBar_NamesTheProjectAndDocument) {
    EditorWorkspace workspace;
    const EditorWorkspace::StatusBarInfo info = workspace.StatusBar();
    CHECK(!info.project.empty());
    CHECK(info.project != "No project"); // the default workspace opens the sample project
    CHECK(info.document == workspace.ActiveDocument().DisplayName());
    CHECK(!info.dirty);
    CHECK(std::string(info.play) == "Editing");
    CHECK(info.job.empty());

    const Entity crate = workspace.Scene().Document().CreateEntity("crate");
    workspace.Scene().Document().CreateEntity("barrel");
    const EditorWorkspace::StatusBarInfo edited = workspace.StatusBar();
    CHECK(edited.dirty);
    CHECK(edited.document == workspace.ActiveDocument().DisplayName());
    CHECK(edited.selected == 0); // nothing picked yet

    workspace.Scene().Document().SetSelection({crate});
    CHECK(workspace.StatusBar().selected == 1);
    workspace.Scene().Document().ClearSelection();
    CHECK(workspace.StatusBar().selected == 0);
}

// The branch is the workspace's own value, so a host (or a test) can show a repository the workspace didn't ask git
// about, and refreshing does not overwrite a value the host set.
AETHER_TEST(StatusBar_ShowsTheSourceControlBranch) {
    HeadlessImGui imgui;
    EditorWorkspace workspace;
    workspace.SetSourceControlBranch("feature/parity");
    CHECK(workspace.StatusBar().branch == "feature/parity");
    workspace.RefreshSourceControl();
    CHECK(workspace.StatusBar().branch == "feature/parity");

    ImGui::NewFrame();
    if (ImGui::Begin("##status_test")) workspace.DrawStatusBar(); // the branch segment draws
    ImGui::End();
    ImGui::Render();

    workspace.SetSourceControlBranch(""); // no repository: the segment is left out
    CHECK(workspace.StatusBar().branch.empty());
}

// The play segment follows the document's PlaySession through its three states.
AETHER_TEST(StatusBar_FollowsThePlayState) {
    EditorWorkspace workspace;
    SceneDocument& document = workspace.Scene().Document();
    document.CreateEntity("player");

    document.Play();
    CHECK(std::string(workspace.StatusBar().play) == "Playing");
    document.Pause();
    CHECK(std::string(workspace.StatusBar().play) == "Paused");
    document.Stop();
    CHECK(std::string(workspace.StatusBar().play) == "Editing");
}

// The unread count is the notification service's, and drawing the bar does not mark anything read: only the
// Messages button does.
AETHER_TEST(StatusBar_CountsUnreadMessages) {
    HeadlessImGui imgui;
    EditorWorkspace workspace;
    CHECK(workspace.StatusBar().unread == 0);

    workspace.Notifications().Post(NotificationSeverity::Info, "Saved", "Scenes/Main.ascene");
    CHECK(workspace.StatusBar().unread == 1);

    ImGui::NewFrame();
    if (ImGui::Begin("##status_test")) workspace.DrawStatusBar();
    ImGui::End();
    ImGui::Render();
    CHECK(workspace.StatusBar().unread == 1);

    workspace.Notifications().MarkAllRead();
    CHECK(workspace.StatusBar().unread == 0);
}

// The bar draws in every play state without a host of its own.
AETHER_TEST(StatusBar_DrawsInEveryPlayState) {
    HeadlessImGui imgui;
    EditorWorkspace workspace;
    SceneDocument& document = workspace.Scene().Document();
    document.CreateEntity("player");

    const auto draw_once = [&workspace] {
        ImGui::NewFrame();
        if (ImGui::Begin("##status_test")) workspace.DrawStatusBar();
        ImGui::End();
        ImGui::Render();
    };
    draw_once(); // Editing
    document.Play();
    draw_once(); // Playing, amber
    document.Pause();
    draw_once(); // Paused
    document.Stop();
    draw_once(); // back to Editing
    CHECK(std::string(workspace.StatusBar().play) == "Editing");
}
