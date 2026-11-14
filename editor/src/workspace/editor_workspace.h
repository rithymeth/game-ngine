#pragma once

#include "aether/core/base.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Every tool editor the engine has, in one place: the Blueprint, Material,
// animation, AI, audio, VFX, UI, world, networking, debug and project
// (Project Settings, Build and Package) tools, each on
// a sample document so it opens on something real. The Windows editor
// (D3D12) and the portable editor shell (Vulkan) both host it, so neither
// shows less than the other.
//
// Drawing: DrawToolsMenu() goes inside the host's BeginMainMenuBar(),
// DrawHub() is one window with every tool (a list by category and the
// selected tool), and DrawWindows() draws the tools popped out into their
// own windows. All of it is plain Dear ImGui, so it runs headless too.

namespace aether {
class World;
namespace gas {
class AttributeSystem;
class EffectSystem;
class AbilitySystem;
} // namespace gas
} // namespace aether

namespace aether::assets {
class AssetDatabase;
}

namespace aether::editor {

class ExtensionRegistry;
class ScenePanel;
class EditorScripts;
class CommandRegistry;
class EditorDocument;
class NotificationService;
class FileDialog;
struct EditorSettings;

class EditorWorkspace {
public:
    // `project_file`: the .aproject the Project tools (Project Settings,
    // Build and Package) work on; empty for a sample project (SampleProject()).
    explicit EditorWorkspace(std::filesystem::path project_file = {});
    ~EditorWorkspace();
    EditorWorkspace(const EditorWorkspace&) = delete;
    EditorWorkspace& operator=(const EditorWorkspace&) = delete;

    // Per frame, before drawing: runs the networked play session and the
    // console's queued log lines.
    void Update(f32 dt);

    // A small project with a startup scene, made under the temp folder the
    // first time and reused after; empty if it can't be made.
    static std::filesystem::path SampleProject(std::string* error = nullptr);
    const std::filesystem::path& ProjectFile() const;
    // Points the Project tools (settings, Build and Package, Plugins) and
    // the plugins' modules at another project. False if it isn't one.
    bool OpenProject(const std::filesystem::path& project_file);

    // Stage 1 of docs/design/EDITOR_PARITY_AUDIT.md: the project hub. A host shows it at launch when it was given no
    // project; it lists the projects opened recently, creates one from a template, and opens one from disk. Drawn by
    // DrawWindows, so both shells and a headless test see the same thing.
    bool project_hub_open = false;
    void OpenProjectHub();
    // The recent list the hub shows, most recent first, as last saved (entries may name files that are gone).
    const std::vector<std::filesystem::path>& RecentProjectFiles() const;
    // Where that list is read from and written to. Defaults to the per-user config folder; a test points it at a
    // scratch file. Loading happens here, so a file written by an earlier session is visible at once.
    void SetRecentProjectsFile(const std::filesystem::path& file);
    // Drops the entries whose project file no longer exists, and saves. Returns how many went.
    std::size_t RemoveMissingProjects();
    // The native picker the Open Project actions use, filtered to .aproject. No value when the platform has no
    // picker (the hub then has a path field); an empty path when the user cancelled.
    std::optional<std::filesystem::path> PickProjectFile();
    // The picker itself, so a test can answer for the platform. Not owned; it also becomes the Scene panel's.
    void SetFileDialog(FileDialog* dialog);

    // What plugins, game modules and the project's editor scripts add (§26.5):
    // panels (tools under "Extensions"), menus, property drawers, asset types.
    ExtensionRegistry& Extensions();
    // The project's Luau editor scripts (Content/Editor/*.luau).
    EditorScripts& Scripts();

    // The world the Gameplay Debugger shows (the host points it at the world being played and
    // clears it with nullptr, which goes back to the built-in sample), and the running gameplay
    // systems its actions go through (any may be null).
    void SetGameplayWorld(World* world, gas::AttributeSystem* attributes = nullptr, gas::EffectSystem* effects = nullptr, gas::AbilitySystem* abilities = nullptr);

    usize ToolCount() const;
    const char* ToolName(usize tool) const;
    const char* ToolCategory(usize tool) const;
    // The tool whose name starts with `name` (case-insensitive), or -1.
    i64 FindTool(std::string_view name) const;

    // The tool the hub shows.
    usize Selected() const;
    void Select(usize tool);
    // Popped out into its own window.
    bool IsWindowOpen(usize tool) const;
    void SetWindowOpen(usize tool, bool open);

    // The tool's contents, into the current window.
    void DrawTool(usize tool);
    // The open scene (the Scene tool's document and actions), for a host's own menus and shortcuts.
    ScenePanel& Scene();
    // The document the shell's quit prompt asks about (Stage 0 of docs/design/EDITOR_PARITY_AUDIT.md): name, dirty
    // state, undo history and save, without the caller knowing it is a scene. There is one document today; Stage 1's
    // tabs turn this into "the focused one".
    EditorDocument& ActiveDocument();
    const EditorDocument& ActiveDocument() const;
    // What the editor wants to tell the user (Stage 0 of docs/design/EDITOR_PARITY_AUDIT.md): the toasts DrawWindows
    // shows and the rows the Messages tool lists. Save, autosave and recovery failures are posted here; a host with a
    // status bar may also read UnreadCount().
    NotificationService& Notifications();
    const NotificationService& Notifications() const;
    // The commands the editor has (Stage 0 of docs/design/EDITOR_PARITY_AUDIT.md): each with a stable id, a label,
    // a default chord, the context its chord applies in, an enabled predicate and an action. The menus, the palette
    // and the keyboard read this one table; a host may add its own (View's "Reset layout" is the shell's).
    CommandRegistry& Commands();
    const CommandRegistry& Commands() const;
    bool RunCommand(std::string_view id);
    // Draws the registry's items for one menu ("File", "Edit", "View") into the current ImGui menu, with their
    // chords and enabled state. Each switches the hub to the tool it belongs to where that makes the result visible.
    void DrawMenuItems(const char* menu);
    // An "Edit" menu (Undo, Redo, Duplicate, ...): the host puts it inside its menu bar.
    void DrawEditMenu();
    // A "Tools" menu: each category's tools; picking one pops it out.
    void DrawToolsMenu();
    // A window with every tool. `open` (optional) gets the title-bar X.
    void DrawHub(const char* title = "Tools", bool* open = nullptr);
    // The hub's contents into the current window (the shell fills its main area with this).
    void DrawHubContents();
    // The popped-out tool windows.
    void DrawWindows();

    // Data safety (Stage 0 of docs/design/EDITOR_PARITY_AUDIT.md): the editor writes the open scene into a
    // per-project recovery slot while it has unsaved changes, and offers it back when the last session did not end
    // cleanly. A clean save or a normal quit removes the slot; only an interrupted session leaves one to recover.
    EditorSettings& Settings();
    bool SaveSettings();
    // The open scene has edits that were never written to its file.
    bool HasUnsavedChanges() const;
    // A recovery autosave is waiting for this project.
    bool HasRecoverableAutosave() const;
    // Writes the open scene into its recovery slot now. False when it can't be written (Play mode, no project, no
    // unsaved changes); the reason goes to the log.
    bool AutosaveNow();
    // Makes the recovery autosave the open scene, keeping the scene file on disk untouched and the document dirty
    // until it is saved. False when there is nothing to recover, it can't be read, or the open scene has unsaved
    // changes of its own (the Scene panel says so).
    bool RecoverAutosave();
    // Throws the recovery autosave away.
    void DiscardAutosave();
    // Shows the recovery prompt when a slot exists; a host calls it once, after construction. DrawWindows draws the
    // prompt, so it works in both shells.
    void OfferRecovery();

    // Quitting. File > Quit and the title-bar X go through RequestQuit(); with unsaved changes it shows the prompt
    // DrawWindows draws, and on_quit_confirmed runs once the user has saved (or chosen to quit anyway). With nothing
    // unsaved it runs at once, before RequestQuit returns. The prompt's answers are public so a headless host can
    // make them too; SaveAndQuit is false (and the prompt stays) when the save itself fails.
    void RequestQuit();
    bool QuitPromptOpen() const;
    bool SaveAndQuit();
    void QuitWithoutSaving();
    void CancelQuit();
    std::function<void()> on_quit_confirmed;

    // View > Preferences: autosave, on or off and how often. Plain UI state, like the palette's.
    bool preferences_open = false;

    // Phase 43.5: the command palette. Plain UI state, like a tool's window flag, so a host --
    // or a headless test -- can open it without a keybinding. DrawWindows() draws it.
    bool command_palette_open = false;
    void DrawCommandPalette();
    // What the palette searches and what it does with a chosen asset. The host supplies both:
    // the workspace does not own the asset database, and "opening" an asset is a host decision.
    // With no database the palette lists tools only; with no opener, choosing an asset does
    // nothing rather than pretending to succeed.
    void SetAssetDatabase(const assets::AssetDatabase* database);
    void SetAssetOpener(std::function<void(const std::string&)> opener);

private:
    void RegisterCommands();
    // Runs the Global commands whose chord was pressed this frame; called from DrawWindows, inside a frame.
    void PollShortcuts();
    void DrawDataSafetyWindows();
    void DrawPreferences();
    void DrawProjectHub();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aether::editor
