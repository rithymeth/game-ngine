#include "ui/code_document.h"

#include <algorithm>
#include <cctype>

namespace aether::editor {

namespace {

bool IsContinuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }
bool IsWordChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || static_cast<unsigned char>(c) >= 0x80;
}
bool IsBlank(char c) { return c == ' ' || c == '\t'; }

std::vector<std::string> SplitLines(std::string_view text) {
    std::vector<std::string> lines;
    std::string current;
    for (usize i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\r') {
            continue; // CRLF files load as LF
        }
        if (c == '\n') {
            lines.push_back(std::move(current));
            current.clear();
        } else if (c == '\t') {
            current += "    "; // no tabs in the model: columns stay simple
        } else {
            current.push_back(c);
        }
    }
    lines.push_back(std::move(current));
    return lines;
}

usize LeadingSpaces(const std::string& line) {
    usize n = 0;
    while (n < line.size() && line[n] == ' ') {
        ++n;
    }
    return n;
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && IsBlank(s.front())) s.remove_prefix(1);
    while (!s.empty() && IsBlank(s.back())) s.remove_suffix(1);
    return s;
}

// The line with any "-- comment" at the end removed (strings aren't
// considered: good enough for indentation decisions).
std::string_view WithoutComment(std::string_view line) {
    const usize comment = line.find("--");
    return comment == std::string_view::npos ? line : line.substr(0, comment);
}

bool EndsWithWord(std::string_view s, std::string_view word) {
    if (s.size() < word.size() || s.substr(s.size() - word.size()) != word) {
        return false;
    }
    return s.size() == word.size() || !IsWordChar(s[s.size() - word.size() - 1]);
}

} // namespace

CodeDocument::CodeDocument(std::string_view text, Options options) : options_(options) { SetText(text); }

void CodeDocument::SetText(std::string_view text) {
    lines_ = SplitLines(text);
    cursor_ = {};
    has_anchor_ = false;
    undo_.clear();
    redo_.clear();
    last_edit_ = EditKind::Other;
    version_ = next_version_++;
    saved_version_ = version_;
}

std::string CodeDocument::Text() const {
    std::string text;
    for (usize i = 0; i < lines_.size(); ++i) {
        if (i > 0) {
            text.push_back('\n');
        }
        text += lines_[i];
    }
    return text;
}

TextPos CodeDocument::Clamp(TextPos pos) const {
    pos.line = std::clamp(pos.line, 0, LineCount() - 1);
    const std::string& line = lines_[static_cast<usize>(pos.line)];
    pos.column = std::clamp(pos.column, 0, static_cast<i32>(line.size()));
    while (pos.column > 0 && pos.column < static_cast<i32>(line.size()) &&
           IsContinuation(line[static_cast<usize>(pos.column)])) {
        --pos.column;
    }
    return pos;
}

usize CodeDocument::OffsetOf(TextPos pos) const {
    pos = Clamp(pos);
    usize offset = 0;
    for (i32 i = 0; i < pos.line; ++i) {
        offset += lines_[static_cast<usize>(i)].size() + 1;
    }
    return offset + static_cast<usize>(pos.column);
}

TextPos CodeDocument::PosOf(usize offset) const {
    for (i32 i = 0; i < LineCount(); ++i) {
        const usize size = lines_[static_cast<usize>(i)].size();
        if (offset <= size) {
            return Clamp({i, static_cast<i32>(offset)});
        }
        offset -= size + 1;
    }
    return {LineCount() - 1, static_cast<i32>(lines_.back().size())};
}

// ---------------------------------------------------------------------------
// Cursor and selection
// ---------------------------------------------------------------------------

void CodeDocument::SetCursor(TextPos pos, bool select) {
    const TextPos old = cursor_;
    cursor_ = Clamp(pos);
    AfterMove(select, old);
}

void CodeDocument::AfterMove(bool select, TextPos old_cursor) {
    if (select) {
        if (!has_anchor_) {
            anchor_ = old_cursor;
            has_anchor_ = true;
        }
    } else {
        has_anchor_ = false;
    }
    last_edit_ = EditKind::Other; // moving ends a run of typing
}

TextPos CodeDocument::SelectionStart() const { return HasSelection() ? std::min(anchor_, cursor_) : cursor_; }
TextPos CodeDocument::SelectionEnd() const { return HasSelection() ? std::max(anchor_, cursor_) : cursor_; }

std::string CodeDocument::SelectedText() const {
    if (!HasSelection()) {
        return {};
    }
    const std::string text = Text();
    const usize from = OffsetOf(SelectionStart());
    return text.substr(from, OffsetOf(SelectionEnd()) - from);
}

void CodeDocument::Select(TextPos from, TextPos to) {
    anchor_ = Clamp(from);
    has_anchor_ = true;
    cursor_ = Clamp(to);
    last_edit_ = EditKind::Other;
}

void CodeDocument::SelectAll() { Select({0, 0}, {LineCount() - 1, static_cast<i32>(lines_.back().size())}); }

void CodeDocument::MoveLeft(bool select) {
    if (HasSelection() && !select) {
        SetCursor(SelectionStart());
        return;
    }
    const TextPos old = cursor_;
    if (cursor_.column > 0) {
        const std::string& line = lines_[static_cast<usize>(cursor_.line)];
        do {
            --cursor_.column;
        } while (cursor_.column > 0 && IsContinuation(line[static_cast<usize>(cursor_.column)]));
    } else if (cursor_.line > 0) {
        --cursor_.line;
        cursor_.column = static_cast<i32>(lines_[static_cast<usize>(cursor_.line)].size());
    }
    AfterMove(select, old);
}

void CodeDocument::MoveRight(bool select) {
    if (HasSelection() && !select) {
        SetCursor(SelectionEnd());
        return;
    }
    const TextPos old = cursor_;
    const std::string& line = lines_[static_cast<usize>(cursor_.line)];
    if (cursor_.column < static_cast<i32>(line.size())) {
        do {
            ++cursor_.column;
        } while (cursor_.column < static_cast<i32>(line.size()) && IsContinuation(line[static_cast<usize>(cursor_.column)]));
    } else if (cursor_.line + 1 < LineCount()) {
        ++cursor_.line;
        cursor_.column = 0;
    }
    AfterMove(select, old);
}

void CodeDocument::MoveUp(bool select) {
    const TextPos old = cursor_;
    if (cursor_.line == 0) {
        cursor_.column = 0;
    } else {
        cursor_ = Clamp({cursor_.line - 1, cursor_.column});
    }
    AfterMove(select, old);
}

void CodeDocument::MoveDown(bool select) {
    const TextPos old = cursor_;
    if (cursor_.line + 1 >= LineCount()) {
        cursor_.column = static_cast<i32>(lines_.back().size());
    } else {
        cursor_ = Clamp({cursor_.line + 1, cursor_.column});
    }
    AfterMove(select, old);
}

void CodeDocument::MoveWordLeft(bool select) {
    const TextPos old = cursor_;
    if (cursor_.column == 0) {
        MoveLeft(select);
        return;
    }
    const std::string& line = lines_[static_cast<usize>(cursor_.line)];
    usize i = static_cast<usize>(cursor_.column);
    while (i > 0 && IsBlank(line[i - 1])) --i;
    if (i > 0 && IsWordChar(line[i - 1])) {
        while (i > 0 && IsWordChar(line[i - 1])) --i;
    } else if (i > 0) {
        --i; // one punctuation character
    }
    cursor_.column = static_cast<i32>(i);
    AfterMove(select, old);
}

void CodeDocument::MoveWordRight(bool select) {
    const TextPos old = cursor_;
    const std::string& line = lines_[static_cast<usize>(cursor_.line)];
    if (cursor_.column >= static_cast<i32>(line.size())) {
        MoveRight(select);
        return;
    }
    usize i = static_cast<usize>(cursor_.column);
    if (IsWordChar(line[i])) {
        while (i < line.size() && IsWordChar(line[i])) ++i;
    } else if (!IsBlank(line[i])) {
        ++i;
    }
    while (i < line.size() && IsBlank(line[i])) ++i;
    cursor_.column = static_cast<i32>(i);
    AfterMove(select, old);
}

void CodeDocument::MoveHome(bool select) {
    const TextPos old = cursor_;
    const i32 first = static_cast<i32>(LeadingSpaces(lines_[static_cast<usize>(cursor_.line)]));
    cursor_.column = cursor_.column == first ? 0 : first;
    AfterMove(select, old);
}

void CodeDocument::MoveEnd(bool select) {
    const TextPos old = cursor_;
    cursor_.column = static_cast<i32>(lines_[static_cast<usize>(cursor_.line)].size());
    AfterMove(select, old);
}

void CodeDocument::MoveToEnd(bool select) { SetCursor({LineCount() - 1, static_cast<i32>(lines_.back().size())}, select); }

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

void CodeDocument::BeginEdit(EditKind kind) {
    const bool coalesce = kind == EditKind::Typing && last_edit_ == EditKind::Typing && cursor_ == typing_at_ &&
                          !HasSelection() && !undo_.empty();
    if (!coalesce) {
        undo_.push_back({lines_, cursor_, version_});
        if (undo_.size() > options_.max_undo) {
            undo_.erase(undo_.begin());
        }
    }
    redo_.clear();
    last_edit_ = kind;
    version_ = next_version_++;
}

void CodeDocument::InsertRaw(std::string_view text) {
    const std::vector<std::string> pieces = SplitLines(text);
    std::string& line = lines_[static_cast<usize>(cursor_.line)];
    const std::string tail = line.substr(static_cast<usize>(cursor_.column));
    line.erase(static_cast<usize>(cursor_.column));
    line += pieces.front();
    if (pieces.size() == 1) {
        cursor_.column = static_cast<i32>(line.size());
        line += tail;
        return;
    }
    lines_.insert(lines_.begin() + cursor_.line + 1, pieces.begin() + 1, pieces.end());
    cursor_.line += static_cast<i32>(pieces.size()) - 1;
    std::string& last = lines_[static_cast<usize>(cursor_.line)];
    cursor_.column = static_cast<i32>(last.size());
    last += tail;
}

void CodeDocument::EraseRange(TextPos from, TextPos to) {
    from = Clamp(from);
    to = Clamp(to);
    if (to < from) {
        std::swap(from, to);
    }
    std::string& first = lines_[static_cast<usize>(from.line)];
    const std::string tail = lines_[static_cast<usize>(to.line)].substr(static_cast<usize>(to.column));
    first.erase(static_cast<usize>(from.column));
    first += tail;
    lines_.erase(lines_.begin() + from.line + 1, lines_.begin() + to.line + 1);
    cursor_ = from;
    has_anchor_ = false;
}

std::string CodeDocument::IndentOf(i32 line) const {
    return std::string(LeadingSpaces(lines_[static_cast<usize>(line)]), ' ');
}

bool CodeDocument::OpensBlock(std::string_view before) const {
    const std::string_view code = Trim(WithoutComment(before));
    if (code.empty()) {
        return false;
    }
    if (code.back() == '{' || code.back() == '(') {
        return true;
    }
    for (std::string_view word : {"then", "do", "else", "repeat"}) {
        if (EndsWithWord(code, word)) {
            return true;
        }
    }
    // "function name(args)" / "local f = function(a)" with no "end" after it.
    const usize function = code.rfind("function");
    if (function != std::string_view::npos && code.back() == ')' &&
        code.find("end", function) == std::string_view::npos) {
        return true;
    }
    return false;
}

void CodeDocument::Type(std::string_view text) {
    if (text.empty()) {
        return;
    }
    const bool typing = text.size() == 1 ? IsWordChar(text[0]) : (static_cast<unsigned char>(text[0]) >= 0x80);
    BeginEdit(typing ? EditKind::Typing : EditKind::Other);
    if (HasSelection()) {
        EraseRange(SelectionStart(), SelectionEnd());
    }
    if (text == "\n") {
        const std::string& line = lines_[static_cast<usize>(cursor_.line)];
        const std::string before = line.substr(0, static_cast<usize>(cursor_.column));
        std::string indent = IndentOf(cursor_.line);
        indent.resize(std::min(indent.size(), before.size()));
        if (OpensBlock(before)) {
            indent += std::string(static_cast<usize>(options_.indent_width), ' ');
        }
        // Trailing spaces before the cursor go (a blank indented line stays empty).
        std::string& current = lines_[static_cast<usize>(cursor_.line)];
        usize trim = static_cast<usize>(cursor_.column);
        while (trim > 0 && current[trim - 1] == ' ') --trim;
        current.erase(trim, static_cast<usize>(cursor_.column) - trim);
        cursor_.column = static_cast<i32>(trim);
        // The rest of the line moves down without its leading spaces.
        std::string& now = lines_[static_cast<usize>(cursor_.line)];
        usize rest = static_cast<usize>(cursor_.column);
        while (rest < now.size() && now[rest] == ' ') ++rest;
        now.erase(static_cast<usize>(cursor_.column), rest - static_cast<usize>(cursor_.column));
        InsertRaw("\n" + indent);
    } else {
        InsertRaw(text);
        if (text.size() == 1) {
            DedentClosingKeyword();
        }
    }
    typing_at_ = cursor_;
}

void CodeDocument::DedentClosingKeyword() {
    std::string& line = lines_[static_cast<usize>(cursor_.line)];
    if (cursor_.column != static_cast<i32>(line.size())) {
        return;
    }
    const std::string_view trimmed = Trim(line);
    const bool closer = trimmed == "end" || trimmed == "else" || trimmed == "elseif" || trimmed == "until" ||
                        trimmed == "}" || trimmed == ")";
    if (!closer || cursor_.line == 0) {
        return;
    }
    // Only when it's indented deeper than the line that opened the block
    // (the nearest non-blank line above, if it doesn't open one itself).
    const usize indent = LeadingSpaces(line);
    i32 above = cursor_.line - 1;
    while (above > 0 && Trim(lines_[static_cast<usize>(above)]).empty()) --above;
    const std::string& prev = lines_[static_cast<usize>(above)];
    const usize prev_indent = LeadingSpaces(prev);
    const usize target = OpensBlock(prev) ? prev_indent : prev_indent - std::min<usize>(prev_indent, static_cast<usize>(options_.indent_width));
    if (indent > target) {
        line.erase(0, indent - target);
        cursor_.column -= static_cast<i32>(indent - target);
    }
}

void CodeDocument::Paste(std::string_view text) {
    if (text.empty()) {
        return;
    }
    BeginEdit(EditKind::Other);
    if (HasSelection()) {
        EraseRange(SelectionStart(), SelectionEnd());
    }
    InsertRaw(text);
}

void CodeDocument::DeleteSelection() {
    if (!HasSelection()) {
        return;
    }
    BeginEdit(EditKind::Other);
    EraseRange(SelectionStart(), SelectionEnd());
}

void CodeDocument::Backspace() {
    if (HasSelection()) {
        DeleteSelection();
        return;
    }
    if (cursor_.line == 0 && cursor_.column == 0) {
        return;
    }
    BeginEdit(EditKind::Other);
    const std::string& line = lines_[static_cast<usize>(cursor_.line)];
    const usize spaces = LeadingSpaces(line);
    TextPos from = cursor_;
    if (cursor_.column > 0 && static_cast<usize>(cursor_.column) <= spaces) {
        // In the indentation: back to the previous indent stop.
        const i32 width = options_.indent_width;
        const i32 stop = ((cursor_.column - 1) / width) * width;
        from.column = stop;
    } else if (cursor_.column > 0) {
        from.column = cursor_.column - 1;
        while (from.column > 0 && IsContinuation(line[static_cast<usize>(from.column)])) --from.column;
    } else {
        from = {cursor_.line - 1, static_cast<i32>(lines_[static_cast<usize>(cursor_.line - 1)].size())};
    }
    EraseRange(from, cursor_);
}

void CodeDocument::Delete() {
    if (HasSelection()) {
        DeleteSelection();
        return;
    }
    const std::string& line = lines_[static_cast<usize>(cursor_.line)];
    TextPos to = cursor_;
    if (cursor_.column < static_cast<i32>(line.size())) {
        do {
            ++to.column;
        } while (to.column < static_cast<i32>(line.size()) && IsContinuation(line[static_cast<usize>(to.column)]));
    } else if (cursor_.line + 1 < LineCount()) {
        to = {cursor_.line + 1, 0};
    } else {
        return;
    }
    BeginEdit(EditKind::Other);
    EraseRange(cursor_, to);
}

void CodeDocument::Indent() {
    const std::string unit(static_cast<usize>(options_.indent_width), ' ');
    if (!HasSelection()) {
        BeginEdit(EditKind::Other);
        const i32 width = options_.indent_width;
        const i32 spaces = width - cursor_.column % width;
        InsertRaw(std::string(static_cast<usize>(spaces), ' '));
        return;
    }
    BeginEdit(EditKind::Other);
    const TextPos start = SelectionStart();
    TextPos end = SelectionEnd();
    const i32 last = end.column == 0 && end.line > start.line ? end.line - 1 : end.line;
    for (i32 l = start.line; l <= last; ++l) {
        if (!lines_[static_cast<usize>(l)].empty()) {
            lines_[static_cast<usize>(l)].insert(0, unit);
        }
    }
    auto shift = [&](TextPos p) {
        if (p.line >= start.line && p.line <= last && !lines_[static_cast<usize>(p.line)].empty() && p.column > 0) {
            p.column += options_.indent_width;
        }
        return p;
    };
    anchor_ = shift(anchor_);
    cursor_ = shift(cursor_);
}

void CodeDocument::Unindent() {
    BeginEdit(EditKind::Other);
    const TextPos start = SelectionStart();
    const TextPos end = SelectionEnd();
    const i32 last = HasSelection() && end.column == 0 && end.line > start.line ? end.line - 1 : end.line;
    const bool had_selection = HasSelection();
    for (i32 l = start.line; l <= last; ++l) {
        std::string& line = lines_[static_cast<usize>(l)];
        const usize remove = std::min(LeadingSpaces(line), static_cast<usize>(options_.indent_width));
        line.erase(0, remove);
        auto shift = [&](TextPos& p) {
            if (p.line == l) p.column = std::max(0, p.column - static_cast<i32>(remove));
        };
        shift(cursor_);
        if (had_selection) shift(anchor_);
    }
}

void CodeDocument::ToggleComment() {
    const TextPos start = SelectionStart();
    const TextPos end = SelectionEnd();
    const i32 last = HasSelection() && end.column == 0 && end.line > start.line ? end.line - 1 : end.line;
    bool all_commented = true;
    usize indent = std::string::npos;
    for (i32 l = start.line; l <= last; ++l) {
        const std::string& line = lines_[static_cast<usize>(l)];
        if (Trim(line).empty()) continue;
        indent = std::min(indent, LeadingSpaces(line));
        if (Trim(line).substr(0, 2) != "--") all_commented = false;
    }
    if (indent == std::string::npos) {
        return; // only blank lines
    }
    BeginEdit(EditKind::Other);
    for (i32 l = start.line; l <= last; ++l) {
        std::string& line = lines_[static_cast<usize>(l)];
        if (Trim(line).empty()) continue;
        i32 delta = 0;
        usize at = indent;
        if (all_commented) {
            at = LeadingSpaces(line);
            const usize length = line.compare(at, 3, "-- ") == 0 ? 3 : 2;
            line.erase(at, length);
            delta = -static_cast<i32>(length);
        } else {
            line.insert(indent, "-- ");
            delta = 3;
        }
        for (TextPos* p : {&cursor_, &anchor_}) {
            if (p->line == l && p->column >= static_cast<i32>(at)) {
                p->column = std::max(static_cast<i32>(at), p->column + delta);
            }
        }
    }
}

void CodeDocument::Replace(TextPos from, TextPos to, std::string_view text) {
    BeginEdit(EditKind::Other);
    EraseRange(from, to);
    InsertRaw(text);
}

bool CodeDocument::Undo() {
    if (undo_.empty()) {
        return false;
    }
    redo_.push_back({lines_, cursor_, version_});
    Snapshot& s = undo_.back();
    lines_ = std::move(s.lines);
    cursor_ = s.cursor;
    version_ = s.version;
    undo_.pop_back();
    has_anchor_ = false;
    last_edit_ = EditKind::Other;
    return true;
}

bool CodeDocument::Redo() {
    if (redo_.empty()) {
        return false;
    }
    undo_.push_back({lines_, cursor_, version_});
    Snapshot& s = redo_.back();
    lines_ = std::move(s.lines);
    cursor_ = s.cursor;
    version_ = s.version;
    redo_.pop_back();
    has_anchor_ = false;
    last_edit_ = EditKind::Other;
    return true;
}

bool CodeDocument::Dirty() const { return version_ != saved_version_; }
void CodeDocument::MarkSaved() { saved_version_ = version_; }

bool CodeDocument::Find(std::string_view needle, bool forward, bool match_case) {
    if (needle.empty()) {
        return false;
    }
    std::string text = Text();
    std::string pattern(needle);
    if (!match_case) {
        for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (char& c : pattern) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    usize found = std::string::npos;
    if (forward) {
        const usize from = OffsetOf(SelectionEnd());
        found = text.find(pattern, from);
        if (found == std::string::npos) found = text.find(pattern); // wrap
    } else {
        const usize from = OffsetOf(SelectionStart());
        found = from > 0 ? text.rfind(pattern, from - 1) : std::string::npos;
        if (found == std::string::npos || found + pattern.size() > from) {
            const usize wrapped = text.rfind(pattern);
            found = found != std::string::npos && found + pattern.size() <= from ? found : wrapped;
        }
    }
    if (found == std::string::npos) {
        return false;
    }
    Select(PosOf(found), PosOf(found + pattern.size()));
    return true;
}

// ---------------------------------------------------------------------------
// Tokenizer
// ---------------------------------------------------------------------------

namespace {

bool IsKeyword(std::string_view w) {
    static const char* const kWords[] = {"and",   "break", "continue", "do",     "else",   "elseif", "end",
                                         "export", "false", "for",     "function", "if",   "in",     "local",
                                         "nil",   "not",   "or",       "repeat", "return", "then",   "true",
                                         "type",  "until", "while"};
    for (const char* k : kWords) {
        if (w == k) return true;
    }
    return false;
}

bool IsBuiltin(std::string_view w) {
    static const char* const kNames[] = {
        "self",     "world",   "Input",     "Timer",     "Event",   "math",     "string",   "table",   "vector",
        "bit32",    "utf8",    "buffer",    "coroutine", "os",      "print",    "pairs",    "ipairs",  "next",
        "select",   "type",    "typeof",    "tostring",  "tonumber", "pcall",   "xpcall",   "error",   "assert",
        "setmetatable", "getmetatable", "rawget", "rawset", "rawequal", "rawlen", "unpack"};
    for (const char* k : kNames) {
        if (w == k) return true;
    }
    return false;
}

// The level of a long bracket "[==[" opening at `i`, or -1.
i32 LongOpen(std::string_view s, usize i) {
    if (i >= s.size() || s[i] != '[') return -1;
    usize j = i + 1;
    while (j < s.size() && s[j] == '=') ++j;
    return j < s.size() && s[j] == '[' ? static_cast<i32>(j - i - 1) : -1;
}

// Where the long bracket of `level` closes at or after `i` (just past it), or npos.
usize LongClose(std::string_view s, usize i, i32 level) {
    const std::string close = "]" + std::string(static_cast<usize>(level), '=') + "]";
    const usize at = s.find(close, i);
    return at == std::string_view::npos ? std::string_view::npos : at + close.size();
}

} // namespace

std::vector<Token> TokenizeLuauLine(std::string_view s, LexState& state) {
    std::vector<Token> tokens;
    usize i = 0;
    auto push = [&](usize start, usize end, TokenKind kind) {
        tokens.push_back({static_cast<i32>(start), static_cast<i32>(end), kind});
    };
    if (state.open != LexState::Open::None) {
        const TokenKind kind = state.open == LexState::Open::Comment ? TokenKind::Comment : TokenKind::String;
        const usize end = LongClose(s, 0, state.level);
        if (end == std::string_view::npos) {
            if (!s.empty()) push(0, s.size(), kind);
            return tokens; // still open
        }
        push(0, end, kind);
        state = {};
        i = end;
    }
    while (i < s.size()) {
        const char c = s[i];
        if (IsBlank(c)) {
            ++i;
            continue;
        }
        const usize start = i;
        if (c == '-' && i + 1 < s.size() && s[i + 1] == '-') {
            const i32 level = LongOpen(s, i + 2);
            if (level >= 0) {
                const usize end = LongClose(s, i + 4 + static_cast<usize>(level), level);
                if (end == std::string_view::npos) {
                    push(start, s.size(), TokenKind::Comment);
                    state = {LexState::Open::Comment, level};
                    return tokens;
                }
                push(start, end, TokenKind::Comment);
                i = end;
                continue;
            }
            push(start, s.size(), TokenKind::Comment);
            return tokens;
        }
        if (const i32 level = LongOpen(s, i); level >= 0) {
            const usize end = LongClose(s, i + 2 + static_cast<usize>(level), level);
            if (end == std::string_view::npos) {
                push(start, s.size(), TokenKind::String);
                state = {LexState::Open::String, level};
                return tokens;
            }
            push(start, end, TokenKind::String);
            i = end;
            continue;
        }
        if (c == '"' || c == '\'' || c == '`') {
            ++i;
            while (i < s.size() && s[i] != c) {
                i += s[i] == '\\' ? 2 : 1;
            }
            i = std::min(i + 1, s.size());
            push(start, i, TokenKind::String);
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) != 0 ||
            (c == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])) != 0)) {
            ++i;
            while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) != 0 || s[i] == '.' || s[i] == '_' ||
                                    ((s[i] == '+' || s[i] == '-') && (s[i - 1] == 'e' || s[i - 1] == 'E')))) {
                ++i;
            }
            push(start, i, TokenKind::Number);
            continue;
        }
        if (IsWordChar(c)) {
            while (i < s.size() && IsWordChar(s[i])) ++i;
            const std::string_view word = s.substr(start, i - start);
            // A field or method name ("t.type", "e:print") isn't a keyword or builtin.
            const bool member = start > 0 && (s[start - 1] == '.' || s[start - 1] == ':') &&
                                !(start > 1 && s[start - 2] == '.');
            const TokenKind kind = member             ? TokenKind::Text
                                   : IsKeyword(word)  ? TokenKind::Keyword
                                   : IsBuiltin(word)  ? TokenKind::Builtin
                                                      : TokenKind::Text;
            push(start, i, kind);
            continue;
        }
        ++i;
        push(start, i, TokenKind::Operator);
    }
    return tokens;
}

} // namespace aether::editor
