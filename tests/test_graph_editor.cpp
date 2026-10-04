#include "graph/blueprint_graph.h"
#include "test_framework.h"

#include <imgui.h>

#include <algorithm>
#include <functional>

using namespace aether;
using namespace aether::editor;
using nlohmann::json;

namespace {

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGui::GetIO().ConfigMacOSXBehaviors = false; // tests press Ctrl on every platform
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1280, 800);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(context_); }
    void Frame(const std::function<void()>& body) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1200, 760));
        ImGui::Begin("Blueprint", nullptr, ImGuiWindowFlags_NoMove);
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

const GraphPinView* Pin(const GraphNodeView& n, const std::string& name, bool output) {
    for (const GraphPinView& p : output ? n.outputs : n.inputs) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

} // namespace

AETHER_TEST(GraphEditor_BlueprintViewColorsPinsAndDebugState) {
    bp::NodeId begin, branch, get, print;
    bp::Blueprint door = Door(begin, branch, get, print);
    bp::Graph& g = door.graphs[0];
    const bp::NodeId broken = bp::GraphBuilder(g).Add("Flow.Teleport");
    bp::ValidationResult diagnostics = bp::ValidateBlueprint(door);

    BlueprintViewOptions options;
    options.diagnostics = &diagnostics;
    options.highlighted = print;
    options.breakpoints = {branch};
    options.fired = {begin};
    const GraphViewModel view = BuildBlueprintView(door, g, options);
    AETHER_CHECK(view.nodes.size() == 5 && view.links.size() == 3);

    const GraphNodeView& ev = *view.Find(begin);
    const GraphNodeView& br = *view.Find(branch);
    const GraphNodeView& gv = *view.Find(get);
    AETHER_CHECK(ev.title == "Event BeginPlay" && ev.header_color == IM_COL32(150, 30, 30, 255));
    AETHER_CHECK(br.header_color == IM_COL32(85, 85, 90, 255) && br.breakpoint);
    AETHER_CHECK(gv.pure && gv.header_color == IM_COL32(40, 105, 55, 255));
    // Pins: exec pins have no label; the bool pin is maroon; connected ones filled.
    const GraphPinView* exec_in = Pin(br, "exec", false);
    const GraphPinView* cond = Pin(br, "condition", false);
    AETHER_CHECK(exec_in != nullptr && exec_in->exec && exec_in->label.empty() && exec_in->connected);
    AETHER_CHECK(cond != nullptr && cond->color == IM_COL32(0x8C, 0x1A, 0x1A, 255) && cond->label == "condition");
    AETHER_CHECK(!Pin(br, "true", true)->connected && Pin(br, "false", true)->connected);
    // Wires: colored by type; exec wires out of fired nodes glow.
    for (const GraphLinkView& l : view.links) {
        if (l.from_node == get) AETHER_CHECK(l.color == IM_COL32(0x8C, 0x1A, 0x1A, 255) && l.glow == 0.0f);
        if (l.from_node == begin) AETHER_CHECK(l.color == IM_COL32(255, 255, 255, 255) && l.glow == 1.0f);
    }
    AETHER_CHECK(view.Find(print)->highlighted);
    // A node that doesn't resolve shows its error.
    const GraphNodeView& bad = *view.Find(broken);
    AETHER_CHECK(bad.title == "Flow.Teleport" && bad.error.find("Unknown node type") != std::string::npos);
    AETHER_CHECK(bad.error.find("BP007") != std::string::npos); // the diagnostic too
}

AETHER_TEST(GraphEditor_EditsCheckTypesAndReplaceLinks) {
    bp::NodeId begin, branch, get, print;
    bp::Blueprint door = Door(begin, branch, get, print);
    bp::Graph& g = door.graphs[0];
    const bp::NodeId make = AddNode(g, "Vec3.Make", 50, 300), get2 = AddNode(g, "Var.Get:IsOpen", 50, 400);
    AETHER_CHECK(g.Find(make)->x == 50.0f);

    std::string error;
    // Wrong type, wrong direction, same node: refused with a reason.
    AETHER_CHECK(!ConnectPins(door, g, {make, "result", true}, {branch, "condition", false}, &error));
    AETHER_CHECK(error.find("Vec3") != std::string::npos);
    AETHER_CHECK(!ConnectPins(door, g, {branch, "condition", false}, {get, "value", true}, &error));
    AETHER_CHECK(!ConnectPins(door, g, {branch, "true", true}, {branch, "exec", false}, &error));
    // A second link into a data input replaces the first.
    AETHER_CHECK(ConnectPins(door, g, {get2, "value", true}, {branch, "condition", false}));
    AETHER_CHECK(std::count_if(g.links.begin(), g.links.end(), [&](const bp::Link& l) {
                     return l.to.node == branch && l.to.pin == "condition";
                 }) == 1);
    // An exec output keeps one link too; bool -> Print text converts.
    AETHER_CHECK(ConnectPins(door, g, {branch, "false", true}, {begin, "then", false}, &error) == false); // "then" is an output
    const bp::NodeId print2 = AddNode(g, "Debug.Print", 700, 0);
    AETHER_CHECK(ConnectPins(door, g, {branch, "false", true}, {print2, "exec", false}));
    AETHER_CHECK(std::count_if(g.links.begin(), g.links.end(), [&](const bp::Link& l) {
                     return l.from.node == branch && l.from.pin == "false";
                 }) == 1);
    AETHER_CHECK(ConnectPins(door, g, {get, "value", true}, {print2, "text", false}));

    // The widget's edits, applied.
    GraphViewResult edits;
    edits.moved = {{print, {600.0f, 50.0f}}};
    edits.disconnect = true;
    edits.disconnected = {get, "value", print2, "text", 0, 0.0f};
    edits.connect = true;
    edits.connect_from = {make, "result", true};
    edits.connect_to = {print2, "text", false};
    edits.deleted = {get2};
    const std::vector<std::string> errors = ApplyGraphEdits(door, g, edits);
    AETHER_CHECK(errors.empty());
    AETHER_CHECK(g.Find(print)->x == 600.0f && g.Find(get2) == nullptr);
    AETHER_CHECK(std::none_of(g.links.begin(), g.links.end(), [&](const bp::Link& l) {
        return l.from.node == get2 || l.to.node == get2;
    }));
    AETHER_CHECK(std::any_of(g.links.begin(), g.links.end(), [&](const bp::Link& l) { return l.from.node == make; }));
    edits = {};
    edits.connect = true;
    edits.connect_from = {make, "result", true};
    edits.connect_to = {branch, "condition", false};
    AETHER_CHECK(ApplyGraphEdits(door, g, edits).size() == 1);
    AETHER_CHECK(bp::ValidateBlueprint(door).Ok());
}

AETHER_TEST(GraphEditor_PaletteSearchIsFuzzyAndContextSensitive) {
    bp::NodeId begin, branch, get, print;
    bp::Blueprint door = Door(begin, branch, get, print);
    bp::Graph& g = door.graphs[0];

    std::vector<bp::PaletteEntry> hits = SearchPalette(door, g, "branch");
    AETHER_CHECK(!hits.empty() && hits[0].id == "Flow.Branch");
    hits = SearchPalette(door, g, "brnch");
    AETHER_CHECK(!hits.empty() && hits[0].id == "Flow.Branch");
    hits = SearchPalette(door, g, "set isopen");
    AETHER_CHECK(!hits.empty() && hits[0].id == "Var.Set:IsOpen");
    AETHER_CHECK(SearchPalette(door, g, "zzqqxx").empty());
    AETHER_CHECK(SearchPalette(door, g, "", nullptr, 5).size() == 5);

    // Dragging off a bool output: only nodes with a pin that takes a bool.
    const GraphPinRef from{get, "value", true};
    hits = SearchPalette(door, g, "", &from, 1000);
    auto has = [&](const std::string& id) {
        return std::any_of(hits.begin(), hits.end(), [&](const bp::PaletteEntry& e) { return e.id == id; });
    };
    AETHER_CHECK(has("Flow.Branch") && has("Math.Not") && has("Debug.Print")); // Print converts to text
    AETHER_CHECK(!has("Vec3.Make") && !has("Event.BeginPlay"));
    // Placing one links it up.
    const bp::NodeId not_node = AddNode(g, "Math.Not", 300, 200);
    AETHER_CHECK(ConnectToNewNode(door, g, from, not_node));
    AETHER_CHECK(std::any_of(g.links.begin(), g.links.end(), [&](const bp::Link& l) {
        return l.from.node == get && l.to.node == not_node && l.to.pin == "a";
    }));
}

AETHER_TEST(GraphEditor_WidgetDragsLinksPalettesAndZooms) {
    HeadlessImGui ui;
    bp::NodeId begin, branch, get, print;
    bp::Blueprint door = Door(begin, branch, get, print);
    bp::Graph& g = door.graphs[0];
    GraphViewState state;
    state.pan_x = state.pan_y = -60.0f; // leave empty canvas above and left of the nodes
    GraphViewResult last;
    std::vector<GraphViewResult> all;
    auto frame = [&] {
        const GraphViewModel model = BuildBlueprintView(door, g);
        ui.Frame([&] {
            last = DrawGraphView("graph", model, state);
            all.push_back(last);
        });
    };
    ImGuiIO& io = ImGui::GetIO();
    auto move = [&](float x, float y) {
        io.AddMousePosEvent(x, y);
        frame();
    };
    auto press = [&](int button, bool down) {
        io.AddMouseButtonEvent(button, down);
        frame();
    };
    auto collect = [&](const std::function<bool(const GraphViewResult&)>& pred) {
        return std::any_of(all.begin(), all.end(), pred);
    };
    frame();
    frame();
    const GraphViewModel model = BuildBlueprintView(door, g);
    AETHER_CHECK(state.width > 100.0f && state.zoom == 1.0f);

    // Layout: pins sit on the node's edges.
    const NodeLayout layout = LayoutNode(*model.Find(branch), state.font_size);
    AETHER_CHECK(layout.width >= 80.0f && layout.input_y.size() == 2 && layout.output_y.size() == 2);
    float px = 0, py = 0;
    AETHER_CHECK(PinScreenPosition(model, state, {branch, "condition", false}, px, py));
    AETHER_CHECK(px == state.CanvasToScreenX(260.0f));

    // Drag the Print node by 40, 20.
    const float nx = state.CanvasToScreenX(520.0f + 30.0f), ny = state.CanvasToScreenY(0.0f + 5.0f);
    AETHER_CHECK(HitTestNode(model, state, nx, ny) == print);
    all.clear();
    move(nx, ny);
    press(0, true);
    move(nx + 20, ny + 10);
    move(nx + 40, ny + 20);
    press(0, false);
    AETHER_CHECK(collect([&](const GraphViewResult& r) {
        return r.moved.size() == 1 && r.moved[0].first == print && r.moved[0].second.first == 560.0f &&
               r.moved[0].second.second == 20.0f;
    }));
    AETHER_CHECK(state.selection == std::set<u32>{print});
    for (const GraphViewResult& r : all) ApplyGraphEdits(door, g, r);
    AETHER_CHECK(g.Find(print)->x == 560.0f);

    // Drag a wire from Get IsOpen's value to Print's text: a link request.
    const GraphViewModel m2 = BuildBlueprintView(door, g);
    float fx, fy, tx, ty;
    PinScreenPosition(m2, state, {get, "value", true}, fx, fy);
    PinScreenPosition(m2, state, {print, "text", false}, tx, ty);
    all.clear();
    move(fx, fy);
    press(0, true);
    move((fx + tx) / 2, (fy + ty) / 2);
    move(tx, ty);
    press(0, false);
    AETHER_CHECK(collect([&](const GraphViewResult& r) {
        return r.connect && r.connect_from == GraphPinRef{get, "value", true} && r.connect_to == GraphPinRef{print, "text", false};
    }));

    // A wire dropped on empty canvas asks for the palette, from that pin.
    all.clear();
    move(fx, fy);
    press(0, true);
    move(fx + 200, fy + 300);
    press(0, false);
    AETHER_CHECK(collect([&](const GraphViewResult& r) {
        return r.open_palette && r.palette_from_pin && r.palette_from == GraphPinRef{get, "value", true};
    }));

    // Right-click on empty canvas: the palette at that canvas point.
    all.clear();
    const float ex = state.CanvasToScreenX(300.0f), ey = state.CanvasToScreenY(400.0f);
    move(ex, ey);
    press(1, true);
    press(1, false);
    AETHER_CHECK(collect([&](const GraphViewResult& r) {
        return r.open_palette && !r.palette_from_pin && std::fabs(r.palette_x - 300.0f) < 0.5f &&
               std::fabs(r.palette_y - 400.0f) < 0.5f;
    }));

    // Alt+click a wire: break it.
    const GraphViewModel m3 = BuildBlueprintView(door, g);
    float ax, ay, bx, by;
    PinScreenPosition(m3, state, {begin, "then", true}, ax, ay);
    PinScreenPosition(m3, state, {branch, "exec", false}, bx, by);
    const float wx = (ax + bx) / 2, wy = (ay + by) / 2;
    AETHER_CHECK(HitTestLink(m3, state, wx, wy) >= 0);
    all.clear();
    io.AddKeyEvent(ImGuiMod_Alt, true);
    move(wx, wy);
    press(0, true);
    press(0, false);
    io.AddKeyEvent(ImGuiMod_Alt, false);
    frame();
    AETHER_CHECK(collect([&](const GraphViewResult& r) { return r.disconnect && r.disconnected.from_node == begin; }));

    // Box-select the three left nodes, then Delete.
    all.clear();
    move(state.CanvasToScreenX(-20), state.CanvasToScreenY(-20));
    press(0, true);
    move(state.CanvasToScreenX(200), state.CanvasToScreenY(100));
    move(state.CanvasToScreenX(380), state.CanvasToScreenY(260));
    press(0, false);
    AETHER_CHECK(state.selection == (std::set<u32>{begin, branch, get}));
    io.AddKeyEvent(ImGuiKey_Delete, true);
    frame();
    io.AddKeyEvent(ImGuiKey_Delete, false);
    frame();
    AETHER_CHECK(collect([&](const GraphViewResult& r) { return r.deleted.size() == 3; }));

    // Wheel zoom keeps the canvas point under the cursor.
    const float cx = state.CanvasToScreenX(100.0f), cy = state.CanvasToScreenY(50.0f);
    move(cx, cy);
    io.AddMouseWheelEvent(0.0f, 2.0f);
    frame();
    AETHER_CHECK(state.zoom > 1.1f);
    AETHER_CHECK(std::fabs(state.ScreenToCanvasX(cx) - 100.0f) < 0.5f && std::fabs(state.ScreenToCanvasY(cy) - 50.0f) < 0.5f);
    // Home fits everything.
    state.request_fit = true;
    frame();
    const GraphViewModel m4 = BuildBlueprintView(door, g);
    for (const GraphNodeView& n : m4.nodes) {
        AETHER_CHECK(state.CanvasToScreenX(n.x) >= state.origin_x && state.CanvasToScreenY(n.y) >= state.origin_y);
        const NodeLayout l = LayoutNode(n, state.font_size);
        AETHER_CHECK(state.CanvasToScreenX(n.x + l.width) <= state.origin_x + state.width);
    }
}
