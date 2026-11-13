#include "test_framework.h"
#include "core/scene_document.h"
#include "workspace/command_registry.h"
#include "workspace/editor_workspace.h"
#include "world/scene_panel.h"

#include <imgui.h>

#include <functional>
#include <string>
#include <vector>

using namespace aether;
using namespace aether::editor;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGui::GetIO().ConfigMacOSXBehaviors = false; // tests press Ctrl on every platform
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1400, 900);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(context_); }
    void Frame(const std::function<void()>& body) {
        ImGui::NewFrame();
        body();
        ImGui::Render();
    }

private:
    ImGuiContext* context_ = nullptr;
};

Command Make(const char* id, CommandChord chord, CommandScope scope) {
    Command command;
    command.id = id;
    command.label = id;
    command.chord = chord;
    command.scope = scope;
    return command;
}

} // namespace

// A command is found by its stable id, refused when the id is taken, and runs only when it is enabled.
AETHER_TEST(CommandRegistry_AddsFindsAndRuns) {
    CommandRegistry registry;
    int runs = 0;
    bool allowed = true;

    Command run = Make("test.run", {}, CommandScope::Global);
    run.run = [&] { ++runs; };
    CHECK(registry.Add(std::move(run)));

    Command gated = Make("test.gated", {}, CommandScope::Global);
    gated.enabled = [&] { return allowed; };
    gated.run = [&] { ++runs; };
    CHECK(registry.Add(std::move(gated)));
    CHECK(registry.Size() == 2);

    CHECK(registry.Find("test.run") != nullptr);
    CHECK(registry.Find("test.nope") == nullptr);
    CHECK(registry.Run("test.run") && runs == 1);
    CHECK(!registry.Run("test.nope"));

    allowed = false;
    CHECK(!registry.Run("test.gated")); // disabled: nothing runs
    allowed = true;
    CHECK(registry.Run("test.gated") && runs == 2);

    // A duplicate or empty id is refused and changes nothing, so a plugin cannot quietly replace a built-in.
    Command duplicate = Make("test.run", {}, CommandScope::Global);
    duplicate.run = [&] { ++runs; };
    CHECK(!registry.Add(std::move(duplicate)));
    CHECK(!registry.Add(Make("", {}, CommandScope::Global)));
    CHECK(registry.Size() == 2 && runs == 2);

    // A dynamic label is what the menus and the palette show.
    Command dynamic = Make("test.dynamic", {}, CommandScope::Global);
    dynamic.label = "Static";
    dynamic.label_now = [] { return std::string("Dynamic"); };
    CHECK(registry.Add(std::move(dynamic)));
    CHECK(registry.Label(*registry.Find("test.dynamic")) == "Dynamic");
    CHECK(registry.Label(*registry.Find("test.run")) == "test.run");
}

AETHER_TEST(CommandRegistry_ChordsFormatAndMatchExactly) {
    HeadlessImGui imgui; // GetKeyName reads ImGui's key table
    const CommandChord save{ImGuiKey_S, true, false, false};
    CHECK(save.ToString() == "Ctrl+S");
    CHECK((CommandChord{ImGuiKey_P, true, true, false}).ToString() == "Ctrl+Shift+P");
    CHECK((CommandChord{ImGuiKey_Delete, false, false, false}).ToString() == "Delete");
    CHECK(CommandChord{}.ToString().empty());
    CHECK(!CommandChord{}.IsSet());

    CHECK(save.Matches(ImGuiKey_S, true, false, false));
    CHECK(!save.Matches(ImGuiKey_S, true, true, false));  // exact modifiers: Ctrl+Shift+S is not Ctrl+S
    CHECK(!save.Matches(ImGuiKey_S, false, false, false));
    CHECK(!save.Matches(ImGuiKey_Y, true, false, false));
    CHECK(!CommandChord{}.Matches(ImGuiKey_None, false, false, false));
}

// A chord is a conflict only when both commands can be active at once: Global counts as active with everything.
AETHER_TEST(CommandRegistry_ConflictsOnlyWithinReachableContexts) {
    CommandRegistry registry;
    const CommandChord save{ImGuiKey_S, true, false, false};
    CHECK(registry.Add(Make("global.save", save, CommandScope::Global)));
    CHECK(registry.Conflicts().empty()); // one command cannot conflict with itself

    CHECK(registry.Add(Make("scene.save", save, CommandScope::Scene)));
    const std::vector<std::string> conflicts = registry.Conflicts();
    CHECK(conflicts.size() == 1);
    if (conflicts.size() == 1) {
        CHECK(conflicts[0].find("global.save") != std::string::npos && conflicts[0].find("scene.save") != std::string::npos);
        CHECK(conflicts[0].find("Ctrl+S") != std::string::npos);
    }

    // The same chord in two parts of the editor that never both have the keyboard is two bindings, not a clash.
    CommandRegistry split;
    CHECK(split.Add(Make("tree.duplicate", {ImGuiKey_D, true, false, false}, CommandScope::SceneTree)));
    CHECK(split.Add(Make("scene.duplicate", {ImGuiKey_D, true, false, false}, CommandScope::Scene)));
    CHECK(split.Conflicts().empty());

    // Same context, same chord: a real clash. Different modifiers, or no chord at all: not one.
    CommandRegistry same;
    CHECK(same.Add(Make("a", save, CommandScope::Scene)));
    CHECK(same.Add(Make("b", {ImGuiKey_S, true, true, false}, CommandScope::Scene)));
    CHECK(same.Add(Make("c", {}, CommandScope::Scene)));
    CHECK(same.Add(Make("d", {}, CommandScope::Scene)));
    CHECK(same.Conflicts().empty());
    CHECK(same.Add(Make("e", save, CommandScope::Scene)));
    CHECK(same.Conflicts().size() == 1);
}

// The startup check the audit asks for: every editor command has a unique id and no two chords can fire together.
AETHER_TEST(EditorWorkspace_CommandsAreUniqueAndUnconflicted) {
    HeadlessImGui imgui;
    EditorWorkspace ws;
    CHECK(ws.Commands().Size() >= 14);
    CHECK(ws.Commands().Conflicts().empty());
    for (const char* id : {"scene.new", "scene.open", "scene.save", "scene.save_as", "file.quit", "edit.undo", "edit.redo",
                           "edit.duplicate", "edit.delete", "edit.copy", "edit.paste", "scene.rename", "view.command_palette",
                           "view.preferences"}) {
        CHECK(ws.Commands().Find(id) != nullptr);
    }
    // What the host draws in a menu has a menu to be drawn in; a palette-only command has none.
    const Command* save = ws.Commands().Find("scene.save");
    CHECK(save != nullptr && save->menu == "File");
    const Command* rename = ws.Commands().Find("scene.rename");
    CHECK(rename != nullptr && rename->menu.empty());
}

AETHER_TEST(EditorWorkspace_CommandsRunAndMenusDrawFromTheTable) {
    HeadlessImGui imgui;
    EditorWorkspace ws;

    // A fresh scene has nothing to undo, so the command is disabled and does not pretend otherwise.
    CHECK(!ws.RunCommand("edit.undo"));
    const usize before = ws.Scene().Document().Entities().size();
    const Entity thing = ws.Scene().Document().CreateEntity("command target");
    CHECK(ws.RunCommand("edit.undo"));
    CHECK(ws.Scene().Document().Entities().size() == before);
    CHECK(ws.RunCommand("edit.redo"));
    CHECK(ws.Scene().Document().Entities().size() == before + 1);

    CHECK(!ws.RunCommand("no.such.command"));

    // View's commands are the registry's: running one is what choosing the menu item does.
    CHECK(!ws.preferences_open);
    CHECK(ws.RunCommand("view.preferences"));
    CHECK(ws.preferences_open);
    CHECK(!ws.command_palette_open);
    CHECK(ws.RunCommand("view.command_palette"));
    CHECK(ws.command_palette_open);

    // The menus draw from it headless (a vertex count cannot say the items are right, only that they drew).
    imgui.Frame([&] {
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                ws.DrawMenuItems("File");
                ImGui::EndMenu();
            }
            ws.DrawEditMenu();
            if (ImGui::BeginMenu("View")) {
                ws.DrawMenuItems("View");
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }
    });
    CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
}

// The keyboard poller is generated from the same table: Ctrl+Shift+P opens the palette, P alone does not.
AETHER_TEST(EditorWorkspace_GlobalChordsComeFromTheTable) {
    HeadlessImGui imgui;
    EditorWorkspace ws;
    ImGuiIO& io = ImGui::GetIO();
    auto draw = [&] { ws.DrawWindows(); };

    imgui.Frame(draw); // a first frame, so ImGui has processed the display size
    io.AddKeyEvent(ImGuiKey_P, true);
    imgui.Frame(draw);
    io.AddKeyEvent(ImGuiKey_P, false);
    imgui.Frame(draw);
    CHECK(!ws.command_palette_open); // no Ctrl: not the command's chord

    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiMod_Shift, true);
    io.AddKeyEvent(ImGuiKey_P, true);
    imgui.Frame(draw);
    io.AddKeyEvent(ImGuiKey_P, false);
    io.AddKeyEvent(ImGuiMod_Shift, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    imgui.Frame(draw);
    CHECK(ws.command_palette_open);
}

// The Scene tool's own keys come from the table too, and only while it has the keyboard: Ctrl+Z here is
// edit.undo, not a hardcoded check in the panel.
AETHER_TEST(EditorWorkspace_SceneChordsRunThroughTheTable) {
    HeadlessImGui imgui;
    EditorWorkspace ws;
    const i64 scene_tool = ws.FindTool("Scene");
    CHECK(scene_tool >= 0);
    const usize base = ws.Scene().Document().Entities().size();
    ws.Scene().Document().CreateEntity("undo me by key");
    CHECK(ws.Scene().Document().Entities().size() == base + 1);
    CHECK(ws.Scene().Document().CanUndo());

    auto draw = [&] {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1400, 900));
        ImGui::Begin("scene host");
        ws.DrawTool(static_cast<usize>(scene_tool));
        ImGui::End();
    };
    imgui.Frame(draw);
    imgui.Frame(draw); // the first frame settles the focus
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_Z, true);
    imgui.Frame(draw);
    io.AddKeyEvent(ImGuiKey_Z, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    imgui.Frame(draw);
    CHECK(ws.Scene().Document().Entities().size() == base);
}
