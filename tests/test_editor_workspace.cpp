#include "test_framework.h"
#include "aether/assets/asset_database.h"
#include "core/editor_document.h"
#include "core/editor_settings.h"
#include "core/notifications.h"
#include "workspace/command_palette.h"
#include "workspace/command_registry.h"
#include "workspace/editor_workspace.h"
#include "world/scene_panel.h"
#include "world/scene_view_panel.h"
#include "world/scene_view_shading.h"
#include "aether/scene/entity_guid.h"

#include <imgui.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <string>

// The editor workspace: every tool editor on a sample document, as the
// Windows editor and the portable shell host it, drawn headless.

using namespace aether;
using namespace aether::editor;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigMacOSXBehaviors = false;
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1600, 960);
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

} // namespace

namespace {

// Points the autosave folder at a scratch directory for one test, and puts the real one back after.
struct ScopedAutosaveFolder {
    std::string previous;
    bool had_previous = false;
    explicit ScopedAutosaveFolder(const std::filesystem::path& folder) {
        if (const char* old = std::getenv("AETHER_EDITOR_AUTOSAVE_DIR")) {
            previous = old;
            had_previous = true;
        }
#if defined(_WIN32)
        _putenv_s("AETHER_EDITOR_AUTOSAVE_DIR", folder.string().c_str());
#else
        setenv("AETHER_EDITOR_AUTOSAVE_DIR", folder.string().c_str(), 1);
#endif
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
    }
    ~ScopedAutosaveFolder() {
#if defined(_WIN32)
        _putenv_s("AETHER_EDITOR_AUTOSAVE_DIR", had_previous ? previous.c_str() : "");
#else
        if (had_previous) setenv("AETHER_EDITOR_AUTOSAVE_DIR", previous.c_str(), 1);
        else unsetenv("AETHER_EDITOR_AUTOSAVE_DIR");
#endif
    }
};

} // namespace

// Stage 0 of the editor parity audit: quitting asks when -- and only when -- there is something to lose.
AETHER_TEST(EditorWorkspace_QuitAsksOnlyWhenThereIsSomethingToLose) {
    const ScopedAutosaveFolder folder(std::filesystem::temp_directory_path() / "aether_workspace_quit");
    EditorWorkspace ws;
    int confirmed = 0;
    ws.on_quit_confirmed = [&] { ++confirmed; };
    CHECK(!ws.HasUnsavedChanges());
    CHECK(std::string(ws.ActiveDocument().Kind()) == "scene");
    CHECK(ws.ActiveDocument().DisplayName() == "Untitled");
    ws.RequestQuit();
    CHECK(confirmed == 1); // nothing unsaved: the host may close at once
    CHECK(!ws.QuitPromptOpen());

    ws.Scene().Document().CreateEntity("unsaved");
    CHECK(ws.ActiveDocument().IsDirty()); // the prompt asks the contract, not the scene panel
    ws.RequestQuit();
    CHECK(confirmed == 1); // asked, not confirmed
    CHECK(ws.QuitPromptOpen());
    ws.CancelQuit();
    CHECK(!ws.QuitPromptOpen());
    CHECK(confirmed == 1);
    CHECK(ws.HasUnsavedChanges()); // canceling keeps the work

    ws.RequestQuit();
    ws.QuitWithoutSaving();
    CHECK(confirmed == 2);
    CHECK(!ws.QuitPromptOpen());
}

// The autosave timer writes while the scene has unsaved changes, and the recovery copy goes away with them.
AETHER_TEST(EditorWorkspace_AutosaveFollowsTheUnsavedChanges) {
    const std::filesystem::path base = std::filesystem::temp_directory_path() / "aether_workspace_autosave";
    const ScopedAutosaveFolder folder(base);
    EditorWorkspace ws;
    ws.Settings().autosave_enabled = true;
    ws.Settings().autosave_interval_seconds = EditorSettings::kMinAutosaveSeconds;
    CHECK(!ws.HasRecoverableAutosave());

    ws.Scene().Document().CreateEntity("unsaved");
    ws.Update(EditorSettings::kMinAutosaveSeconds * 0.5f);
    CHECK(!ws.HasRecoverableAutosave()); // not due yet
    ws.Update(EditorSettings::kMinAutosaveSeconds * 0.5f + 1.0f);
    CHECK(ws.HasRecoverableAutosave());

    CHECK(ws.Scene().Document().Undo()); // back to the last saved state
    CHECK(!ws.HasUnsavedChanges());
    ws.Update(0.0f);
    CHECK(!ws.HasRecoverableAutosave());
}

// A session that ends without saving leaves a slot; the next launch recovers it, and the scene file is untouched.
AETHER_TEST(EditorWorkspace_RecoveryBringsBackAnInterruptedSessionsWork) {
    const std::filesystem::path base = std::filesystem::temp_directory_path() / "aether_workspace_recovery";
    const ScopedAutosaveFolder folder(base);
    std::filesystem::path project;
    {
        EditorWorkspace first;
        project = first.ProjectFile();
        first.Scene().Document().CreateEntity("recovered marker");
        CHECK(first.AutosaveNow());
        CHECK(first.HasRecoverableAutosave());
    } // destroyed without saving: what a crash leaves behind
    EditorWorkspace second;
    CHECK(second.ProjectFile() == project);
    CHECK(second.HasRecoverableAutosave());
    const usize before = second.Scene().Document().Entities().size();
    CHECK(second.RecoverAutosave());
    CHECK(second.Scene().Document().Entities().size() == before + 1);
    CHECK(second.Scene().Document().IsDirty());
    CHECK(second.HasUnsavedChanges());
    second.DiscardAutosave();
    CHECK(!second.HasRecoverableAutosave());
}

// The Messages tool lists what the notification service holds and marks it read; the toast stack reads
// the same service and expires with the workspace clock.
AETHER_TEST(EditorWorkspace_NotificationsReachTheMessagesTool) {
    HeadlessImGui imgui;
    EditorWorkspace ws;
    const i64 messages = ws.FindTool("Messages");
    CHECK(messages >= 0);
    ws.Notifications().Post(NotificationSeverity::Warning, "Heads up", "something happened");
    CHECK(ws.Notifications().UnreadCount() == 1);
    imgui.Frame([&] { ws.DrawTool(static_cast<usize>(messages)); });
    CHECK(ws.Notifications().UnreadCount() == 0);

    const u64 info = ws.Notifications().Post(NotificationSeverity::Info, "Saved", "ok");
    imgui.Frame([&] { ws.DrawWindows(); }); // the toasts are drawn here
    CHECK(ws.Notifications().IsToastShowing(info));
    ws.Update(5.0f); // past the 4 s an info toast stays
    CHECK(!ws.Notifications().IsToastShowing(info));
}

AETHER_TEST(EditorWorkspace_HasEveryTool) {
    HeadlessImGui imgui;
    EditorWorkspace ws;
    CHECK(ws.ToolCount() >= 19);
    std::set<std::string> names, categories;
    for (usize i = 0; i < ws.ToolCount(); ++i) {
        names.insert(ws.ToolName(i));
        categories.insert(ws.ToolCategory(i));
    }
    CHECK(names.size() == ws.ToolCount()); // unique: they're window titles
    for (const char* c : {"Scripting", "Rendering", "Animation", "AI", "Audio", "UI", "World", "Networking", "Debug"}) {
        CHECK(categories.count(c) == 1);
    }
    for (const char* t : {"Blueprint", "Luau", "Material", "Particle", "Animation", "Behavior", "Navigation", "Sound Cue",
                          "Mixer", "UI Designer", "Terrain", "Foliage", "Spline", "World Partition", "Net Play",
                          "Net Profiler", "Console", "Profiler", "Crash", "Sequencer", "Messages"}) {
        CHECK(ws.FindTool(t) >= 0);
    }
    // Every tool says what it is for: the hub must not be a list of bare names.
    for (usize i = 0; i < ws.ToolCount(); ++i) {
        const std::string description = ws.ToolDescription(i);
        CHECK(description.size() >= 20);
        CHECK(description != ws.ToolName(i));
        CHECK(description.find(ws.ToolCategory(i)) == std::string::npos); // a description, not the category again
    }
    CHECK(ws.FindTool("material") == ws.FindTool("Material - M_Lit")); // any case, a prefix
    CHECK(ws.FindTool("Gizmo") == -1 && ws.FindTool("") == -1);
}

// The tool list and the open tool are two panels a host docks apart; drawn headless they still select and draw.
AETHER_TEST(EditorWorkspace_ToolListAndOpenToolDrawApart) {
    HeadlessImGui imgui;
    EditorWorkspace ws;
    const i64 terrain = ws.FindTool("Terrain");
    CHECK(terrain >= 0);
    ws.Select(static_cast<usize>(terrain));
    for (int frame = 0; frame < 2; ++frame) {
        imgui.Frame([&] {
            ImGui::SetNextWindowSize(ImVec2(300.0f, 900.0f), ImGuiCond_Always);
            ImGui::Begin("Tools");
            ws.DrawToolList();
            ImGui::End();
            ImGui::SetNextWindowSize(ImVec2(1000.0f, 900.0f), ImGuiCond_Always);
            ImGui::Begin("Editor");
            ws.DrawSelectedTool();
            ImGui::End();
        });
    }
    CHECK(ws.Selected() == static_cast<usize>(terrain));
    CHECK(std::string(ws.ToolDescription(static_cast<usize>(terrain))).find("landscape") != std::string::npos);
}

// The workspaces across the top, as the concept draws them: one tab per workspace with its tool, laid out side by side,
// and a click on a tab opens that workspace's tool.
AETHER_TEST(EditorWorkspace_WorkspaceStripOpensEachTool) {
    HeadlessImGui imgui;
    EditorWorkspace ws;
    ImGuiIO& io = ImGui::GetIO();
    auto frame = [&] {
        imgui.Frame([&] {
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(1400.0f, 40.0f));
            ImGui::Begin("strip", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);
            ws.DrawWorkspaceStrip();
            ImGui::End();
        });
    };
    frame();
    const std::vector<EditorWorkspace::WorkspaceTab> tabs = ws.WorkspaceTabs();
    CHECK(tabs.size() == 11);
    CHECK(tabs.front().label == "World" && tabs.back().label == "Profiler");
    usize blueprint = tabs.size();
    for (usize i = 0; i < tabs.size(); ++i) {
        CHECK(ws.FindTool(tabs[i].tool) >= 0); // every tab opens a tool that exists
        CHECK(tabs[i].max_x > tabs[i].min_x && tabs[i].max_y > tabs[i].min_y);
        if (i > 0) CHECK(tabs[i].min_x > tabs[i - 1].max_x); // side by side, left to right, with no overlap
        if (tabs[i].label == "Blueprint") blueprint = i;
    }
    CHECK(blueprint < tabs.size());

    const usize before = ws.Selected();
    CHECK(std::string(ws.ToolName(before)) == "Scene"); // a project opens on the level editor, as the concept draws it
    CHECK(!ws.SelectWorkspace("Gizmo")); // not a workspace: nothing changes
    CHECK(ws.Selected() == before);
    CHECK(ws.SelectWorkspace("Blueprint"));
    CHECK(ws.ActiveWorkspace() == "Blueprint");
    CHECK(std::string(ws.ToolName(ws.Selected())).rfind("Blueprint", 0) == 0);
    // The active workspace is read from the open tool, not remembered separately, and World is the level editor.
    ws.Select(static_cast<usize>(ws.FindTool("Terrain")));
    CHECK(ws.ActiveWorkspace().empty());
    CHECK(ws.SelectWorkspace("World"));
    CHECK(ws.ActiveWorkspace() == "World");
    CHECK(std::string(ws.ToolName(ws.Selected())) == "Scene");

    // And a click on the tab switches, which is how a person does it.
    frame();
    const EditorWorkspace::WorkspaceTab target = ws.WorkspaceTabs()[blueprint];
    const ImVec2 at((target.min_x + target.max_x) * 0.5f, (target.min_y + target.max_y) * 0.5f);
    auto step = [&](const ImVec2& mouse, bool down) {
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
        frame();
    };
    step(ImVec2(2.0f, 2.0f), false);
    step(at, false);
    step(at, true);
    step(at, false);
    CHECK(ws.ActiveWorkspace() == "Blueprint");
}

AETHER_TEST(EditorWorkspace_WorkspaceStripPlaysAndStopsTheScene) {
    HeadlessImGui imgui;
    EditorWorkspace ws;
    ImGuiIO& io = ImGui::GetIO();
    auto frame = [&] {
        imgui.Frame([&] {
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(1400.0f, 40.0f));
            ImGui::Begin("strip", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);
            ws.DrawWorkspaceStrip("Vulkan");
            ImGui::End();
        });
    };
    auto move = [&](const ImVec2& mouse, bool down) {
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
        frame();
    };
    enum Chip { kMain, kStop, kSimulate };
    auto click = [&](const EditorWorkspace::StripPlay& rect, Chip chip) {
        const f32 min_x = chip == kMain ? rect.main_min_x : (chip == kStop ? rect.stop_min_x : rect.simulate_min_x);
        const f32 max_x = chip == kMain ? rect.main_max_x : (chip == kStop ? rect.stop_max_x : rect.simulate_max_x);
        const f32 min_y = chip == kMain ? rect.main_min_y : (chip == kStop ? rect.stop_min_y : rect.simulate_min_y);
        const f32 max_y = chip == kMain ? rect.main_max_y : (chip == kStop ? rect.stop_max_y : rect.simulate_max_y);
        const ImVec2 at((min_x + max_x) * 0.5f, (min_y + max_y) * 0.5f);
        move(ImVec2(2.0f, 2.0f), false);
        move(at, false);
        move(at, true);
        move(at, false);
    };
    frame();
    const EditorWorkspace::StripPlay editing = ws.WorkspaceStripPlays();
    CHECK(editing.visible);
    CHECK(editing.main_max_x > editing.main_min_x); // the concept's green Play chip
    CHECK(editing.main_min_x > ws.WorkspaceTabs().back().max_x); // at the strip's right end, not among the tabs
    CHECK(editing.simulate_max_x > editing.main_max_x); // Simulate sits beside Play while editing
    CHECK(editing.step_max_x == 0.0f && editing.stop_max_x == 0.0f); // nothing to step or stop while editing

    click(editing, kSimulate);
    CHECK(ws.Scene().Document().PlayState() == PlaySession::State::Playing);
    CHECK(ws.Scene().Document().IsSimulating()); // the game runs, the view's camera stays the person's
    frame();
    const EditorWorkspace::StripPlay simulating = ws.WorkspaceStripPlays();
    CHECK(simulating.visible);
    CHECK(simulating.simulate_max_x == 0.0f); // Simulate shows only while editing
    CHECK(simulating.step_max_x > simulating.main_max_x); // Pause, Step, Stop, left to right
    CHECK(simulating.stop_min_x > simulating.step_max_x);

    click(simulating, kMain); // Pause
    CHECK(ws.Scene().Document().PlayState() == PlaySession::State::Paused);
    CHECK(ws.Scene().Document().IsSimulating());
    click(ws.WorkspaceStripPlays(), kStop); // Stop
    CHECK(ws.Scene().Document().PlayState() == PlaySession::State::Editing && !ws.Scene().Document().IsSimulating());

    frame();
    const EditorWorkspace::StripPlay again = ws.WorkspaceStripPlays();
    CHECK(again.simulate_max_x > again.main_max_x);
    click(again, kMain); // an ordinary Play afterwards
    CHECK(ws.Scene().Document().PlayState() == PlaySession::State::Playing && !ws.Scene().Document().IsSimulating());
    click(ws.WorkspaceStripPlays(), kStop);
    CHECK(ws.Scene().Document().PlayState() == PlaySession::State::Editing);
}

AETHER_TEST(EditorWorkspace_RuntimeErrorsReachMessagesWithTheEntity) {
    EditorWorkspace ws;
    SceneDocument& document = ws.Scene().Document();
    const Entity door = document.CreateEntity("Front Door");
    const EntityGuid guid = document.GetWorld().GetComponent<IdComponent>(door)->guid;
    struct StubShading final : SceneViewShading {
        bool Ready() const override { return true; }
        const std::string& Note() const override { return note; }
        bool Advance(SceneDocument&, const SceneViewCamera&, u32, u32, SceneViewMode, f32) override { return true; }
        const ImageTexture& Picture() const override { return texture; }
        std::vector<RuntimeError> Errors() const override { return errors; }
        void Stop() override {}
        ImageTexture texture;
        std::string note;
        std::vector<RuntimeError> errors;
    };
    StubShading shading;
    ws.SceneView().shading = &shading;
    const Notification* row = nullptr;
    const auto error_rows = [&] {        std::vector<const Notification*> rows;
        for (const Notification& n : ws.Notifications().All()) {
            if (n.severity == NotificationSeverity::Error) rows.push_back(&n);
        }
        return rows;
    };

    document.Play();
    shading.errors = {{"Broken.luau:3: attempt to index nil with 'boom' (in OnUpdate, on entity \"Front Door\" " + ToString(guid) + ")",
                       "OnUpdate", ToString(guid), "Front Door"}};
    ws.Update(1.0f / 60.0f);
    CHECK(error_rows().size() == 1);
    if (error_rows().size() == 1) {
        row = error_rows().front();
        CHECK(row->title == "Error in OnUpdate");
        CHECK(row->message.find("Broken.luau:3") != std::string::npos);
        CHECK(row->target.kind == NotificationTarget::Kind::Entity);
        CHECK(row->target.value == ToString(guid)); // clicking the row in Messages selects the door
    }

    // The game keeps counting the same error up; Messages keeps the one row.
    shading.errors[0].text += " (repeated 10 times)";
    ws.Update(1.0f / 60.0f);
    CHECK(error_rows().size() == 1);

    // Stop ends the session's list; a new play session starts it again.
    document.Stop();
    ws.Update(1.0f / 60.0f);
    document.Play();
    ws.Update(1.0f / 60.0f);
    CHECK(error_rows().size() == 2);
}

AETHER_TEST(EditorWorkspace_BreadcrumbNamesTheProjectAndTheOpenScene) {
    EditorWorkspace ws;
    const std::string crumb = ws.Breadcrumb();
    CHECK(crumb.rfind("Game / ", 0) == 0);
    CHECK(crumb.size() > 7 + ws.ActiveDocument().DisplayName().size());
    CHECK(crumb.compare(crumb.size() - ws.ActiveDocument().DisplayName().size(), std::string::npos,
                        ws.ActiveDocument().DisplayName()) == 0);
    if (!ws.ProjectFile().empty()) // the default workspace opens the sample project
        CHECK(crumb.find(" / " + ws.ProjectFile().stem().string() + " / ") != std::string::npos);
}

// The concept's menu bar search ("Search commands, assets... Ctrl K"): the box is the palette's query, a click opens
// the palette, and the same text filters it. Closing the palette empties the box.
AETHER_TEST(EditorWorkspace_TheMenuBarSearchOpensThePalette) {
    HeadlessImGui imgui;
    EditorWorkspace ws;
    ImGuiIO& io = ImGui::GetIO();
    auto frame = [&] {
        imgui.Frame([&] {
            if (ImGui::BeginMainMenuBar()) {
                ws.DrawCommandSearch(280.0f);
                ImGui::EndMainMenuBar();
            }
            ws.DrawWindows();
        });
    };
    frame();
    const EditorWorkspace::CommandSearchBox box = ws.LastCommandSearch();
    CHECK(box.visible);
    CHECK(box.max_x > box.min_x && box.max_y > box.min_y);
    CHECK(!ws.command_palette_open && ws.PaletteQuery().empty());

    auto step = [&](const ImVec2& mouse, bool down) {
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
        frame();
    };
    const ImVec2 at((box.min_x + box.max_x) * 0.5f, (box.min_y + box.max_y) * 0.5f);
    step(ImVec2(2.0f, 2.0f), false);
    step(at, false);
    step(at, true);
    step(at, false);
    CHECK(ws.command_palette_open); // a click on the box is a door to the palette

    io.AddInputCharactersUTF8("anim"); // the focused box feeds the palette's query
    frame();
    CHECK(ws.PaletteQuery() == "anim");

    ws.command_palette_open = false;
    frame();
    CHECK(ws.PaletteQuery().empty()); // one text: closing the palette leaves nothing behind in the box
}

// Ctrl+K opens the palette with the keyboard already in its search field, so the first character typed is a search.
AETHER_TEST(EditorWorkspace_OpeningThePalettePutsTheKeyboardInTheSearchBox) {
    HeadlessImGui imgui;
    EditorWorkspace ws;
    ImGuiIO& io = ImGui::GetIO();
    auto frame = [&] {
        imgui.Frame([&] {
            if (ImGui::BeginMainMenuBar()) {
                ws.DrawCommandSearch(280.0f);
                ImGui::EndMainMenuBar();
            }
            ws.DrawWindows();
        });
    };
    frame();
    ws.OpenCommandPalette();
    frame();
    frame(); // the box takes the keyboard on the frame after the request
    io.AddInputCharactersUTF8("save");
    frame();
    CHECK(ws.command_palette_open && ws.PaletteQuery() == "save");
}

// The concept's first row names the project and its save state beside the mark, and puts the user's initials in a
// circle at the far right. The chip reads the active document, so the first unsaved edit changes its words.
AETHER_TEST(EditorWorkspace_TheMenuBarNamesTheProjectAndItsSaveState) {
    HeadlessImGui imgui;
    EditorWorkspace ws;
    auto frame = [&] {
        imgui.Frame([&] {
            if (ImGui::BeginMainMenuBar()) {
                ws.DrawProjectChip();
                ImGui::SameLine();
                ws.DrawUserAvatar();
                ImGui::EndMainMenuBar();
            }
        });
    };
    frame();
    const EditorWorkspace::MenuChip chip = ws.LastProjectChip();
    const EditorWorkspace::MenuChip avatar = ws.LastAvatar();
    CHECK(chip.visible && avatar.visible);
    CHECK(chip.max_x > chip.min_x && chip.max_y > chip.min_y);
    CHECK(avatar.max_x > chip.max_x && avatar.max_y > chip.min_y); // the avatar sits to the chip's right
    if (!ws.ProjectFile().empty()) // the default workspace opens the sample project
        CHECK(ws.ProjectChipLabel().rfind(ws.ProjectFile().stem().string() + " · ", 0) == 0);
    CHECK(ws.ProjectChipLabel().find("All changes saved") != std::string::npos);
    CHECK(!ws.ActiveDocument().IsDirty());

    ws.Scene().Document().CreateEntity("Chip Test");
    CHECK(ws.ActiveDocument().IsDirty());
    frame();
    CHECK(ws.ProjectChipLabel().find("Unsaved changes") != std::string::npos);

    ws.SetUserInitials("rm");
    CHECK(ws.UserInitials() == "RM"); // the circle draws capitals whatever a host passes
}

AETHER_TEST(EditorWorkspace_DrawsEveryToolHeadless) {
    HeadlessImGui imgui;
    EditorWorkspace ws;
    // Each tool in the hub, a couple of frames apiece.
    for (usize i = 0; i < ws.ToolCount(); ++i) {
        ws.Select(i);
        CHECK(ws.Selected() == i);
        for (int f = 0; f < 2; ++f) {
            imgui.Frame([&] {
                ws.Update(1.0f / 60.0f);
                ws.DrawHub();
                ws.DrawWindows();
            });
        }
    }
    // Every tool popped out at once, with the menu bar's Tools menu.
    for (usize i = 0; i < ws.ToolCount(); ++i) ws.SetWindowOpen(i, true);
    for (int f = 0; f < 3; ++f) {
        imgui.Frame([&] {
            if (ImGui::BeginMainMenuBar()) {
                ws.DrawToolsMenu();
                ImGui::EndMainMenuBar();
            }
            ws.DrawHub();
            ws.DrawWindows();
        });
    }
    CHECK(ws.IsWindowOpen(0));
    ws.SetWindowOpen(0, false);
    CHECK(!ws.IsWindowOpen(0));
    ws.Select(ws.ToolCount() + 5); // out of range: ignored
    CHECK(ws.Selected() < ws.ToolCount());
}

// Phase 43.5: the command palette's matching half. Keeping it out of the drawing code is what
// makes it checkable -- what a query lists, and what choosing a result does.
AETHER_TEST(Editor_CommandPaletteFiltersAndSelectsTools) {
    EditorWorkspace ws;
    CommandPalette palette(ws);

    // An empty query lists every tool in workspace order, then every command in registry order, so the
    // palette doubles as an index.
    CHECK(palette.Results().size() == ws.ToolCount() + ws.Commands().Size());
    for (std::size_t i = 0; i < ws.ToolCount(); ++i) {
        CHECK(palette.Results()[i].kind == PaletteKind::Tool);
        CHECK(palette.Results()[i].tool == i);
    }
    for (std::size_t i = ws.ToolCount(); i < palette.Results().size(); ++i) {
        CHECK(palette.Results()[i].kind == PaletteKind::Command && !palette.Results()[i].command.empty());
    }

    // Matching is case-insensitive and covers the category as well as the name.
    const std::string first = ws.ToolName(0);
    std::string upper = first;
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    palette.SetQuery(upper);
    CHECK(!palette.Results().empty() && palette.Results()[0].tool == 0);

    palette.SetQuery("this matches no tool at all");
    CHECK(palette.Results().empty());
    CHECK(!palette.ActivateFirst(ws)); // nothing to choose

    palette.SetQuery("");

    // Choosing a result selects that tool; an out-of-range choice does nothing.
    const std::size_t target = ws.ToolCount() - 1;
    CHECK(palette.Activate(ws, target));
    CHECK(ws.Selected() == palette.Results()[target].tool);
    CHECK(!palette.Activate(ws, palette.Results().size()));

    // And the palette draws: opening the window adds geometry to the frame. (A vertex count
    // cannot say the list is right, only that the window rendered.)
    HeadlessImGui imgui;
    ws.command_palette_open = false;
    imgui.Frame([&] { ws.DrawWindows(); });
    const int closed = ImGui::GetDrawData()->TotalVtxCount;
    ws.command_palette_open = true;
    imgui.Frame([&] { ws.DrawWindows(); });
    CHECK(ImGui::GetDrawData()->TotalVtxCount > closed);
}

AETHER_TEST(Editor_CommandPaletteClosesAfterChoosingATool) {
    HeadlessImGui imgui;
    EditorWorkspace ws;
    CHECK(ws.ToolCount() > 1);
    if (ws.ToolCount() <= 1) return;
    ws.Select(1);
    ws.command_palette_open = true;
    ImVec2 first_result;
    const auto frame = [&] {
        imgui.Frame([&] {
            const ImVec2 pos(40, 50);
            const ImGuiStyle& style = ImGui::GetStyle();
            // The first tool is below the title bar and search field.
            first_result = ImVec2(pos.x + style.WindowPadding.x + 12,
                                 pos.y + ImGui::GetFrameHeight() + style.WindowPadding.y +
                                     ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeight() * 0.5f);
            ImGui::SetNextWindowPos(pos);
            ws.DrawCommandPalette();
        });
    };
    frame();
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(first_result.x, first_result.y);
    frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame();
    CHECK(ws.Selected() == 0);
    CHECK(!ws.command_palette_open);
}

// Phase 43.5: the asset half of the palette. It lists matching assets and hands the chosen
// path to the host's opener -- what "opening" an asset means is the host's decision, so the
// palette reports rather than acts.
AETHER_TEST(Editor_CommandPaletteSearchesAssetsAndHandsThemToTheHost) {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "aether_test_palette_assets";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "Textures");
    { std::ofstream file(root / "Textures/brick.png"); file << "fake png"; }
    assets::AssetDatabase assets(root);
    CHECK(assets.Scan().assets == 1);

    EditorWorkspace ws;
    CommandPalette palette(ws, &assets);
    std::string opened;
    palette.SetAssetOpener([&](const std::string& path) { opened = path; });

    palette.SetQuery("brick"); // matches only the asset, not any tool name
    CHECK(palette.Results().size() == 1);
    CHECK(palette.Results()[0].kind == PaletteKind::Asset && palette.Results()[0].path == "Textures/brick.png");
    CHECK(palette.ActivateFirst(ws));
    CHECK(opened == "Textures/brick.png");
    CHECK(ws.Selected() < ws.ToolCount()); // activating an asset does not change the selected tool

    // Without an opener an asset entry cannot be activated: nothing silently succeeds.
    CommandPalette no_opener(ws, &assets);
    no_opener.SetQuery("brick");
    CHECK(!no_opener.ActivateFirst(ws));

    std::filesystem::remove_all(root);
}
