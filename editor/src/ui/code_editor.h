#pragma once

#include "ui/code_document.h"

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace aether::editor {

// The script code editor panel's widget (Phase 11 step 6,
// docs/design/PHASE_SPECS.md §11.5), built only on Dear ImGui so it runs
// headless in tests. It edits a CodeDocument with Luau highlighting, a
// gutter with line numbers and breakpoints, the debugger's current line, an
// error marker, and a completion popup fed by a callback (the editor wires
// it to script::CompleteScript; this library doesn't depend on scripting).

struct CompletionSuggestion {
    std::string label;
    std::string detail; // shown dimmed after the label
};

struct CompletionReply {
    usize replace_from = 0; // text offset; the item replaces [replace_from, cursor)
    std::vector<CompletionSuggestion> items;
};

// Called with the whole text and the cursor's offset in it.
using CompletionProvider = std::function<CompletionReply(const std::string& text, usize cursor)>;

struct CodeEditorOptions {
    CompletionProvider completion;
    bool auto_complete = true; // open the popup while typing names and after '.' / ':'
    bool read_only = false;
    i32 execution_line = -1;   // 0-based line the debugger is stopped on, or -1
    i32 error_line = -1;       // 0-based line of a compile error, or -1
    std::string error_message; // shown when hovering the error line's gutter
};

// Per-widget state the caller keeps (one per open file).
struct CodeEditorState {
    std::set<i32> breakpoints; // 0-based lines; the gutter toggles them
    bool focused = false;
    bool request_focus = false; // takes keyboard focus on the next draw
    bool scroll_to_cursor = false;

    // The completion popup.
    bool completion_open = false;
    i32 completion_selected = 0;
    CompletionReply completion;

    // Where the last draw put things (screen space), for tests and hit testing.
    float origin_x = 0.0f, origin_y = 0.0f; // top-left of line 0's text
    float gutter_x = 0.0f;                  // left edge of the gutter
    float line_height = 0.0f;
    float char_width = 0.0f;
};

struct CodeEditorResult {
    bool changed = false;       // the text was edited this frame
    i32 toggled_breakpoint = -1; // a gutter click added or removed this line's breakpoint
    bool save_requested = false; // Ctrl+S
};

// Keys while focused:
//   typing, Enter (auto-indent), Backspace/Delete, arrows (+Shift to select,
//   +Ctrl by word), Home/End, Ctrl+Home/End, PageUp/PageDown, Ctrl+A,
//   Ctrl+C/X/V, Ctrl+Z / Ctrl+Y (or Ctrl+Shift+Z), Tab/Shift+Tab, Ctrl+/,
//   Ctrl+S, Ctrl+Space (completion). In the popup: Up/Down, Enter or Tab to
//   accept, Escape to close.
// Mouse: click to place the cursor (Shift+click extends the selection),
// drag to select, click the gutter to toggle a breakpoint.
CodeEditorResult DrawCodeEditor(const char* id, CodeDocument& document, CodeEditorState& state,
                                const CodeEditorOptions& options = {});

} // namespace aether::editor
