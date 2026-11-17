#include "workspace/editor_workspace.h"
#include "workspace/command_registry.h"

#include "core/scene_document.h"
#include "world/scene_panel.h"
#include "world/scene_view_panel.h"
#include "world/viewport_tools.h"

#include "test_framework.h"

#include <imgui.h>

#include <algorithm>
#include <string>
#include <vector>

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
    draw_once(); // editing: Save, Undo, Redo, the transform tools, Build, Settings, the breadcrumb
    document.Play();
    draw_once(); // playing: the breadcrumb turns amber while the scene has no file
    document.Pause();
    draw_once(); // paused
    document.Stop();
    CHECK(document.PlayState() == PlaySession::State::Editing);
}

// The concept's row 3: Move/Rotate/Scale as icons at the left, then the "Perspective", "Lit" and "Show" dropdowns, then the
// space, pivot and snap controls. The rectangles are what a host aligns to and a test clicks; the choices are the view's
// own state, so a click here is the same change the picture's chips make.
AETHER_TEST(MainToolbar_DrawsTheConceptsRowThree) {
    HeadlessImGui imgui;
    EditorWorkspace workspace;
    SceneViewPanel& view = workspace.SceneView();
    auto frame = [&] {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(1500.0f, 220.0f));
        if (ImGui::Begin("##toolbar_row3", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) workspace.DrawMainToolbar();
        ImGui::End();
        ImGui::Render();
    };
    auto step = [&](const ImVec2& at, bool down) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(at.x, at.y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
        frame();
    };
    auto click = [&](const ImVec2& at) {
        step(ImVec2(2.0f, 2.0f), false);
        step(at, false);
        step(at, true);
        step(at, false);
        frame();
    };
    auto centre = [](const ImVec2& low, const ImVec2& high) { return ImVec2((low.x + high.x) * 0.5f, (low.y + high.y) * 0.5f); };
    auto item_at = [](const std::vector<std::string>& items, const char* label) {
        const auto it = std::find(items.begin(), items.end(), label);
        return it == items.end() ? std::ptrdiff_t(-1) : static_cast<std::ptrdiff_t>(it - items.begin());
    };

    step(ImVec2(2.0f, 2.0f), false);
    const ViewportTools::ModeIcons icons = view.Tools().LastModeIcons();
    CHECK(icons.move_max.x > icons.move_min.x && icons.rotate_max.x > icons.rotate_min.x && icons.scale_max.x > icons.scale_min.x);
    CHECK(icons.rotate_min.x >= icons.move_max.x && icons.scale_min.x >= icons.rotate_max.x); // the icons sit in a row

    click(centre(icons.scale_min, icons.scale_max));
    CHECK(view.Tools().Mode() == GizmoMode::Scale);
    click(centre(icons.move_min, icons.move_max));
    CHECK(view.Tools().Mode() == GizmoMode::Move);

    const SceneViewPanel::LookControls row = view.LastLookControls();
    CHECK(row.projection_max.x > row.projection_min.x && row.mode_max.x > row.mode_min.x && row.show_max.x > row.show_min.x);
    CHECK(row.mode_min.x >= row.projection_max.x && row.show_min.x >= row.mode_max.x); // and the dropdowns after them

    // Perspective dropdown: the five choices; Top is the top orthographic view.
    click(centre(row.projection_min, row.projection_max));
    SceneViewPanel::LookControls open = view.LastLookControls();
    CHECK(open.items.size() == 5);
    const std::ptrdiff_t top = item_at(open.items, "Top");
    CHECK(top >= 0);
    if (top >= 0) click(centre(open.item_min[static_cast<usize>(top)], open.item_max[static_cast<usize>(top)]));
    CHECK(view.Camera().projection == SceneViewProjection::Orthographic);
    CHECK(view.Camera().Orientation() == SceneViewOrientation::Top);

    // Lit dropdown: the four view modes; Lit is what the shell's renderer draws.
    open = view.LastLookControls();
    click(centre(open.mode_min, open.mode_max));
    open = view.LastLookControls();
    CHECK(open.items.size() == 4);
    const std::ptrdiff_t lit = item_at(open.items, "Lit");
    CHECK(lit >= 0);
    if (lit >= 0) click(centre(open.item_min[static_cast<usize>(lit)], open.item_max[static_cast<usize>(lit)]));
    CHECK(view.mode == SceneViewMode::Lit);

    // Show dropdown: the five overlays; a click on one flips it.
    open = view.LastLookControls();
    click(centre(open.show_min, open.show_max));
    open = view.LastLookControls();
    CHECK(open.items.size() == 5);
    const std::ptrdiff_t grid = item_at(open.items, "Grid");
    CHECK(grid >= 0);
    const bool grid_before = view.show.grid;
    if (grid >= 0) click(centre(open.item_min[static_cast<usize>(grid)], open.item_max[static_cast<usize>(grid)]));
    CHECK(view.show.grid != grid_before);
}
