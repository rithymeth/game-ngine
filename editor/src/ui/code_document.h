#pragma once

#include "aether/core/base.h"

#include <string>
#include <string_view>
#include <vector>

namespace aether::editor {

// The text model behind the script code editor (Phase 11 step 6,
// docs/design/PHASE_SPECS.md §11.5): lines, a cursor with an optional
// selection, editing with auto-indent, undo/redo, find. No UI; the ImGui
// widget (code_editor.h) draws it and turns keys into these calls.

struct TextPos {
    i32 line = 0;
    i32 column = 0; // a byte offset into the line (UTF-8; never inside a code point)
    bool operator==(const TextPos& o) const { return line == o.line && column == o.column; }
    bool operator!=(const TextPos& o) const { return !(*this == o); }
    bool operator<(const TextPos& o) const { return line != o.line ? line < o.line : column < o.column; }
};

struct CodeDocumentOptions {
    i32 indent_width = 4; // spaces per indent level (tabs are never inserted)
    usize max_undo = 256;
};

class CodeDocument {
public:
    using Options = CodeDocumentOptions;

    CodeDocument() : CodeDocument(std::string_view{}) {}
    explicit CodeDocument(std::string_view text, Options options = {});

    // Replaces everything (loading a file). Clears undo; not dirty.
    void SetText(std::string_view text);
    std::string Text() const; // lines joined with '\n'
    const std::vector<std::string>& Lines() const { return lines_; }
    i32 LineCount() const { return static_cast<i32>(lines_.size()); }
    const Options& GetOptions() const { return options_; }

    // Offsets into Text() and back (for completion, which works on offsets).
    usize OffsetOf(TextPos pos) const;
    TextPos PosOf(usize offset) const;
    TextPos Clamp(TextPos pos) const;

    // --- Cursor and selection -------------------------------------------
    TextPos Cursor() const { return cursor_; }
    // Moves the cursor; with `select`, extends the selection from where it
    // was (or its existing anchor).
    void SetCursor(TextPos pos, bool select = false);
    bool HasSelection() const { return has_anchor_ && anchor_ != cursor_; }
    TextPos SelectionStart() const;
    TextPos SelectionEnd() const;
    std::string SelectedText() const;
    void Select(TextPos from, TextPos to); // cursor ends at `to`
    void SelectAll();
    void ClearSelection() { has_anchor_ = false; }

    void MoveLeft(bool select = false);
    void MoveRight(bool select = false);
    void MoveUp(bool select = false);
    void MoveDown(bool select = false);
    void MoveWordLeft(bool select = false);
    void MoveWordRight(bool select = false);
    // Home goes to the first non-blank character, then (pressed again) to column 0.
    void MoveHome(bool select = false);
    void MoveEnd(bool select = false);
    void MoveToStart(bool select = false) { SetCursor({0, 0}, select); }
    void MoveToEnd(bool select = false);

    // --- Editing (each is one undo step, except runs of typing) ----------
    // Types text, replacing the selection. '\n' in it starts a new line with
    // auto-indent when `text` is exactly "\n"; pasted text is inserted as is.
    // Typing a closing keyword (end, else, elseif, until) or '}' alone on its
    // line takes back one indent level.
    void Type(std::string_view text);
    void Paste(std::string_view text);
    void NewLine() { Type("\n"); }
    void Backspace(); // with the cursor in leading spaces, removes to the previous indent stop
    void Delete();
    void DeleteSelection();
    // Tab/Shift+Tab: indents or unindents the selected lines, or (with no
    // selection) inserts spaces to the next indent stop / unindents the line.
    void Indent();
    void Unindent();
    // Ctrl+/: adds "-- " to the selected lines (or the cursor's), or removes
    // it if every non-blank line has it.
    void ToggleComment();
    // Replaces [from, to) with `text` (completion accepting an item).
    void Replace(TextPos from, TextPos to, std::string_view text);

    bool Undo();
    bool Redo();
    bool CanUndo() const { return !undo_.empty(); }
    bool CanRedo() const { return !redo_.empty(); }

    // Whether the text changed since the last SetText/MarkSaved (undoing back
    // to the saved text makes it clean again).
    bool Dirty() const;
    void MarkSaved();
    // Changes with every edit (and back with undo): cheap change detection.
    u64 Version() const { return version_; }

    // Selects the next match of `needle` after the cursor (or before it when
    // !forward), wrapping around. False if there's none.
    bool Find(std::string_view needle, bool forward = true, bool match_case = false);

private:
    struct Snapshot {
        std::vector<std::string> lines;
        TextPos cursor;
        u64 version = 0;
    };
    enum class EditKind { Other, Typing };

    void BeginEdit(EditKind kind);
    void InsertRaw(std::string_view text); // at the cursor, no selection handling
    void EraseRange(TextPos from, TextPos to);
    void AfterMove(bool select, TextPos old_cursor);
    std::string IndentOf(i32 line) const;
    bool OpensBlock(std::string_view before_cursor) const;
    void DedentClosingKeyword();

    Options options_;
    std::vector<std::string> lines_{""};
    TextPos cursor_;
    TextPos anchor_;
    bool has_anchor_ = false;
    std::vector<Snapshot> undo_;
    std::vector<Snapshot> redo_;
    EditKind last_edit_ = EditKind::Other;
    TextPos typing_at_{-1, -1}; // where the last typed character ended
    u64 version_ = 0;           // changes with every edit; restored by undo
    u64 next_version_ = 1;
    u64 saved_version_ = 0;
};

// ---------------------------------------------------------------------------
// Syntax highlighting
// ---------------------------------------------------------------------------

enum class TokenKind : u8 { Text, Keyword, Builtin, Number, String, Comment, Operator };

struct Token {
    i32 start = 0; // byte offsets into the line
    i32 end = 0;
    TokenKind kind = TokenKind::Text;
};

// What a line leaves open for the next: a long comment or string ("--[[",
// "[==[") of some level, or nothing.
struct LexState {
    enum class Open : u8 { None, Comment, String } open = Open::None;
    i32 level = 0;
    bool operator==(const LexState& o) const { return open == o.open && level == o.level; }
};

// Splits one line of Luau into tokens (whitespace between them is left out),
// given what the previous line left open, and updates `state` for the next.
// Builtins are the engine's script API and Luau's library names (world,
// Input, math, print, self, ...).
std::vector<Token> TokenizeLuauLine(std::string_view line, LexState& state);

} // namespace aether::editor
