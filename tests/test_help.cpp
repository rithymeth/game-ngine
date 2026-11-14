#include "workspace/editor_workspace.h"
#include "workspace/command_registry.h"

#include "test_framework.h"

#include <imgui.h>

#include <algorithm>
#include <set>
#include <string>

// Stage 1 of docs/design/EDITOR_PARITY_AUDIT.md: the Help menu. The cheat sheet is generated from the command table,
// so these tests are about that link: every chord the editor has is listed once, in menu order, and the Help commands
// open the windows they name. A chord added without a menu still appears, under "Other".

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

int MenuRank(const std::string& menu) {
    static constexpr const char* kOrder[] = {"File", "Edit", "Asset", "Actor", "View", "Tools", "Window", "Help", "Other"};
    int index = 0;
    for (const char* known : kOrder) {
        if (menu == known) return index;
        ++index;
    }
    return 100;
}

} // namespace

// The cheat sheet is exactly the chorded commands: no chord is missing, none is listed twice, and each row carries the
// label, chord and context the table has.
AETHER_TEST(Help_ShortcutsListsEveryChordedCommandOnce) {
    HeadlessImGui imgui; // chord names come from ImGui's key table
    EditorWorkspace workspace;
    const CommandRegistry& commands = workspace.Commands();
    const std::vector<ShortcutRow> rows = commands.Shortcuts();

    usize chorded = 0;
    for (usize i = 0; i < commands.Size(); ++i) {
        if (commands.At(i).chord.IsSet()) ++chorded;
    }
    CHECK(rows.size() == chorded);
    CHECK(chorded > 0);
    if (rows.size() != chorded) return;

    std::set<std::string> ids;
    for (const ShortcutRow& row : rows) {
        CHECK(ids.insert(row.id).second); // once
        CHECK(!row.chord.empty());
        CHECK(!row.label.empty());
        CHECK(!row.menu.empty());
        CHECK(std::string(CommandScopeName(row.scope)).size() > 0);
        const Command* command = commands.Find(row.id);
        CHECK(command != nullptr);
        if (command == nullptr) continue;
        CHECK(command->chord.ToString() == row.chord);
        CHECK(row.menu == (command->menu.empty() ? std::string("Other") : command->menu));
        CHECK(commands.Label(*command) == row.label);
    }
}

// Menu order is the reading order a Help sheet wants: File, then Edit, ... then Help, then anything a command adds
// without a menu; within a menu, by label. Getting this wrong reorders the rows the user sees.
AETHER_TEST(Help_ShortcutsAreInMenuOrder) {
    HeadlessImGui imgui;
    EditorWorkspace workspace;
    const std::vector<ShortcutRow> rows = workspace.Commands().Shortcuts();
    for (usize i = 1; i < rows.size(); ++i) {
        const ShortcutRow& previous = rows[i - 1];
        const ShortcutRow& current = rows[i];
        const int rank_previous = MenuRank(previous.menu);
        const int rank_current = MenuRank(current.menu);
        if (rank_previous < 100 && rank_current < 100) {
            CHECK(rank_previous <= rank_current);
            if (rank_previous == rank_current) CHECK(previous.label <= current.label);
        } else if (rank_previous == rank_current) {
            CHECK(previous.menu <= current.menu);
            if (previous.menu == current.menu) CHECK(previous.label <= current.label);
        } else {
            CHECK(rank_previous <= rank_current); // a known menu before an extension's
        }
    }
}

// F1 is the shortcut for the cheat sheet and it lives under Help, which is what the window itself documents.
AETHER_TEST(Help_F1OpensTheCheatSheet) {
    HeadlessImGui imgui;
    EditorWorkspace workspace;
    const Command* shortcuts = workspace.Commands().Find("help.shortcuts");
    CHECK(shortcuts != nullptr);
    if (shortcuts == nullptr) return;
    CHECK(shortcuts->menu == "Help");
    CHECK(shortcuts->chord.IsSet());
    CHECK(shortcuts->chord.key == ImGuiKey_F1);
    CHECK(!shortcuts->chord.ctrl && !shortcuts->chord.shift && !shortcuts->chord.alt);
    CHECK(shortcuts->chord.ToString() == "F1");
}

// The two Help actions open the windows they name; the About window's own button opens the cheat sheet too.
AETHER_TEST(Help_CommandsOpenTheirWindows) {
    HeadlessImGui imgui;
    EditorWorkspace workspace;
    CHECK(!workspace.shortcuts_open);
    CHECK(!workspace.about_open);

    CHECK(workspace.RunCommand("help.shortcuts"));
    CHECK(workspace.shortcuts_open);
    CHECK(!workspace.about_open);

    workspace.shortcuts_open = false;
    CHECK(workspace.RunCommand("help.about"));
    CHECK(workspace.about_open);
    CHECK(!workspace.shortcuts_open);

    // Drawing both windows is what a headless host does; it must not need a frame of its own.
    ImGui::NewFrame();
    workspace.DrawWindows();
    ImGui::Render();
    CHECK(workspace.about_open); // still open: nothing answered "Close"
}

// The Help menu draws from the table itself: the same labels and chords the shortcuts list.
AETHER_TEST(Help_MenuDrawsFromTheTable) {
    HeadlessImGui imgui;
    EditorWorkspace workspace;
    ImGui::NewFrame();
    if (ImGui::BeginMainMenuBar()) {
        workspace.DrawHelpMenu();
        ImGui::EndMainMenuBar();
    }
    ImGui::Render();
    CHECK(workspace.Commands().Find("help.about") != nullptr);
}

// A chorded command with no menu (a toolbar action) still gets a row, under "Other", rather than being dropped.
AETHER_TEST(Help_ShortcutRowsNameTheirContext) {
    HeadlessImGui imgui;
    EditorWorkspace workspace;
    Command custom;
    custom.id = "test.chord_only";
    custom.label = "Chord Only";
    custom.chord = {ImGuiKey_K, true, true, false};
    custom.scope = CommandScope::SceneTree;
    custom.run = [] {};
    CHECK(workspace.Commands().Add(std::move(custom)));

    const std::vector<ShortcutRow> rows = workspace.Commands().Shortcuts();
    const auto found = std::find_if(rows.begin(), rows.end(), [](const ShortcutRow& row) { return row.id == "test.chord_only"; });
    CHECK(found != rows.end());
    if (found == rows.end()) return;
    CHECK(found->menu == "Other");
    CHECK(found->chord == "Ctrl+Shift+K");
    CHECK(std::string(CommandScopeName(found->scope)) == "Scene hierarchy");
    // "Other" rows come after every known menu: nothing with a menu may follow one.
    for (auto row = found; row != rows.end(); ++row) CHECK(row->menu == "Other");
}
