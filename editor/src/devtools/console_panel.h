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

    // `playing` is the open scene's PIE state: the first frame of a play session clears the output when `clear_on_play` is on.
    void Draw(bool playing = false);
    // A drop-down window across the top of the screen while `open`;
    // `toggle_pressed` (the tilde key this frame) opens and closes it.
    void Overlay(bool toggle_pressed, float screen_width, float height_fraction = 0.4f);
    bool open = false;

    // What the filters let through, for tests and hosts: the level toggles, then the category (empty: all) and the text
    // substring; with `collapse_repeats` consecutive identical lines are one entry that carries their count.
    struct Entry {
        const ConsoleLine* line = nullptr;
        usize count = 1;
    };
    std::vector<Entry> VisibleLines();
    // The categories the output holds, first-seen order, without repeats (command output has none and is left out).
    std::vector<std::string> Categories();

    // What the input line does, for tests and key handling.
    void Submit(const std::string& line);
    void HistoryUp();
    void HistoryDown();
    void TabComplete();
    std::string input;
    std::string filter;
    std::string category; // "" shows every category
    bool show_trace = false; // trace is noise by default; the rest start on
    bool show_info = true;
    bool show_warn = true;
    bool show_error = true;
    bool collapse_repeats = true;
    bool clear_on_play = true;
    const std::vector<std::string>& Suggestions() const { return suggestions_; }

private:
    bool Passes(const ConsoleLine& line) const;
    Console& console_;
    int history_pos_ = -1; // -1: not browsing
    std::vector<std::string> suggestions_;
    std::string last_tab_;
    bool scroll_to_bottom_ = false;
    bool last_playing_ = false;
};

} // namespace aether::editor
