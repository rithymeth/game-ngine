#include "workspace/editor_workspace.h"
#include "workspace/command_registry.h"

#include "core/scene_document.h"
#include "world/scene_panel.h"

#include "test_framework.h"

#include <imgui.h>

// Stage 1 of docs/design/EDITOR_PARITY_AUDIT.md: the main toolbar. Its buttons are the command table and the open
// document, so what a click does is testable without the pixels: Step advances exactly one frame, and the tool
// actions open the tool they name.

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

// The gate item: while paused, Step makes exactly the next frame simulate, and resuming ignores it.
AETHER_TEST(MainToolbar_StepAdvancesExactlyOneFrame) {
    SceneDocument document;
    document.CreateEntity("player");
    CHECK(document.PlayState() == PlaySession::State::Editing);
    CHECK(!document.ShouldSimulate());

    document.Play();
    CHECK(document.PlayState() == PlaySession::State::Playing);
    CHECK(document.ShouldSimulate());

    document.Pause();
    CHECK(document.PlayState() == PlaySession::State::Paused);
    CHECK(!document.ShouldSimulate());
    document.Step();
    CHECK(document.ShouldSimulate()); // exactly one frame
    CHECK(!document.ShouldSimulate());

    document.Play(); // resume
    CHECK(document.PlayState() == PlaySession::State::Playing);
    document.Step(); // ignored while playing
    CHECK(document.ShouldSimulate());
    CHECK(document.ShouldSimulate());

    document.Stop();
    CHECK(document.PlayState() == PlaySession::State::Editing);
    CHECK(!document.ShouldSimulate());
    CHECK(document.Entities().size() == 1); // the played world was discarded, the authored one restored
}

// Undo, Redo and Save come from the command table, so the toolbar's enabled state is the document's.
AETHER_TEST(MainToolbar_EnabledStateFollowsTheDocument) {
    HeadlessImGui imgui;
    EditorWorkspace workspace;
    const CommandRegistry& commands = workspace.Commands();
    const Command* undo = commands.Find("edit.undo");
    const Command* redo = commands.Find("edit.redo");
    const Command* save = commands.Find("scene.save");
    CHECK(undo != nullptr && redo != nullptr && save != nullptr);
    if (undo == nullptr || redo == nullptr || save == nullptr) return;

    CHECK(!commands.Enabled(*undo));
    CHECK(!commands.Enabled(*redo));
    workspace.Scene().Document().CreateEntity("crate");
    CHECK(commands.Enabled(*undo));
    CHECK(workspace.RunCommand("edit.undo"));
    CHECK(commands.Enabled(*redo));
    CHECK(workspace.RunCommand("edit.redo"));
    CHECK(commands.Enabled(*save));
}

// Build and Package and Project Settings open the tool they name in its own window; Settings opens Preferences.
AETHER_TEST(MainToolbar_ToolActionsOpenTheirWindows) {
    HeadlessImGui imgui;
    EditorWorkspace workspace;
    CHECK(!workspace.preferences_open);

    CHECK(workspace.RunCommand("project.build"));
    const i64 build = workspace.FindTool("Build and Package");
    CHECK(build >= 0);
    if (build >= 0) CHECK(workspace.IsWindowOpen(static_cast<usize>(build)));

    CHECK(workspace.RunCommand("project.settings"));
    const i64 settings = workspace.FindTool("Project Settings");
    CHECK(settings >= 0);
    if (settings >= 0) CHECK(workspace.IsWindowOpen(static_cast<usize>(settings)));

    CHECK(workspace.RunCommand("view.preferences"));
    CHECK(workspace.preferences_open);
}

// The toolbar draws in each play state without needing a host of its own.
AETHER_TEST(MainToolbar_DrawsInEveryPlayState) {
    HeadlessImGui imgui;
    EditorWorkspace workspace;
    SceneDocument& document = workspace.Scene().Document();
    document.CreateEntity("player");

    const auto draw_once = [&workspace] {
        ImGui::NewFrame();
        if (ImGui::Begin("##toolbar_test")) workspace.DrawMainToolbar();
        ImGui::End();
        ImGui::Render();
    };
    draw_once(); // editing: Save, Undo, Redo, the transform tools, Play, Build, Settings
    document.Play();
    draw_once(); // playing: Pause, Step (disabled), Stop, the playing underline
    document.Pause();
    draw_once(); // paused: Resume, Step (enabled)
    document.Stop();
    CHECK(document.PlayState() == PlaySession::State::Editing);
}
