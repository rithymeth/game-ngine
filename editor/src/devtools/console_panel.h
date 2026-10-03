#pragma once

#include "aether/core/console.h"

#include <string>

namespace aether::editor {

// The console window (Phase 23 step 1): the output (coloured by level,
// filterable), and an input line with history (Up/Down) and completion
// (Tab; a second Tab lists the candidates). Portable; draws into the
// current ImGui window. Game builds draw the same panel as a drop-down
// overlay toggled with the tilde key (see Overlay).
class ConsolePanel {
public:
    explicit ConsolePanel(Console& console) : console_(console) {}

    void Draw();
    // A drop-down window across the top of the screen while `open`;
    // `toggle_pressed` (the tilde key this frame) opens and closes it.
    void Overlay(bool toggle_pressed, float screen_width, float height_fraction = 0.4f);
    bool open = false;

    // What the input line does, for tests and key handling.
    void Submit(const std::string& line);
    void HistoryUp();
    void HistoryDown();
    void TabComplete();
    std::string input;
    std::string filter;
    const std::vector<std::string>& Suggestions() const { return suggestions_; }

private:
    Console& console_;
    int history_pos_ = -1; // -1: not browsing
    std::vector<std::string> suggestions_;
    std::string last_tab_;
    bool scroll_to_bottom_ = false;
};

} // namespace aether::editor
