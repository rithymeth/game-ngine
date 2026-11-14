#include "workspace/editor_workspace.h"
#include "workspace/command_registry.h"

#include "core/scene_document.h"
#include "world/scene_panel.h"

#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"

#include "test_framework.h"

#include <imgui.h>

// Stage 1 of docs/design/EDITOR_PARITY_AUDIT.md: the Asset and Actor menus. Both are plain command-table entries, so
// what a menu item does is testable by id: an Actor item makes the entity kind it names on the open scene (one undo
// step), and an Asset item puts the Content Browser where the action happens.

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

// Each Actor command creates its kind, selects it, and is one undo step.
AETHER_TEST(Menus_ActorCommandsCreateTheirEntityKind) {
    EditorWorkspace workspace;
    SceneDocument& document = workspace.Scene().Document();
    const usize before = document.Entities().size();

    CHECK(workspace.RunCommand("actor.camera"));
    CHECK(document.Entities().size() == before + 1);
    CHECK(!document.Selection().empty());
    if (!document.Selection().empty()) {
        const Entity camera = document.Selection().front();
        CHECK(document.GetWorld().HasComponent<Camera>(camera));
        CHECK(document.GetWorld().HasComponent<EntityName>(camera));
    }

    CHECK(workspace.RunCommand("actor.cine_camera"));
    CHECK(document.Entities().size() == before + 2);
    if (!document.Selection().empty()) {
        const Entity cine = document.Selection().front();
        CHECK(document.GetWorld().HasComponent<Camera>(cine));
        CHECK(document.GetWorld().HasComponent<CineCamera>(cine));
    }

    CHECK(workspace.RunCommand("actor.empty"));
    CHECK(document.Entities().size() == before + 3);
    if (!document.Selection().empty()) {
        const Entity empty = document.Selection().front();
        CHECK(!document.GetWorld().HasComponent<Camera>(empty));
    }

    // Each creation was recorded, so undoing once removes the last one.
    CHECK(workspace.RunCommand("edit.undo"));
    CHECK(document.Entities().size() == before + 2);
}

// Both menus are registry entries with no chords, so they cannot collide with a keyboard shortcut.
AETHER_TEST(Menus_AssetAndActorCommandsAreRegisteredUnderTheirMenus) {
    EditorWorkspace workspace;
    const CommandRegistry& commands = workspace.Commands();
    for (const char* id : {"actor.empty", "actor.camera", "actor.cine_camera"}) {
        const Command* command = commands.Find(id);
        CHECK(command != nullptr);
        if (command == nullptr) continue;
        CHECK(command->menu == "Actor");
        CHECK(!command->chord.IsSet());
        CHECK(commands.Enabled(*command));
    }
    for (const char* id : {"asset.browser", "asset.refresh", "asset.validate"}) {
        const Command* command = commands.Find(id);
        CHECK(command != nullptr);
        if (command == nullptr) continue;
        CHECK(command->menu == "Asset");
        CHECK(!command->chord.IsSet());
        CHECK(commands.Enabled(*command));
    }
    CHECK(commands.Conflicts().empty());
}

// The Asset commands put the Content Browser on screen: that is where Refresh and Validate act.
AETHER_TEST(Menus_AssetCommandsFocusTheContentBrowser) {
    HeadlessImGui imgui;
    EditorWorkspace workspace;
    const i64 browser = workspace.FindTool("Content Browser");
    CHECK(browser >= 0);
    if (browser < 0) return;

    CHECK(workspace.RunCommand("asset.browser"));
    CHECK(workspace.IsWindowOpen(static_cast<usize>(browser)));
    CHECK(workspace.Selected() == static_cast<usize>(browser));

    workspace.SetWindowOpen(static_cast<usize>(browser), false);
    CHECK(workspace.RunCommand("asset.validate"));
    CHECK(workspace.IsWindowOpen(static_cast<usize>(browser)));

    workspace.SetWindowOpen(static_cast<usize>(browser), false);
    CHECK(workspace.RunCommand("asset.refresh")); // refreshing does not need the window, but must not fail
    CHECK(workspace.Commands().Find("asset.refresh") != nullptr);
}

// The shell draws both menus from the table; drawing them needs no host state of its own.
AETHER_TEST(Menus_DrawFromTheTableOverAHeadlessFrame) {
    HeadlessImGui imgui;
    EditorWorkspace workspace;
    ImGui::NewFrame();
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("Asset")) {
            workspace.DrawMenuItems("Asset");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Actor")) {
            workspace.DrawMenuItems("Actor");
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
    ImGui::Render();
    CHECK(workspace.Commands().Find("actor.empty") != nullptr);
}
