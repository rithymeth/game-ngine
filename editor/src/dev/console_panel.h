#pragma once

#include "aether/core/base.h"

#include <deque>
#include <string>
#include <vector>

// The editor's Console panel (Phase 23 step 4). A dockable ImGui window with a
// scrollback, an input line with ↑/↓ history, tab autocomplete over the registered
// CVars + commands, and a hook for the host to log its own output into it.
// Portable (ImGui + engine CVars only), so it runs headless in tests.

namespace aether::editor {

class ConsolePanel {
public:
    // Logs a line into the scrollback (the host feeds its own output/errors
    // here each frame; command output is captured automatically).
    void Log(const std::string& text, bool error = false);

    void Open() { open_ = true; }
    void Close() { open_ = false; }
    bool IsOpen() const { return open_; }
    void Toggle() { open_ = !open_; }
    bool* OpenPtr() { return &open_; }

    // Draws the window (no-op when closed). Returns the raw submitted line to
    // execute, or an empty string; the host runs it via cvars::RunCommand.
    std::string Draw();

    // Command the panel executed via cvars::RunCommand (for tests).
    const std::deque<std::string>& History() const { return history_; }

private:
    bool open_ = false;
    std::deque<std::string> scrollback_;
    std::deque<std::string> history_;
    usize history_index_ = 0; // == history_.size() when positioned at the newest
    std::string input_;
    bool autoscroll_ = true;
};

} // namespace aether::editor
