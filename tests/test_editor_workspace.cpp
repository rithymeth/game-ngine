#include "test_framework.h"
#include "aether/assets/asset_database.h"
#include "core/editor_document.h"
#include "core/editor_settings.h"
#include "core/notifications.h"
#include "workspace/command_palette.h"
#include "workspace/command_registry.h"
#include "workspace/editor_workspace.h"
#include "world/scene_panel.h"

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
    CHECK(ws.FindTool("material") == ws.FindTool("Material - M_Lit")); // any case, a prefix
    CHECK(ws.FindTool("Gizmo") == -1 && ws.FindTool("") == -1);
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
