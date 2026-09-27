#include "graph/blueprint_editor.h"
#include "test_framework.h"

#include "aether/blueprint/nodes.h"

#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <functional>

using namespace aether;
using namespace aether::editor;
using nlohmann::json;

namespace {

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1400, 900);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(context_); }
    void Frame(const std::function<void()>& body) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1400, 900));
        ImGui::Begin("BP_Door", nullptr, ImGuiWindowFlags_NoMove);
        body();
        ImGui::End();
        ImGui::Render();
    }

private:
    ImGuiContext* context_ = nullptr;
};

// BP_Door: BeginPlay -> Branch(Get IsOpen) -> false -> Print.
bp::Blueprint Door(bp::NodeId& begin, bp::NodeId& branch, bp::NodeId& get, bp::NodeId& print) {
    bp::Blueprint blueprint;
    bp::Variable open;
    open.name = "IsOpen";
    open.type = bp::PinType::Of(bp::ValueType::Bool);
    open.default_value = false;
    blueprint.variables = {open};
    bp::Graph g;
    g.name = "EventGraph";
    bp::GraphBuilder b(g);
    begin = b.Add("Event.BeginPlay", json::object(), 0, 0);
    branch = b.Add("Flow.Branch", json::object(), 260, 0);
    get = b.Add("Var.Get:IsOpen", json::object(), 100, 140);
    print = b.Add("Debug.Print", json::object(), 520, 0);
    b.Connect(begin, "then", branch, "exec").Connect(get, "value", branch, "condition").Connect(branch, "false", print, "exec");
    blueprint.graphs.push_back(g);
    return blueprint;
}

bool HasLink(const bp::Graph& g, bp::NodeId from, const std::string& from_pin, bp::NodeId to, const std::string& to_pin) {
    return std::any_of(g.links.begin(), g.links.end(), [&](const bp::Link& l) {
        return l.from.node == from && l.from.pin == from_pin && l.to.node == to && l.to.pin == to_pin;
    });
}

usize CountType(const bp::Blueprint& b, const std::string& type) {
    usize n = 0;
    for (const bp::Graph& g : b.graphs) {
        n += static_cast<usize>(std::count_if(g.nodes.begin(), g.nodes.end(), [&](const bp::Node& x) { return x.type == type; }));
    }
    return n;
}

} // namespace

AETHER_TEST(BlueprintDocument_UndoRedoMergesAndSaves) {
    bp::NodeId begin, branch, get, print;
    BlueprintDocument doc(Door(begin, branch, get, print));
    AETHER_CHECK(!doc.Dirty() && !doc.CanUndo() && doc.Name() == "Untitled");

    AETHER_CHECK(doc.AddVariable(bp::PinType::Of(bp::ValueType::Float)) == "NewVar");
    AETHER_CHECK(doc.AddVariable() == "NewVar_1");
    AETHER_CHECK(doc.Dirty() && doc.UndoLabel() == "Add variable NewVar_1");
    AETHER_CHECK(doc.Undo() && doc.Get().FindVariable("NewVar_1") == nullptr && doc.CanRedo());
    AETHER_CHECK(doc.Redo() && doc.Get().FindVariable("NewVar_1") != nullptr);

    // Typing in one field is one undo step; a new edit clears redo.
    for (const char* text : {"D", "Do", "Door"}) {
        doc.Edit("Set tooltip", [&](bp::Blueprint& b) { b.variables[0].tooltip = text; }, "tooltip:IsOpen");
    }
    AETHER_CHECK(doc.Get().variables[0].tooltip == "Door");
    AETHER_CHECK(doc.Undo() && doc.Get().variables[0].tooltip.empty());
    AETHER_CHECK(doc.Redo() && doc.Get().variables[0].tooltip == "Door" && !doc.CanRedo());

    // Save, then load it back.
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "aether_test_bp_door.abp";
    std::string error;
    AETHER_CHECK(!doc.Save(&error) && !error.empty()); // no path yet
    AETHER_CHECK(doc.SaveAs(path, &error) && !doc.Dirty() && doc.Name() == "aether_test_bp_door");
    BlueprintDocument loaded;
    AETHER_CHECK(loaded.Load(path, &error) && loaded.Get().variables.size() == 3 && loaded.Get().variables[0].tooltip == "Door");
    std::filesystem::remove(path);

    // Compile status goes stale on an edit.
    AETHER_CHECK(doc.CompileStatus() == BlueprintDocument::Status::NotCompiled);
    AETHER_CHECK(doc.Compile().Ok() && doc.CompileStatus() == BlueprintDocument::Status::UpToDate);
    doc.AddVariable();
    AETHER_CHECK(doc.CompileStatus() == BlueprintDocument::Status::Stale);
    doc.Edit("Break", [&](bp::Blueprint& b) { bp::GraphBuilder(b.graphs[0]).Add("Flow.Teleport"); });
    AETHER_CHECK(!doc.Compile().Ok() && doc.CompileStatus() == BlueprintDocument::Status::Errors);
}

AETHER_TEST(BlueprintDocument_RenamesAndRemovesFollowReferences) {
    bp::NodeId begin, branch, get, print;
    BlueprintDocument doc(Door(begin, branch, get, print));
    std::string error;

    // Variables: Get/Set nodes follow a rename and go with a removal.
    doc.Edit("Add Set", [&](bp::Blueprint& b) { bp::GraphBuilder(b.graphs[0]).Add("Var.Set:IsOpen"); });
    AETHER_CHECK(doc.RenameVariable("IsOpen", "Opened", &error));
    AETHER_CHECK(CountType(doc.Get(), "Var.Get:Opened") == 1 && CountType(doc.Get(), "Var.Set:Opened") == 1);
    AETHER_CHECK(!doc.RenameVariable("Opened", "2fast", &error) && error.find("valid") != std::string::npos);
    doc.AddVariable();
    AETHER_CHECK(!doc.RenameVariable("Opened", "NewVar", &error) && error.find("already") != std::string::npos);
    AETHER_CHECK(bp::ValidateBlueprint(doc.Get()).Ok());

    // A type change breaks links that no longer fit (Vec3 -> Branch's bool).
    AETHER_CHECK(HasLink(doc.Get().graphs[0], get, "value", branch, "condition"));
    AETHER_CHECK(doc.SetVariableType("Opened", bp::PinType::Of(bp::ValueType::Vec3)));
    AETHER_CHECK(!HasLink(doc.Get().graphs[0], get, "value", branch, "condition"));
    AETHER_CHECK(doc.Get().FindVariable("Opened")->default_value.index() != 0);
    AETHER_CHECK(doc.Undo() && HasLink(doc.Get().graphs[0], get, "value", branch, "condition"));
    AETHER_CHECK(doc.RemoveVariable("Opened") && CountType(doc.Get(), "Var.Get:Opened") == 0 &&
                 CountType(doc.Get(), "Var.Set:Opened") == 0);
    AETHER_CHECK(std::none_of(doc.Get().graphs[0].links.begin(), doc.Get().graphs[0].links.end(),
                              [&](const bp::Link& l) { return l.from.node == get; }));

    // Functions: made with linked Entry and Return; calls follow a rename.
    const std::string fn = doc.AddFunction();
    AETHER_CHECK(fn == "NewFunction");
    const bp::Graph* f = doc.Get().FindGraph(fn);
    AETHER_CHECK(f != nullptr && f->kind == bp::GraphKind::Function && f->nodes.size() == 2 && f->links.size() == 1);
    doc.Edit("Call", [&](bp::Blueprint& b) { bp::GraphBuilder(b.graphs[0]).Add("Call.Self:NewFunction"); });
    AETHER_CHECK(doc.RenameGraph(fn, "OpenDoor", &error) && CountType(doc.Get(), "Call.Self:OpenDoor") == 1);
    AETHER_CHECK(!doc.RenameGraph("EventGraph", "Main", &error));
    AETHER_CHECK(!doc.RemoveGraph("EventGraph", &error) && error.find("Event Graph") != std::string::npos);
    const std::string macro = doc.AddMacro();
    AETHER_CHECK(doc.Get().FindGraph(macro)->kind == bp::GraphKind::Macro);
    AETHER_CHECK(bp::ValidateBlueprint(doc.Get()).Ok());
    AETHER_CHECK(doc.RemoveGraph("OpenDoor", &error) && CountType(doc.Get(), "Call.Self:OpenDoor") == 0);

    // Dispatchers: this Blueprint's Call nodes follow and go.
    const std::string d = doc.AddDispatcher();
    doc.Edit("Call dispatcher", [&](bp::Blueprint& b) { bp::GraphBuilder(b.graphs[0]).Add("Dispatch.Call:" + d); });
    AETHER_CHECK(doc.RenameDispatcher(d, "OnOpened", &error) && CountType(doc.Get(), "Dispatch.Call:OnOpened") == 1);
    AETHER_CHECK(doc.RemoveDispatcher("OnOpened") && CountType(doc.Get(), "Dispatch.Call:OnOpened") == 0);
}

AETHER_TEST(BlueprintDocument_ClipboardAndComments) {
    bp::NodeId begin, branch, get, print;
    BlueprintDocument doc(Door(begin, branch, get, print));
    const std::string clip = doc.CopyNodes("EventGraph", {branch, get});
    AETHER_CHECK(!clip.empty());

    // Pasted with fresh IDs at the point, keeping the link between the two
    // copies but not the ones to nodes left behind.
    const std::vector<bp::NodeId> pasted = doc.PasteNodes("EventGraph", clip, 1000.0f, 500.0f);
    AETHER_CHECK(pasted.size() == 2);
    const bp::Graph& g = doc.Get().graphs[0];
    AETHER_CHECK(g.nodes.size() == 6 && pasted[0] > print && pasted[1] > print);
    const bp::Node* new_branch = g.Find(pasted[0]);
    const bp::Node* new_get = g.Find(pasted[1]);
    AETHER_CHECK(new_branch->type == "Flow.Branch" && new_get->type == "Var.Get:IsOpen");
    AETHER_CHECK(new_branch->x == 1160.0f && new_branch->y == 500.0f && new_get->x == 1000.0f && new_get->y == 640.0f);
    AETHER_CHECK(HasLink(g, pasted[1], "value", pasted[0], "condition"));
    AETHER_CHECK(std::count_if(g.links.begin(), g.links.end(), [&](const bp::Link& l) {
                     return l.to.node == pasted[0] && l.to.pin == "exec";
                 }) == 0);
    AETHER_CHECK(doc.UndoLabel() == "Paste");

    // Not a node clipboard: nothing, and no undo step.
    AETHER_CHECK(doc.PasteNodes("EventGraph", "hello", 0, 0).empty() && doc.UndoLabel() == "Paste");
    AETHER_CHECK(doc.PasteNodes("EventGraph", "{\"x\": 1}", 0, 0).empty());

    const std::vector<bp::NodeId> dup = doc.DuplicateNodes("EventGraph", {print});
    AETHER_CHECK(dup.size() == 1 && doc.Get().graphs[0].Find(dup[0])->x == 550.0f);

    // Comment boxes: around a region, saved in the .abp.
    AETHER_CHECK(doc.AddComment("EventGraph", 0, 0, 400, 200, "Door logic"));
    AETHER_CHECK(!doc.AddComment("Nope", 0, 0, 1, 1));
    const bp::CommentBox& box = doc.Get().graphs[0].comments.at(0);
    AETHER_CHECK(box.text == "Door logic" && box.x == -20.0f && box.y == -50.0f && box.width == 440.0f && box.height == 270.0f);
    bp::Blueprint back;
    AETHER_CHECK(bp::BlueprintFromJson(bp::BlueprintToJson(doc.Get()), back));
    AETHER_CHECK(back.graphs[0].comments.size() == 1 && back.graphs[0].comments[0].text == "Door logic" &&
                 back.graphs[0].comments[0].width == 440.0f && back.graphs[0].comments[0].color == box.color);
    json bad = bp::BlueprintToJson(doc.Get());
    bad["graphs"][0]["comments"][0]["rect"] = json::array({1, 2});
    std::string error;
    AETHER_CHECK(!bp::BlueprintFromJson(bad, back, &error) && error.find("comment") != std::string::npos);
}

AETHER_TEST(BlueprintEditor_WidgetMovesAndResizesComments) {
    HeadlessImGui ui;
    GraphViewModel model;
    GraphNodeView inside, outside;
    inside.id = 1;
    inside.title = "Inside";
    inside.x = 50;
    inside.y = 60;
    outside.id = 2;
    outside.title = "Outside";
    outside.x = 600;
    outside.y = 60;
    model.nodes = {inside, outside};
    model.comments.push_back({"Group", 0, 0, 400, 300, 0x40FFFFFF});
    GraphViewState state;
    state.pan_x = state.pan_y = -50.0f;
    std::vector<GraphViewResult> all;
    auto frame = [&] {
        ui.Frame([&] { all.push_back(DrawGraphView("g", model, state)); });
    };
    ImGuiIO& io = ImGui::GetIO();
    auto move = [&](float x, float y) {
        io.AddMousePosEvent(x, y);
        frame();
    };
    auto press = [&](bool down) {
        io.AddMouseButtonEvent(0, down);
        frame();
    };
    frame();
    frame();

    // Drag the title bar: the box and the node inside move, the other doesn't.
    const float tx = state.CanvasToScreenX(200), ty = state.CanvasToScreenY(8);
    AETHER_CHECK(HitTestCommentTitle(model, state, tx, ty) == 0 && HitTestNode(model, state, tx, ty) == 0);
    all.clear();
    move(tx, ty);
    press(true);
    move(tx + 15, ty + 10);
    move(tx + 30, ty + 20);
    press(false);
    AETHER_CHECK(state.selected_comment == 0);
    auto changed = std::find_if(all.begin(), all.end(), [](const GraphViewResult& r) { return !r.comments_changed.empty(); });
    AETHER_CHECK(changed != all.end());
    if (changed != all.end()) {
        AETHER_CHECK(changed->comments_changed[0].x == 30.0f && changed->comments_changed[0].y == 20.0f);
        AETHER_CHECK(changed->moved.size() == 1 && changed->moved[0].first == 1 && changed->moved[0].second.first == 80.0f);
    }

    // The corner resizes.
    const float cx = state.CanvasToScreenX(396), cy = state.CanvasToScreenY(296);
    AETHER_CHECK(HitTestCommentCorner(model, state, cx, cy) == 0);
    all.clear();
    move(cx, cy);
    press(true);
    move(cx + 50, cy + 40);
    press(false);
    changed = std::find_if(all.begin(), all.end(), [](const GraphViewResult& r) { return !r.comments_changed.empty(); });
    AETHER_CHECK(changed != all.end() && changed->comments_changed[0].width == 450.0f &&
                 changed->comments_changed[0].height == 340.0f && changed->moved.empty());

    // Delete with the comment selected.
    all.clear();
    io.AddKeyEvent(ImGuiKey_Delete, true);
    frame();
    io.AddKeyEvent(ImGuiKey_Delete, false);
    frame();
    AETHER_CHECK(std::any_of(all.begin(), all.end(), [](const GraphViewResult& r) { return r.deleted_comment == 0; }));
}

AETHER_TEST(BlueprintEditor_PanelsPaletteResultsAndShortcuts) {
    HeadlessImGui ui;
    bp::NodeId begin, branch, get, print;
    BlueprintDocument doc(Door(begin, branch, get, print));
    BlueprintEditor editor(doc);
    auto frame = [&] { ui.Frame([&] { editor.Draw(); }); };
    frame();
    frame();
    AETHER_CHECK(editor.OpenGraphs() == std::vector<std::string>{"EventGraph"} && editor.ActiveGraph() == "EventGraph");

    // Every kind of Details page draws.
    const std::string fn = doc.AddFunction();
    const std::string d = doc.AddDispatcher();
    doc.AddComment("EventGraph", 0, 0, 100, 100);
    for (const BlueprintEditor::Item& item :
         {BlueprintEditor::Item{BlueprintEditor::ItemKind::Variable, "IsOpen", 0, -1},
          BlueprintEditor::Item{BlueprintEditor::ItemKind::Graph, fn, 0, -1},
          BlueprintEditor::Item{BlueprintEditor::ItemKind::Graph, "EventGraph", 0, -1},
          BlueprintEditor::Item{BlueprintEditor::ItemKind::Dispatcher, d, 0, -1},
          BlueprintEditor::Item{BlueprintEditor::ItemKind::Node, "EventGraph", print, -1},
          BlueprintEditor::Item{BlueprintEditor::ItemKind::Comment, "EventGraph", 0, 0}}) {
        editor.Select(item);
        frame();
        AETHER_CHECK(editor.Selected() == item);
    }

    // Tabs: open a function, rename it (the tab follows), close it.
    editor.OpenGraph(fn);
    frame();
    frame();
    AETHER_CHECK(editor.ActiveGraph() == fn && editor.OpenGraphs().size() == 2);
    AETHER_CHECK(editor.RenameGraph(fn, "Open"));
    frame();
    AETHER_CHECK(editor.ActiveGraph() == "Open" && editor.OpenGraphs()[1] == "Open");
    AETHER_CHECK(!editor.RenameGraph("Open", "bad name") && !editor.Status().empty());
    editor.CloseGraph("Open");
    editor.CloseGraph("EventGraph"); // stays
    frame();
    AETHER_CHECK(editor.OpenGraphs() == std::vector<std::string>{"EventGraph"} && editor.ActiveGraph() == "EventGraph");

    // Compiler results: an error row focuses its node.
    bp::NodeId broken = 0;
    doc.Edit("Break", [&](bp::Blueprint& b) { broken = bp::GraphBuilder(b.graphs[0]).Add("Flow.Teleport", json::object(), 900, 400); });
    editor.CompileNow();
    AETHER_CHECK(doc.CompileStatus() == BlueprintDocument::Status::Errors && editor.Status().find("failed") != std::string::npos);
    frame();
    const bp::ValidationResult& diags = doc.LastCompile().diagnostics;
    auto row = std::find_if(diags.diagnostics.begin(), diags.diagnostics.end(), [&](const bp::Diagnostic& x) { return x.node == broken; });
    AETHER_CHECK(row != diags.diagnostics.end());
    editor.FocusNode(row->graph, row->node);
    frame();
    AETHER_CHECK(editor.Selected().kind == BlueprintEditor::ItemKind::Node && editor.Selected().node == broken);
    AETHER_CHECK(editor.ViewState("EventGraph").selection == std::set<u32>{broken});
    doc.Undo();
    editor.Select({});
    frame();

    // The palette from a dragged pin: filtered, and the placed node links.
    const GraphPinRef from{get, "value", true};
    editor.OpenPalette(700, 300, &from);
    frame();
    AETHER_CHECK(editor.PaletteOpen());
    editor.SetPaletteQuery("not");
    const std::vector<bp::PaletteEntry> results = editor.PaletteResults();
    AETHER_CHECK(std::any_of(results.begin(), results.end(), [](const bp::PaletteEntry& e) { return e.id == "Math.Not"; }));
    editor.SetPaletteQuery("sqrt"); // takes a float, which a bool can't feed
    const std::vector<bp::PaletteEntry> none = editor.PaletteResults();
    AETHER_CHECK(std::none_of(none.begin(), none.end(), [](const bp::PaletteEntry& e) { return e.id == "Math.Sqrt"; }));
    const bp::NodeId not_node = editor.PlaceFromPalette("Math.Not");
    frame();
    AETHER_CHECK(not_node != 0 && !editor.PaletteOpen());
    AETHER_CHECK(HasLink(doc.Get().graphs[0], get, "value", not_node, "a"));
    AETHER_CHECK(doc.Get().graphs[0].Find(not_node)->x == 700.0f);
    AETHER_CHECK(editor.Selected().kind == BlueprintEditor::ItemKind::Node && editor.Selected().node == not_node);

    editor.OpenPalette(0, 0); // with no pin, the float node is listed
    editor.SetPaletteQuery("sqrt");
    const std::vector<bp::PaletteEntry> all_sqrt = editor.PaletteResults();
    AETHER_CHECK(std::any_of(all_sqrt.begin(), all_sqrt.end(), [](const bp::PaletteEntry& e) { return e.id == "Math.Sqrt"; }));
    frame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true); // Esc closes it
    frame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    frame();
    AETHER_CHECK(!editor.PaletteOpen());

    // Graph keys while hovered: Ctrl+D duplicates the selection, C comments
    // it; editor keys: Ctrl+Z undoes.
    GraphViewState& view = editor.ViewState("EventGraph");
    view.selection = {print};
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(view.origin_x + view.width - 20, view.origin_y + view.height - 20);
    frame();
    const usize before = doc.Get().graphs[0].nodes.size();
    auto key = [&](ImGuiKey k, bool ctrl) {
        if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(k, true);
        frame();
        io.AddKeyEvent(k, false);
        if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, false);
        frame();
    };
    key(ImGuiKey_D, true);
    AETHER_CHECK(doc.Get().graphs[0].nodes.size() == before + 1);
    const usize comments = doc.Get().graphs[0].comments.size();
    key(ImGuiKey_C, false);
    AETHER_CHECK(doc.Get().graphs[0].comments.size() == comments + 1);
    key(ImGuiKey_Z, true);
    AETHER_CHECK(doc.Get().graphs[0].comments.size() == comments);
    key(ImGuiKey_Z, true);
    AETHER_CHECK(doc.Get().graphs[0].nodes.size() == before);
    // Tab opens the palette at the mouse.
    key(ImGuiKey_Tab, false);
    AETHER_CHECK(editor.PaletteOpen());
    key(ImGuiKey_F7, false);
    AETHER_CHECK(doc.CompileStatus() != BlueprintDocument::Status::NotCompiled);
}
