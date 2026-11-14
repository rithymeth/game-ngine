#pragma once

// Stage 0 of docs/design/EDITOR_PARITY_AUDIT.md: one table of editor commands.
//
// Menus, the command palette and the keyboard both read it, so a label, a chord
// and the action behind it are written down once. A startup test asks the table
// for duplicate ids (Add refuses them) and for chords that would both fire
// (Conflicts(), which the test requires to be empty).

#include <imgui.h>

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace aether::editor {

// Where a chord applies. A Global chord fires wherever the keyboard focus is
// (outside text fields); the others fire only where that part of the editor has
// focus, so a graph editor's own Ctrl+S keeps meaning "save this graph".
enum class CommandScope {
    Global,
    Scene,     // the Scene tool's document: undo, redo, save
    SceneTree, // the Scene tool's hierarchy list: duplicate, delete, copy, paste, rename
};

struct CommandChord {
    ImGuiKey key = ImGuiKey_None;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;

    bool IsSet() const { return key != ImGuiKey_None; }
    // "Ctrl+Shift+P"; empty for a command with no default chord.
    std::string ToString() const;
    // Exact modifiers: a Ctrl+S chord does not fire while Shift is held too.
    bool Matches(ImGuiKey pressed, bool ctrl_down, bool shift_down, bool alt_down) const;
    bool Pressed(const ImGuiIO& io) const;
    bool operator==(const CommandChord& other) const {
        return key == other.key && ctrl == other.ctrl && shift == other.shift && alt == other.alt;
    }
};

struct Command {
    std::string id;     // stable and dotted -- "scene.save". Settings and plugins name a command by this.
    std::string label;  // as the menus and the palette show it
    std::string menu;   // "File", "Edit", "View"; empty for a palette-only command
    bool separator_before = false;
    CommandChord chord;
    CommandScope scope = CommandScope::Global;
    // Empty means always enabled. Asked before a menu item draws and before a chord fires.
    std::function<bool()> enabled;
    // Empty means the label is static; Undo names what it would undo.
    std::function<std::string()> label_now;
    std::function<void()> run;
};

// "Anywhere", "Scene tool" or "Scene hierarchy": where a chord has to be focused to fire, for the cheat sheet.
const char* CommandScopeName(CommandScope scope);

// One row of the shortcut cheat sheet, built from the table so a new command with a chord appears without a second
// edit. `menu` is "Other" for a command with no menu (a toolbar or palette-only action).
struct ShortcutRow {
    std::string id;
    std::string label;
    std::string chord;
    std::string menu;
    CommandScope scope;
};

class CommandRegistry {
public:
    // False, and nothing added, when the id is empty or already registered.
    bool Add(Command command);

    const Command* Find(std::string_view id) const;
    std::size_t Size() const { return commands_.size(); }
    const Command& At(std::size_t index) const { return commands_[index]; }

    bool Enabled(const Command& command) const;
    std::string Label(const Command& command) const;

    // False when the command does not exist, is disabled, or has no action.
    bool Run(std::string_view id);

    // Pairs whose chords are the same and whose contexts can be active at once
    // (Global counts as active with everything else). Commands without a chord
    // are not pairs. Empty is the healthy state; a startup test requires it.
    std::vector<std::string> Conflicts() const;

    // The command a key press means in `context`, or nullptr. Runs the first
    // matching, enabled command and reports whether it did.
    bool RunPressedChord(CommandScope scope) const;

    // Every command with a default chord, in menu order (File, Edit, Asset, Actor, View, Tools, Window, Help, then
    // "Other" and any menu an extension adds), by label within a menu. The cheat sheet draws exactly this.
    std::vector<ShortcutRow> Shortcuts() const;

private:
    std::vector<Command> commands_;
};

} // namespace aether::editor
