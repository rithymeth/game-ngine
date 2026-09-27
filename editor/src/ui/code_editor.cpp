#include "ui/code_editor.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace aether::editor {

namespace {

constexpr ImU32 kColText = IM_COL32(212, 212, 212, 255);
constexpr ImU32 kColKeyword = IM_COL32(197, 134, 192, 255);
constexpr ImU32 kColBuiltin = IM_COL32(78, 201, 176, 255);
constexpr ImU32 kColNumber = IM_COL32(181, 206, 168, 255);
constexpr ImU32 kColString = IM_COL32(206, 145, 120, 255);
constexpr ImU32 kColComment = IM_COL32(106, 153, 85, 255);
constexpr ImU32 kColOperator = IM_COL32(180, 180, 180, 255);
constexpr ImU32 kColLineNumber = IM_COL32(110, 118, 129, 255);
constexpr ImU32 kColSelection = IM_COL32(38, 79, 120, 200);
constexpr ImU32 kColCursor = IM_COL32(230, 230, 230, 255);
constexpr ImU32 kColBreakpoint = IM_COL32(229, 20, 0, 255);
constexpr ImU32 kColExecution = IM_COL32(255, 204, 0, 60);
constexpr ImU32 kColExecutionArrow = IM_COL32(255, 204, 0, 255);
constexpr ImU32 kColError = IM_COL32(229, 20, 0, 50);
constexpr ImU32 kColPopupSelected = IM_COL32(4, 57, 94, 255);

ImU32 ColorOf(TokenKind kind) {
    switch (kind) {
    case TokenKind::Keyword: return kColKeyword;
    case TokenKind::Builtin: return kColBuiltin;
    case TokenKind::Number: return kColNumber;
    case TokenKind::String: return kColString;
    case TokenKind::Comment: return kColComment;
    case TokenKind::Operator: return kColOperator;
    case TokenKind::Text: return kColText;
    }
    return kColText;
}

bool IsNameChar(unsigned int c) { return (c < 128 && (std::isalnum(static_cast<int>(c)) != 0 || c == '_')); }

void AppendUtf8(std::string& out, unsigned int c) {
    if (c < 0x80) {
        out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

float ColumnX(const std::string& line, i32 column) {
    return ImGui::CalcTextSize(line.data(), line.data() + std::min<usize>(line.size(), static_cast<usize>(column))).x;
}

// The column nearest to x (relative to the line's start).
i32 ColumnAt(const std::string& line, float x) {
    if (x <= 0.0f) {
        return 0;
    }
    float left = 0.0f;
    usize i = 0;
    while (i < line.size()) {
        usize next = i + 1;
        while (next < line.size() && (static_cast<unsigned char>(line[next]) & 0xC0) == 0x80) ++next;
        const float right = ImGui::CalcTextSize(line.data(), line.data() + next).x;
        if (x < (left + right) * 0.5f) {
            return static_cast<i32>(i);
        }
        left = right;
        i = next;
    }
    return static_cast<i32>(line.size());
}

void RefreshCompletion(CodeDocument& doc, CodeEditorState& state, const CodeEditorOptions& options, bool force) {
    if (!options.completion) {
        state.completion_open = false;
        return;
    }
    const std::string text = doc.Text();
    const usize cursor = doc.OffsetOf(doc.Cursor());
    state.completion = options.completion(text, cursor);
    const bool empty_prefix = state.completion.replace_from >= cursor;
    // While typing, only open for a name being typed or right after '.'/':'.
    bool wanted = force;
    if (!wanted && cursor > 0) {
        const char before = text[cursor - 1];
        wanted = !empty_prefix || before == '.' || before == ':' || before == '"' || before == '\'';
    }
    state.completion_open = wanted && !state.completion.items.empty();
    // Close when the only suggestion is exactly what's typed.
    if (state.completion_open && state.completion.items.size() == 1 && !empty_prefix &&
        text.compare(state.completion.replace_from, cursor - state.completion.replace_from,
                     state.completion.items[0].label) == 0) {
        state.completion_open = false;
    }
    state.completion_selected = 0;
}

void AcceptCompletion(CodeDocument& doc, CodeEditorState& state) {
    if (!state.completion_open || state.completion.items.empty()) {
        return;
    }
    const CompletionSuggestion& item =
        state.completion.items[static_cast<usize>(std::clamp<i32>(state.completion_selected, 0,
                                                                   static_cast<i32>(state.completion.items.size()) - 1))];
    doc.Replace(doc.PosOf(state.completion.replace_from), doc.Cursor(), item.label);
    state.completion_open = false;
}

} // namespace

CodeEditorResult DrawCodeEditor(const char* id, CodeDocument& doc, CodeEditorState& state,
                                const CodeEditorOptions& options) {
    CodeEditorResult result;
    ImGuiIO& io = ImGui::GetIO();
    const u64 version_before = doc.Version();

    ImGui::PushID(id);
    ImGui::BeginChild("##code", ImVec2(0, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoMove |
                          ImGuiWindowFlags_NoNavInputs);
    if (state.request_focus) {
        ImGui::SetWindowFocus();
        state.focused = true;
        state.request_focus = false;
    }

    const float line_h = ImGui::GetTextLineHeightWithSpacing();
    const float char_w = ImGui::CalcTextSize("M").x;
    const std::vector<std::string>& lines = doc.Lines();
    const i32 line_count = doc.LineCount();
    char digits_buffer[16];
    const int digits = std::snprintf(digits_buffer, sizeof(digits_buffer), "%d", line_count);
    const float marker_w = line_h;                       // breakpoint dot / execution arrow
    const float gutter_w = marker_w + char_w * static_cast<float>(std::max(digits, 3)) + char_w * 1.5f;

    const ImVec2 content = ImGui::GetCursorScreenPos();
    state.gutter_x = content.x;
    state.origin_x = content.x + gutter_w;
    state.origin_y = content.y;
    state.line_height = line_h;
    state.char_width = char_w;

    float widest = 0.0f;
    for (const std::string& line : lines) {
        widest = std::max(widest, static_cast<float>(line.size()) * char_w);
    }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 area(std::max(gutter_w + widest + char_w * 4.0f, avail.x),
                      std::max(static_cast<float>(line_count) * line_h + line_h, avail.y));
    ImGui::InvisibleButton("##text", area, ImGuiButtonFlags_MouseButtonLeft);
    const ImGuiID item_id = ImGui::GetItemID();
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = io.MousePos;

    auto pos_at_mouse = [&]() {
        const i32 line = std::clamp(static_cast<i32>(std::floor((mouse.y - state.origin_y) / line_h)), 0,
                                    line_count - 1);
        return TextPos{line, ColumnAt(lines[static_cast<usize>(line)], mouse.x - state.origin_x)};
    };

    // --- Mouse ----------------------------------------------------------
    if (ImGui::IsItemActivated()) {
        state.focused = true;
        state.completion_open = false;
        if (mouse.x < state.origin_x) {
            const i32 line = static_cast<i32>(std::floor((mouse.y - state.origin_y) / line_h));
            if (line >= 0 && line < line_count) {
                if (!state.breakpoints.erase(line)) {
                    state.breakpoints.insert(line);
                }
                result.toggled_breakpoint = line;
            }
        } else {
            doc.SetCursor(pos_at_mouse(), io.KeyShift);
        }
    } else if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && mouse.x >= state.gutter_x) {
        doc.SetCursor(pos_at_mouse(), true);
        state.scroll_to_cursor = true;
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !hovered && !ImGui::IsWindowHovered()) {
        state.focused = false; // clicked somewhere else
        state.completion_open = false;
    }
    if (hovered && mouse.x < state.origin_x && options.error_line >= 0 && !options.error_message.empty() &&
        static_cast<i32>(std::floor((mouse.y - state.origin_y) / line_h)) == options.error_line) {
        ImGui::SetTooltip("%s", options.error_message.c_str());
    }

    // --- Keyboard -------------------------------------------------------
    const bool has_keys = state.focused && ImGui::IsWindowFocused();
    if (has_keys) {
        for (ImGuiKey key : {ImGuiKey_Tab, ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Escape, ImGuiKey_UpArrow,
                             ImGuiKey_DownArrow, ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_PageUp,
                             ImGuiKey_PageDown, ImGuiKey_Home, ImGuiKey_End, ImGuiKey_Space}) {
            ImGui::SetKeyOwner(key, item_id);
        }
        io.WantCaptureKeyboard = true;
        io.WantTextInput = true;

        const bool ctrl = io.KeyCtrl;
        const bool shift = io.KeyShift;
        const bool editable = !options.read_only;
        auto pressed = [](ImGuiKey key) { return ImGui::IsKeyPressed(key, true); };
        bool moved = false;
        bool refresh = false;

        if (state.completion_open) {
            const i32 count = static_cast<i32>(state.completion.items.size());
            if (pressed(ImGuiKey_DownArrow)) {
                state.completion_selected = (state.completion_selected + 1) % count;
            } else if (pressed(ImGuiKey_UpArrow)) {
                state.completion_selected = (state.completion_selected + count - 1) % count;
            } else if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter) || pressed(ImGuiKey_Tab)) {
                if (editable) AcceptCompletion(doc, state);
                state.completion_open = false;
            } else if (pressed(ImGuiKey_Escape)) {
                state.completion_open = false;
            }
        } else {
            if (pressed(ImGuiKey_UpArrow)) { doc.MoveUp(shift); moved = true; }
            if (pressed(ImGuiKey_DownArrow)) { doc.MoveDown(shift); moved = true; }
            if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) {
                if (editable) doc.NewLine();
                moved = true;
            }
            if (pressed(ImGuiKey_Tab) && editable) {
                shift ? doc.Unindent() : doc.Indent();
                moved = true;
            }
            if (pressed(ImGuiKey_Escape)) doc.ClearSelection();
        }
        if (pressed(ImGuiKey_LeftArrow)) {
            ctrl ? doc.MoveWordLeft(shift) : doc.MoveLeft(shift);
            moved = true;
            state.completion_open = false;
        }
        if (pressed(ImGuiKey_RightArrow)) {
            ctrl ? doc.MoveWordRight(shift) : doc.MoveRight(shift);
            moved = true;
            state.completion_open = false;
        }
        if (pressed(ImGuiKey_Home)) {
            ctrl ? doc.MoveToStart(shift) : doc.MoveHome(shift);
            moved = true;
        }
        if (pressed(ImGuiKey_End)) {
            ctrl ? doc.MoveToEnd(shift) : doc.MoveEnd(shift);
            moved = true;
        }
        const i32 page = std::max(1, static_cast<i32>(ImGui::GetWindowHeight() / line_h) - 1);
        if (pressed(ImGuiKey_PageUp)) {
            doc.SetCursor({doc.Cursor().line - page, doc.Cursor().column}, shift);
            moved = true;
        }
        if (pressed(ImGuiKey_PageDown)) {
            doc.SetCursor({doc.Cursor().line + page, doc.Cursor().column}, shift);
            moved = true;
        }
        if (pressed(ImGuiKey_Backspace) && editable) {
            doc.Backspace();
            refresh = state.completion_open;
        }
        if (pressed(ImGuiKey_Delete) && editable) {
            doc.Delete();
            state.completion_open = false;
        }
        if (ctrl) {
            if (pressed(ImGuiKey_A)) doc.SelectAll();
            if (pressed(ImGuiKey_C) && doc.HasSelection()) ImGui::SetClipboardText(doc.SelectedText().c_str());
            if (pressed(ImGuiKey_X) && doc.HasSelection() && editable) {
                ImGui::SetClipboardText(doc.SelectedText().c_str());
                doc.DeleteSelection();
            }
            if (pressed(ImGuiKey_V) && editable) {
                if (const char* clip = ImGui::GetClipboardText()) doc.Paste(clip);
            }
            if (pressed(ImGuiKey_Z) && editable) shift ? doc.Redo() : doc.Undo();
            if (pressed(ImGuiKey_Y) && editable) doc.Redo();
            if (pressed(ImGuiKey_Slash) && editable) doc.ToggleComment();
            if (pressed(ImGuiKey_S)) result.save_requested = true;
            if (pressed(ImGuiKey_Space) && options.completion) {
                RefreshCompletion(doc, state, options, true);
            }
        }
        if (moved) {
            state.scroll_to_cursor = true;
        }

        // Typed characters (Ctrl+letter shortcuts don't type).
        if (!ctrl && editable && !io.InputQueueCharacters.empty()) {
            std::string typed;
            unsigned int last = 0;
            for (ImWchar c : io.InputQueueCharacters) {
                if (c >= 32 && c != 127) {
                    AppendUtf8(typed, c);
                    last = c;
                }
            }
            for (usize i = 0; i < typed.size();) {
                usize next = i + 1;
                while (next < typed.size() && (static_cast<unsigned char>(typed[next]) & 0xC0) == 0x80) ++next;
                doc.Type(std::string_view(typed).substr(i, next - i));
                i = next;
            }
            if (!typed.empty()) {
                state.scroll_to_cursor = true;
                if (options.auto_complete && (IsNameChar(last) || last == '.' || last == ':' || last == '"' ||
                                              last == '\'')) {
                    refresh = true;
                } else {
                    state.completion_open = false;
                }
            }
        }
        if (!io.InputQueueCharacters.empty()) {
            io.InputQueueCharacters.resize(0); // consumed: other widgets don't also type them
        }
        if (refresh) {
            RefreshCompletion(doc, state, options, false);
        }
    }

    // --- Scrolling ------------------------------------------------------
    if (state.scroll_to_cursor) {
        state.scroll_to_cursor = false;
        const TextPos cursor = doc.Cursor();
        const float y = static_cast<float>(cursor.line) * line_h;
        const float window_h = ImGui::GetWindowHeight();
        if (y < ImGui::GetScrollY()) ImGui::SetScrollY(y);
        else if (y + line_h * 2.0f > ImGui::GetScrollY() + window_h) ImGui::SetScrollY(y + line_h * 2.0f - window_h);
        const float x = gutter_w + ColumnX(lines[static_cast<usize>(cursor.line)], cursor.column);
        const float window_w = ImGui::GetWindowWidth();
        if (x < ImGui::GetScrollX() + gutter_w) ImGui::SetScrollX(std::max(0.0f, x - gutter_w));
        else if (x + char_w * 2.0f > ImGui::GetScrollX() + window_w) ImGui::SetScrollX(x + char_w * 2.0f - window_w);
    }

    // --- Drawing --------------------------------------------------------
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const i32 first = std::max(0, static_cast<i32>(ImGui::GetScrollY() / line_h) - 1);
    const i32 last = std::min(line_count, first + static_cast<i32>(ImGui::GetWindowHeight() / line_h) + 3);
    const TextPos sel_start = doc.SelectionStart();
    const TextPos sel_end = doc.SelectionEnd();
    const float text_right = content.x + area.x;
    LexState lex;
    for (i32 l = 0; l < last; ++l) {
        const std::string& line = lines[static_cast<usize>(l)];
        const std::vector<Token> tokens = TokenizeLuauLine(line, lex); // always: carries long comments along
        if (l < first) {
            continue;
        }
        const float y = state.origin_y + static_cast<float>(l) * line_h;
        if (l == options.execution_line) {
            draw->AddRectFilled(ImVec2(state.origin_x, y), ImVec2(text_right, y + line_h), kColExecution);
        } else if (l == options.error_line) {
            draw->AddRectFilled(ImVec2(state.origin_x, y), ImVec2(text_right, y + line_h), kColError);
        }
        if (doc.HasSelection() && l >= sel_start.line && l <= sel_end.line) {
            const float x0 = state.origin_x + (l == sel_start.line ? ColumnX(line, sel_start.column) : 0.0f);
            const float x1 = state.origin_x + (l == sel_end.line ? ColumnX(line, sel_end.column)
                                                                 : ColumnX(line, static_cast<i32>(line.size())) + char_w);
            draw->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + line_h), kColSelection);
        }
        for (const Token& token : tokens) {
            const float x = state.origin_x + ColumnX(line, token.start);
            draw->AddText(ImVec2(x, y), ColorOf(token.kind), line.data() + token.start, line.data() + token.end);
        }
        // Gutter.
        char number[16];
        const int length = std::snprintf(number, sizeof(number), "%d", l + 1);
        const float number_w = ImGui::CalcTextSize(number, number + length).x;
        draw->AddText(ImVec2(state.origin_x - char_w * 1.0f - number_w, y), kColLineNumber, number, number + length);
        const ImVec2 marker(state.gutter_x + marker_w * 0.5f, y + line_h * 0.5f);
        if (state.breakpoints.count(l) != 0) {
            draw->AddCircleFilled(marker, line_h * 0.3f, kColBreakpoint);
        }
        if (l == options.execution_line) {
            const float r = line_h * 0.3f;
            draw->AddTriangleFilled(ImVec2(marker.x - r, marker.y - r), ImVec2(marker.x + r, marker.y),
                                    ImVec2(marker.x - r, marker.y + r), kColExecutionArrow);
        }
        if (l == options.error_line) {
            draw->AddLine(ImVec2(state.origin_x, y + line_h - 1.0f),
                          ImVec2(state.origin_x + ColumnX(line, static_cast<i32>(line.size())), y + line_h - 1.0f),
                          kColBreakpoint, 1.5f);
        }
        if (state.focused && l == doc.Cursor().line) {
            const float x = state.origin_x + ColumnX(line, doc.Cursor().column);
            draw->AddLine(ImVec2(x, y), ImVec2(x, y + line_h - 1.0f), kColCursor, 1.5f);
        }
    }

    // --- Completion popup -------------------------------------------------
    if (state.completion_open && has_keys && !state.completion.items.empty()) {
        const TextPos cursor = doc.Cursor();
        const float x = state.origin_x + ColumnX(lines[static_cast<usize>(cursor.line)], cursor.column);
        const float y = state.origin_y + static_cast<float>(cursor.line + 1) * line_h;
        ImGui::SetNextWindowPos(ImVec2(x, y));
        ImGui::BeginTooltip();
        const i32 count = static_cast<i32>(state.completion.items.size());
        constexpr i32 kShown = 10;
        const i32 top = std::clamp(state.completion_selected - kShown / 2, 0, std::max(0, count - kShown));
        for (i32 i = top; i < std::min(count, top + kShown); ++i) {
            const CompletionSuggestion& item = state.completion.items[static_cast<usize>(i)];
            if (i == state.completion_selected) {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(
                    p, ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y + ImGui::GetTextLineHeight()),
                    kColPopupSelected);
            }
            ImGui::TextUnformatted(item.label.c_str());
            if (!item.detail.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", item.detail.c_str());
            }
        }
        if (count > kShown) {
            ImGui::TextDisabled("%d of %d", state.completion_selected + 1, count);
        }
        ImGui::EndTooltip();
    }

    ImGui::EndChild();
    ImGui::PopID();
    result.changed = doc.Version() != version_before;
    return result;
}

} // namespace aether::editor
