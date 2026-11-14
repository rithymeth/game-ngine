#include "workspace/command_registry.h"

#include <algorithm>
#include <utility>

namespace aether::editor {

std::string CommandChord::ToString() const {
    if (!IsSet()) return {};
    std::string text;
    if (ctrl) text += "Ctrl+";
    if (shift) text += "Shift+";
    if (alt) text += "Alt+";
    text += ImGui::GetKeyName(key);
    return text;
}

bool CommandChord::Matches(ImGuiKey pressed, bool ctrl_down, bool shift_down, bool alt_down) const {
    return IsSet() && key == pressed && ctrl == ctrl_down && shift == shift_down && alt == alt_down;
}

bool CommandChord::Pressed(const ImGuiIO& io) const {
    return Matches(key, io.KeyCtrl, io.KeyShift, io.KeyAlt) && ImGui::IsKeyPressed(key, false);
}

bool CommandRegistry::Add(Command command) {
    if (command.id.empty() || Find(command.id) != nullptr) return false;
    commands_.push_back(std::move(command));
    return true;
}

const Command* CommandRegistry::Find(std::string_view id) const {
    if (id.empty()) return nullptr;
    for (const Command& command : commands_) {
        if (command.id == id) return &command;
    }
    return nullptr;
}

bool CommandRegistry::Enabled(const Command& command) const { return !command.enabled || command.enabled(); }

std::string CommandRegistry::Label(const Command& command) const {
    return command.label_now ? command.label_now() : command.label;
}

bool CommandRegistry::Run(std::string_view id) {
    const Command* command = Find(id);
    if (command == nullptr || !Enabled(*command) || !command->run) return false;
    // Copied first: the action may add commands, and the table's storage is not the action's to keep.
    const std::function<void()> run = command->run;
    run();
    return true;
}

std::vector<std::string> CommandRegistry::Conflicts() const {
    const auto overlap = [](CommandScope a, CommandScope b) {
        return a == b || a == CommandScope::Global || b == CommandScope::Global;
    };
    std::vector<std::string> conflicts;
    for (std::size_t i = 0; i < commands_.size(); ++i) {
        const Command& a = commands_[i];
        if (!a.chord.IsSet()) continue;
        for (std::size_t j = i + 1; j < commands_.size(); ++j) {
            const Command& b = commands_[j];
            if (!b.chord.IsSet() || !(a.chord == b.chord) || !overlap(a.scope, b.scope)) continue;
            conflicts.push_back(a.id + " and " + b.id + " both use " + a.chord.ToString());
        }
    }
    return conflicts;
}

const char* CommandScopeName(CommandScope scope) {
    switch (scope) {
    case CommandScope::Global: return "Anywhere";
    case CommandScope::Scene: return "Scene tool";
    case CommandScope::SceneTree: return "Scene hierarchy";
    }
    return "Anywhere";
}

std::vector<ShortcutRow> CommandRegistry::Shortcuts() const {
    static constexpr const char* kMenuOrder[] = {"File", "Edit", "Asset", "Actor", "View", "Tools", "Window", "Help", "Other"};
    const auto rank = [](const std::string& menu) {
        int index = 0;
        for (const char* known : kMenuOrder) {
            if (menu == known) return index;
            ++index;
        }
        return 100; // a menu an extension adds comes after the editor's, alphabetically
    };
    std::vector<ShortcutRow> rows;
    for (const Command& command : commands_) {
        if (!command.chord.IsSet()) continue;
        rows.push_back({command.id, Label(command), command.chord.ToString(),
                        command.menu.empty() ? std::string("Other") : command.menu, command.scope});
    }
    std::stable_sort(rows.begin(), rows.end(), [&rank](const ShortcutRow& a, const ShortcutRow& b) {
        const int rank_a = rank(a.menu);
        const int rank_b = rank(b.menu);
        if (rank_a != rank_b) return rank_a < rank_b;
        if (a.menu != b.menu) return a.menu < b.menu;
        return a.label < b.label;
    });
    return rows;
}

bool CommandRegistry::RunPressedChord(CommandScope scope) const {
    const ImGuiIO& io = ImGui::GetIO();
    for (std::size_t i = 0; i < commands_.size(); ++i) {
        const Command& command = commands_[i];
        if (command.scope != scope || !command.chord.Pressed(io) || !Enabled(command)) continue;
        // Copied before the call, for the same reason as in Run().
        const std::function<void()> run = command.run;
        if (!run) continue;
        run();
        return true;
    }
    return false;
}

} // namespace aether::editor
