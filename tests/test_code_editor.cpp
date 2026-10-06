#include "test_framework.h"
#include "ui/code_editor.h"

#include <imgui.h>

#include <functional>

using namespace aether;
using namespace aether::editor;

namespace {

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGui::GetIO().ConfigMacOSXBehaviors = false; // tests press Ctrl on every platform
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1024, 768);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(context_); }
    void Frame(const std::function<void()>& body) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(800, 600));
        ImGui::Begin("Script");
        body();
        ImGui::End();
        ImGui::Render();
    }

private:
    ImGuiContext* context_ = nullptr;
};

// The editor under test, one frame at a time.
struct EditorHarness {
    HeadlessImGui ui;
    CodeDocument doc;
    CodeEditorState state;
    CodeEditorOptions options;
    CodeEditorResult last;
    bool any_changed = false;
    i32 toggled = -1;
    bool saved = false;

    void Frame() {
        ui.Frame([&] {
            last = DrawCodeEditor("editor", doc, state, options);
            any_changed |= last.changed;
            if (last.toggled_breakpoint >= 0) toggled = last.toggled_breakpoint;
            saved |= last.save_requested;
        });
    }
    void Focus() {
        state.request_focus = true;
        Frame();
        Frame();
    }
    void Type(const char* text) {
        ImGui::GetIO().AddInputCharactersUTF8(text);
        Frame();
    }
    void Press(ImGuiKey key, bool ctrl = false, bool shift = false) {
        ImGuiIO& io = ImGui::GetIO();
        if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, true);
        if (shift) io.AddKeyEvent(ImGuiMod_Shift, true);
        io.AddKeyEvent(key, true);
        Frame();
        io.AddKeyEvent(key, false);
        if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, false);
        if (shift) io.AddKeyEvent(ImGuiMod_Shift, false);
        Frame();
    }
    void Click(float x, float y) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(x, y);
        Frame();
        io.AddMouseButtonEvent(0, true);
        Frame();
        io.AddMouseButtonEvent(0, false);
        Frame();
    }
    // Screen position of the middle of (line, column), monospace font.
    ImVec2 At(i32 line, i32 column) const {
        return ImVec2(state.origin_x + (static_cast<float>(column) + 0.1f) * state.char_width,
                      state.origin_y + (static_cast<float>(line) + 0.5f) * state.line_height);
    }
};

// Types one character at a time, as the keyboard does.
void TypeEach(CodeDocument& doc, std::string_view text) {
    for (char c : text) doc.Type(std::string_view(&c, 1));
}

std::vector<TokenKind> Kinds(const std::vector<Token>& tokens) {
    std::vector<TokenKind> kinds;
    for (const Token& t : tokens) kinds.push_back(t.kind);
    return kinds;
}

} // namespace

AETHER_TEST(CodeDocument_EditingMovingAndOffsets) {
    CodeDocument doc("local a = 1\r\n\tprint(a)\nreturn a");
    AETHER_CHECK(doc.LineCount() == 3);
    AETHER_CHECK(doc.Lines()[1] == "    print(a)"); // CRLF and tabs normalized
    AETHER_CHECK(!doc.Dirty());
    AETHER_CHECK(doc.OffsetOf({1, 4}) == 16 && doc.PosOf(16) == (TextPos{1, 4}));
    AETHER_CHECK(doc.PosOf(10000) == (TextPos{2, 8}));
    AETHER_CHECK(doc.Clamp({9, 99}) == (TextPos{2, 8}));

    // Arrows wrap lines; words; Home toggles between indent and column 0.
    doc.SetCursor({1, 0});
    doc.MoveLeft();
    AETHER_CHECK(doc.Cursor() == (TextPos{0, 11}));
    doc.MoveRight();
    AETHER_CHECK(doc.Cursor() == (TextPos{1, 0}));
    doc.MoveHome();
    AETHER_CHECK(doc.Cursor().column == 4);
    doc.MoveHome();
    AETHER_CHECK(doc.Cursor().column == 0);
    doc.SetCursor({0, 0});
    doc.MoveWordRight();
    AETHER_CHECK(doc.Cursor().column == 6); // after "local "
    doc.MoveWordRight(true);
    AETHER_CHECK(doc.SelectedText() == "a ");
    doc.MoveEnd();
    doc.MoveWordLeft();
    AETHER_CHECK(doc.Cursor().column == 10);

    // UTF-8: the cursor never lands inside a code point.
    CodeDocument utf("x = \"h\xC3\xA9\"");
    utf.SetCursor({0, 7});
    AETHER_CHECK(utf.Cursor().column == 6); // clamped to the start of "é"
    utf.MoveRight();
    AETHER_CHECK(utf.Cursor().column == 8);
    utf.Backspace();
    AETHER_CHECK(utf.Text() == "x = \"h\"");

    // Typing replaces the selection; Delete joins lines; Backspace at col 0 too.
    doc.Select({0, 6}, {0, 7});
    doc.Type("b");
    AETHER_CHECK(doc.Lines()[0] == "local b = 1" && doc.Dirty());
    doc.SetCursor({0, 11});
    doc.Delete();
    AETHER_CHECK(doc.LineCount() == 2 && doc.Lines()[0] == "local b = 1    print(a)");
    doc.SetCursor({1, 0});
    doc.Backspace();
    AETHER_CHECK(doc.LineCount() == 1);
    doc.SelectAll();
    AETHER_CHECK(doc.SelectedText() == doc.Text());
    doc.Paste("one\ntwo");
    AETHER_CHECK(doc.Text() == "one\ntwo" && doc.Cursor() == (TextPos{1, 3}));
}

AETHER_TEST(CodeDocument_AutoIndentTabsAndComments) {
    CodeDocument doc;
    TypeEach(doc, "function Spin:OnUpdate(dt)\nif dt > 0 then\nmove()\nend\nend");
    AETHER_CHECK(doc.Text() == "function Spin:OnUpdate(dt)\n    if dt > 0 then\n        move()\n    end\nend");

    // Enter in the middle keeps the indent and moves the rest down.
    CodeDocument split("    a = 1 b = 2");
    split.SetCursor({0, 10});
    split.NewLine();
    AETHER_CHECK(split.Text() == "    a = 1\n    b = 2" && split.Cursor() == (TextPos{1, 4}));
    // A comment doesn't open a block; "else" dedents.
    CodeDocument branch("if x then -- note");
    branch.SetCursor({0, 17});
    branch.NewLine();
    AETHER_CHECK(branch.Cursor().column == 4);
    TypeEach(branch, "a\nelse");
    AETHER_CHECK(branch.Lines()[2] == "else");

    // Backspace in the indentation goes back an indent stop.
    CodeDocument indent("        x");
    indent.SetCursor({0, 8});
    indent.Backspace();
    AETHER_CHECK(indent.Lines()[0] == "    x");

    // Tab inserts to the next stop; with a selection it indents lines.
    CodeDocument tab("ab\ncd\n\nef");
    tab.SetCursor({0, 1});
    tab.Indent();
    AETHER_CHECK(tab.Lines()[0] == "a   b");
    tab.Select({1, 1}, {3, 1});
    tab.Indent();
    AETHER_CHECK(tab.Lines()[1] == "    cd" && tab.Lines()[2].empty() && tab.Lines()[3] == "    ef");
    AETHER_CHECK(tab.SelectedText() == "d\n\n    e");
    tab.Unindent();
    AETHER_CHECK(tab.Lines()[1] == "cd" && tab.Lines()[3] == "ef");

    // Ctrl+/ comments at the common indent and uncomments.
    CodeDocument comment("if a then\n    b()\nend");
    comment.Select({0, 0}, {2, 3});
    comment.ToggleComment();
    AETHER_CHECK(comment.Text() == "-- if a then\n--     b()\n-- end");
    comment.ToggleComment();
    AETHER_CHECK(comment.Text() == "if a then\n    b()\nend");
}

AETHER_TEST(CodeDocument_UndoRedoDirtyAndFind) {
    CodeDocument doc("x");
    doc.SetCursor({0, 1});
    for (const char* c : {"y", "z", " ", "w"}) doc.Type(c);
    AETHER_CHECK(doc.Text() == "xyz w");
    // A run of typing is one step; the space is its own.
    AETHER_CHECK(doc.Undo() && doc.Text() == "xyz ");
    AETHER_CHECK(doc.Undo() && doc.Text() == "xyz");
    AETHER_CHECK(doc.Undo() && doc.Text() == "x");
    AETHER_CHECK(!doc.Dirty()); // back to what was loaded
    AETHER_CHECK(!doc.Undo());
    AETHER_CHECK(doc.Redo() && doc.Text() == "xyz" && doc.Dirty());
    doc.MarkSaved();
    AETHER_CHECK(!doc.Dirty());
    doc.Type("!");
    AETHER_CHECK(doc.Dirty() && !doc.CanRedo()); // a new edit drops the redo history
    doc.Undo();
    AETHER_CHECK(!doc.Dirty());
    // Moving ends a run.
    doc.MoveLeft();
    doc.Type("a");
    doc.MoveEnd();
    doc.Type("b");
    AETHER_CHECK(doc.Undo() && doc.Text() == "xyaz");

    CodeDocument find("Speed = 1\nlocal speed = speed * 2");
    AETHER_CHECK(find.Find("speed") && find.SelectionStart() == (TextPos{0, 0}));
    AETHER_CHECK(find.Find("speed") && find.SelectionStart() == (TextPos{1, 6}));
    AETHER_CHECK(find.Find("speed") && find.SelectionStart() == (TextPos{1, 14}));
    AETHER_CHECK(find.Find("speed") && find.SelectionStart() == (TextPos{0, 0})); // wrapped
    AETHER_CHECK(find.Find("speed", false) && find.SelectionStart() == (TextPos{1, 14}));
    AETHER_CHECK(find.Find("speed", true, true) && find.SelectionStart() == (TextPos{1, 6}));
    AETHER_CHECK(!find.Find("nothing"));
}

AETHER_TEST(CodeDocument_TokenizesLuau) {
    LexState state;
    std::vector<Token> t = TokenizeLuauLine("local x = self.speed * 2.5e-3 -- fast", state);
    AETHER_CHECK((Kinds(t) == std::vector<TokenKind>{TokenKind::Keyword, TokenKind::Text, TokenKind::Operator,
                                                    TokenKind::Builtin, TokenKind::Operator, TokenKind::Text,
                                                    TokenKind::Operator, TokenKind::Number, TokenKind::Comment}));
    AETHER_CHECK(t[7].start == 23 && t[7].end == 29);

    // Strings with escapes; a method named like a keyword isn't one.
    t = TokenizeLuauLine("print(\"a \\\" b\", t.end, 'x')", state);
    AETHER_CHECK(t[0].kind == TokenKind::Builtin && t[2].kind == TokenKind::String && t[2].end == 14);
    AETHER_CHECK(t[6].kind == TokenKind::Text); // "end" after '.'

    // Long comments and strings carry across lines.
    t = TokenizeLuauLine("x = 1 --[[ starts", state);
    AETHER_CHECK(t.back().kind == TokenKind::Comment && state.open == LexState::Open::Comment);
    t = TokenizeLuauLine("still inside", state);
    AETHER_CHECK(t.size() == 1 && t[0].kind == TokenKind::Comment && state.open == LexState::Open::Comment);
    t = TokenizeLuauLine("ends ]] y = 2", state);
    AETHER_CHECK(t[0].kind == TokenKind::Comment && t[0].end == 7 && state.open == LexState::Open::None);
    AETHER_CHECK(t[1].kind == TokenKind::Text);
    t = TokenizeLuauLine("s = [==[ a ]] b", state);
    AETHER_CHECK(state.open == LexState::Open::String && state.level == 2);
    t = TokenizeLuauLine("]==]", state);
    AETHER_CHECK(t.size() == 1 && t[0].kind == TokenKind::String && state.open == LexState::Open::None);
}

AETHER_TEST(CodeEditor_TypesEditsAndShortcuts) {
    EditorHarness h;
    h.doc.SetText("local a = 1");
    h.Frame();
    AETHER_CHECK(h.state.line_height > 0.0f && h.state.char_width > 0.0f);

    // Keys do nothing until focused.
    h.Type("zz");
    AETHER_CHECK(h.doc.Text() == "local a = 1" && !h.any_changed);

    h.Focus();
    AETHER_CHECK(h.state.focused);
    h.Press(ImGuiKey_End);
    h.Type(" + 2");
    AETHER_CHECK(h.doc.Text() == "local a = 1 + 2" && h.any_changed);
    h.Press(ImGuiKey_Enter);
    h.Type("if a then");
    h.Press(ImGuiKey_Enter);
    h.Type("b()");
    AETHER_CHECK(h.doc.Text() == "local a = 1 + 2\nif a then\n    b()");
    h.Press(ImGuiKey_Backspace);
    h.Press(ImGuiKey_LeftArrow, false, true); // Shift+Left selects "("
    AETHER_CHECK(h.doc.SelectedText() == "(");
    h.Press(ImGuiKey_A, true);
    AETHER_CHECK(h.doc.SelectedText() == h.doc.Text());
    h.Press(ImGuiKey_C, true);
    AETHER_CHECK(std::string(ImGui::GetClipboardText()) == h.doc.Text());
    h.Press(ImGuiKey_Z, true);
    AETHER_CHECK(h.doc.Text() == "local a = 1 + 2\nif a then\n    b()");
    h.Press(ImGuiKey_Y, true);
    AETHER_CHECK(h.doc.Text() == "local a = 1 + 2\nif a then\n    b(");
    h.Press(ImGuiKey_S, true);
    AETHER_CHECK(h.saved);
    AETHER_CHECK(h.doc.Text() == "local a = 1 + 2\nif a then\n    b("); // Ctrl+letters don't type

    // Tab indents inside the editor instead of moving focus.
    h.Press(ImGuiKey_Home);
    h.Press(ImGuiKey_Tab);
    AETHER_CHECK(h.doc.Lines()[2] == "        b(" && h.state.focused);
    h.Press(ImGuiKey_Slash, true);
    AETHER_CHECK(h.doc.Lines()[2] == "        -- b(");

    // Read-only: moves but doesn't edit.
    h.options.read_only = true;
    h.any_changed = false;
    h.Type("nope");
    h.Press(ImGuiKey_Backspace);
    AETHER_CHECK(!h.any_changed);
}

AETHER_TEST(CodeEditor_MouseGutterAndCompletion) {
    EditorHarness h;
    h.doc.SetText("local speed = 1\nlocal spin = 2\nprint(sp)");
    h.options.execution_line = 1;
    h.options.error_line = 2;
    h.options.error_message = "bad";
    std::vector<usize> asked;
    h.options.completion = [&](const std::string& text, usize cursor) {
        asked.push_back(cursor);
        CompletionReply reply;
        usize from = cursor;
        while (from > 0 && (std::isalnum(static_cast<unsigned char>(text[from - 1])) != 0 || text[from - 1] == '_')) {
            --from;
        }
        reply.replace_from = from;
        const std::string prefix = text.substr(from, cursor - from);
        for (const char* name : {"speed", "spin", "sprint"}) {
            if (std::string(name).rfind(prefix, 0) == 0) reply.items.push_back({name, "number"});
        }
        return reply;
    };
    h.Frame();

    // Clicking text places the cursor (and focuses); Shift+click selects.
    const ImVec2 p = h.At(1, 6);
    h.Click(p.x, p.y);
    AETHER_CHECK(h.state.focused && h.doc.Cursor() == (TextPos{1, 6}));
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, true);
    const ImVec2 q = h.At(1, 10);
    h.Click(q.x, q.y);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, false);
    h.Frame();
    AETHER_CHECK(h.doc.SelectedText() == "spin");

    // The gutter toggles breakpoints.
    h.Click(h.state.gutter_x + 4.0f, h.At(0, 0).y);
    AETHER_CHECK(h.toggled == 0 && h.state.breakpoints.count(0) == 1);
    h.toggled = -1;
    h.Click(h.state.gutter_x + 4.0f, h.At(0, 0).y);
    AETHER_CHECK(h.toggled == 0 && h.state.breakpoints.empty());
    h.Click(h.state.gutter_x + 4.0f, h.At(2, 0).y);
    AETHER_CHECK(h.state.breakpoints == std::set<i32>{2});
    AETHER_CHECK(h.doc.Text() == "local speed = 1\nlocal spin = 2\nprint(sp)"); // gutter clicks don't edit

    // Typing a name opens the popup; Down + Enter accepts the second item.
    const ImVec2 r = h.At(2, 8);
    h.Click(r.x, r.y);
    AETHER_CHECK(h.doc.Cursor() == (TextPos{2, 8}));
    h.Type("r");
    AETHER_CHECK(h.state.completion_open && h.state.completion.items.size() == 1);
    h.Press(ImGuiKey_Backspace);
    AETHER_CHECK(h.state.completion_open && h.state.completion.items.size() == 3);
    h.Press(ImGuiKey_DownArrow);
    AETHER_CHECK(h.state.completion_selected == 1 && h.doc.Cursor() == (TextPos{2, 8})); // popup took the arrow
    h.Press(ImGuiKey_Enter);
    AETHER_CHECK(!h.state.completion_open);
    AETHER_CHECK(h.doc.Lines()[2] == "print(spin)");
    AETHER_CHECK(h.doc.Undo() && h.doc.Lines()[2] == "print(sp)");

    // Escape closes; Ctrl+Space reopens; a non-name character closes it.
    h.doc.SetCursor({2, 8});
    h.Type("e");
    AETHER_CHECK(h.state.completion_open);
    h.Press(ImGuiKey_Escape);
    AETHER_CHECK(!h.state.completion_open);
    h.Press(ImGuiKey_Space, true);
    AETHER_CHECK(h.state.completion_open && h.state.completion.items[0].label == "speed");
    h.Type(")");
    AETHER_CHECK(!h.state.completion_open);
    AETHER_CHECK(!asked.empty());

    // Clicking outside the editor drops focus.
    h.Frame();
    h.Click(900.0f, 700.0f);
    AETHER_CHECK(!h.state.focused);
}
