#include "ai/bt_editor.h"
#include "ai/nav_panel.h"
#include "test_framework.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>

// Phase 20 step 6: the AI editors - navigation and AI debug drawing, the
// Behavior Tree document (nodes, decorators, services, keys that every
// reference follows, checks, undo, files), the tree's layout, view and
// graph edits, the editor and the Navigation panel drawn headless.

using namespace aether;
using namespace aether::editor;
using namespace aether::ai;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

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
        ImGui::Begin("AI", nullptr, ImGuiWindowFlags_NoMove);
        body();
        ImGui::End();
        ImGui::Render();
    }

private:
    ImGuiContext* context_ = nullptr;
};

bool Floor(const ModelRenderer& m, std::vector<Vec3>& v, std::vector<u32>& i) {
    if (std::strcmp(m.asset_path, "floor") != 0) return false;
    nav::NavGeometry g;
    g.AddPlane(Vec3(0, 0, 0), 10, 10);
    v = g.vertices;
    i = g.indices;
    return true;
}

// Guard: Selector [ Sequence(if Enemy is set) [MoveTo Enemy, Log], Wait ].
BehaviorTreeDocument GuardDoc() {
    BehaviorTreeDocument doc;
    CHECK(doc.AddKey("Enemy", BlackboardType::Entity));
    CHECK(doc.AddKey("Health", BlackboardType::Float));
    const auto chase = doc.AddNode({}, BtNodeType::Sequence, 0);
    CHECK(chase && *chase == BtPath{0});
    CHECK(doc.AddNode({0}, BtNodeType::MoveTo));
    CHECK(doc.SetNodeField({0, 0}, "key", "Enemy"));
    CHECK(doc.AddNode({0}, BtNodeType::Log));
    CHECK(doc.AddDecorator({0}, BtDecoratorType::BlackboardCondition));
    return doc;
}

} // namespace

AETHER_TEST(AIEditor_NavDebugDraw) {
    nav::NavGeometry g;
    g.AddPlane(Vec3(0, 0, 0), 10, 10);
    g.AddPlane(Vec3(0, 0, 0), 2, 2, 5); // not over the floor's area... both are baked; the higher area wins where they overlap
    nav::NavLink link;
    link.start = Vec3(-5, 0, 0), link.end = Vec3(5, 0, 0), link.bidirectional = false;
    g.links.push_back(link);
    nav::NavMeshSettings s;
    s.tile_size = 24;
    nav::DynamicNavMesh dyn;
    CHECK(dyn.Build(g, s));
    nav::NavDebugDraw draw;
    nav::NavDebugOptions o;
    nav::DrawNavMesh(dyn.Mesh(), o, draw);
    CHECK(!draw.triangles.empty());
    const usize edges = draw.lines.size();
    CHECK(edges > 0);
    // Lifted off the mesh.
    CHECK(std::all_of(draw.triangles.begin(), draw.triangles.end(), [](const nav::NavDebugTriangle& t) { return t.a.y > 0.0f; }));
    // Areas get colours of their own; walkable is blue.
    CHECK(nav::NavAreaColor(nav::kAreaWalkable) != nav::NavAreaColor(5));
    CHECK(nav::NavAreaColor(5) == nav::NavAreaColor(5));
    CHECK((nav::NavAreaColor(nav::kAreaWalkable, 0xFF) >> 24) == 0xFF);
    CHECK(std::any_of(draw.lines.begin(), draw.lines.end(), [](const nav::NavDebugLine& l) { return l.color == nav::nav_colors::kOneWayLink; }));
    // Toggles.
    nav::NavDebugDraw none;
    nav::NavDebugOptions off;
    off.polygons = off.edges = off.links = off.volumes = false;
    nav::DrawNavMesh(dyn.Mesh(), off, none);
    CHECK(none.triangles.empty() && none.lines.empty());
    off.tile_bounds = true;
    nav::DrawNavMesh(dyn.Mesh(), off, none);
    CHECK(none.lines.size() == dyn.Mesh().Data().tiles.size() * 4);
    // Volumes: a box (4 + 4 + 4 lines), a cylinder, a prism.
    nav::NavDebugDraw v;
    nav::DrawNavVolume(nav::NavVolume::Box(Vec3(0, 1, 0), Vec3(1, 1, 1)), v);
    CHECK(v.lines.size() == 12);
    CHECK(v.lines[0].color == nav::nav_colors::kObstacle);
    v.Clear();
    nav::DrawNavVolume(nav::NavVolume::Cylinder(Vec3(0, 1, 0), 1, 2, 3), v);
    CHECK(v.lines.size() == 16 * 2 + 4);
    CHECK(v.lines[0].color == nav::NavAreaColor(3, 0xFF));
    v.Clear();
    nav::DrawNavVolume(nav::NavVolume::Prism({Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(0, 0, 1)}, 0, 1), v);
    CHECK(v.lines.size() == 9);
    // Dynamic volumes are drawn with the mesh.
    dyn.AddVolume(nav::NavVolume::Box(Vec3(5, 1, 5), Vec3(1, 1, 1)));
    nav::NavDebugDraw all;
    nav::DrawDynamicNavMesh(dyn, o, all);
    CHECK(std::count_if(all.lines.begin(), all.lines.end(), [](const nav::NavDebugLine& l) { return l.color == nav::nav_colors::kObstacle; }) == 12);
    // A link's arc rises in the middle; a path is a line per leg plus posts.
    nav::NavDebugDraw arc;
    nav::DrawNavLink(link, arc);
    const f32 top = std::max_element(arc.lines.begin(), arc.lines.end(), [](const nav::NavDebugLine& a, const nav::NavDebugLine& b) { return a.b.y < b.b.y; })->b.y;
    CHECK(std::fabs(top - 2.5f) < 0.01f);
    nav::NavPath path;
    path.points = {Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(1, 0, 1)};
    nav::NavDebugDraw p;
    nav::DrawNavPath(path, nav::nav_colors::kPath, p);
    CHECK(p.lines.size() == 2 + 3);
}

AETHER_TEST(AIEditor_AIDebugDraw) {
    World world;
    ModelRenderer floor;
    SetModelPath(floor, "floor");
    world.CreateEntity(Transform{}, floor);
    nav::NavWorld nav(world, Floor);
    CHECK(nav.Bake({}));
    nav::NavCrowd crowd(world, nav);
    PerceptionWorld perception(world);
    NavAgent agent;
    const Entity walker = world.CreateEntity(Transform{Vec3(-5, 0, 0), Quaternion::Identity()}, agent);
    world.GetComponent<NavAgent>(walker)->MoveTo(Vec3(5, 0, 5));
    AIPerception eyes;
    const Entity guard = world.CreateEntity(Transform{Vec3(0, 0, -5), Quaternion::Identity()}, eyes);
    world.CreateEntity(Transform{Vec3(0, 0, 0), Quaternion::Identity()}, AIStimuliSource{});
    crowd.Update(0.1f);
    perception.Update(0.1f);
    nav::NavDebugDraw d;
    DrawAIDebug(world, &crowd, {}, d);
    const auto count = [&](u32 color) { return std::count_if(d.lines.begin(), d.lines.end(), [&](const nav::NavDebugLine& l) { return l.color == color; }); };
    CHECK(count(ai_colors::kCorners) >= 1);
    CHECK(count(ai_colors::kGoal) == 3);
    CHECK(count(ai_colors::kSight) == 16 + 2);
    CHECK(count(ai_colors::kSeen) == 1);
    CHECK(count(ai_colors::kHearing) == 0);
    world.GetComponent<AIPerception>(guard)->fov_degrees = 360.0f;
    AIDebugOptions o;
    o.hearing = true;
    o.agent_paths = false;
    d.Clear();
    DrawAIDebug(world, &crowd, o, d);
    CHECK(count(ai_colors::kSight) == 32 && count(ai_colors::kHearing) == 32 && count(ai_colors::kCorners) == 0);
}

AETHER_TEST(AIEditor_Document) {
    BehaviorTreeDocument doc = GuardDoc();
    std::string error;
    CHECK(doc.Get().root.type == BtNodeType::Selector);
    CHECK(doc.Get().root.children.size() == 2); // the chase sequence, then the starting Wait
    CHECK(doc.Node({1})->type == BtNodeType::Wait);
    CHECK(doc.Node({0, 1})->event == "Hello");
    CHECK(doc.Node({9}) == nullptr);
    // Children only under composites.
    CHECK(!doc.AddNode({1}, BtNodeType::Log, std::nullopt, &error));
    CHECK(error.find("Wait") != std::string::npos);
    // Moves: reorder, reparent (with the parent's index shifting), and refusals.
    auto moved = doc.MoveNode({0, 1}, {0}, 0);
    CHECK(moved && *moved == BtPath({0, 0}) && doc.Node({0, 0})->type == BtNodeType::Log);
    moved = doc.MoveNode({0, 1}, {}, 2); // the MoveTo, out to the root's end
    CHECK(moved && *moved == BtPath({2}) && doc.Node({2})->type == BtNodeType::MoveTo);
    CHECK(doc.AddNode({}, BtNodeType::Sequence, 0)); // a new first child: the chase moves to [1]
    moved = doc.MoveNode({2}, {0}, 0); // the old chase sequence (now [1])? no: [2] is the Wait
    CHECK(moved && *moved == BtPath({0, 0}) && doc.Node({0, 0})->type == BtNodeType::Wait);
    // Moving a node into a later sibling (the old chase, now [1]): that sibling's index drops by one.
    moved = doc.MoveNode({0}, {1}, 0);
    CHECK(moved && *moved == BtPath({0, 0}));
    CHECK(!doc.MoveNode({0}, {1}, 0, &error)); // [1] is the MoveTo now: a task
    CHECK(!doc.MoveNode({}, {0}, 0, &error));
    CHECK(!doc.MoveNode({0}, {0, 0}, 0, &error) && error.find("itself") != std::string::npos);
    CHECK(doc.Undo() && doc.Undo() && doc.Undo() && doc.Undo() && doc.Undo()); // back to the guard
    CHECK(doc.Node({0, 1})->type == BtNodeType::Log && doc.Node({1})->type == BtNodeType::Wait);
    // Duplicate, remove, change type.
    const auto dup = doc.DuplicateNode({0, 0});
    CHECK(dup && *dup == BtPath({0, 1}) && doc.Node({0, 1})->type == BtNodeType::MoveTo);
    CHECK(doc.RemoveNode({0, 1}));
    CHECK(!doc.RemoveNode({}, &error));
    CHECK(!doc.DuplicateNode({}, &error));
    CHECK(!doc.SetNodeType({0}, BtNodeType::Wait, &error)); // it has children
    CHECK(doc.SetNodeType({0}, BtNodeType::Parallel) && doc.Node({0})->type == BtNodeType::Parallel);
    CHECK(doc.SetNodeType({1}, BtNodeType::Fail) && doc.SetNodeType({1}, BtNodeType::Wait));
    // Fields through the saved form.
    nlohmann::json j = doc.NodeJson({1});
    CHECK(j["type"] == "Wait" && j.contains("seconds") && !j.contains("children"));
    CHECK(doc.SetNodeField({1}, "seconds", 2.5, &error) && doc.Node({1})->seconds == 2.5f);
    CHECK(doc.SetNodeField({1}, "seconds", 3.0, &error, "drag") && doc.SetNodeField({1}, "seconds", 3.5, &error, "drag"));
    CHECK(doc.Undo() && doc.Node({1})->seconds == 2.5f); // one step for the drag
    CHECK(!doc.SetNodeField({1}, "seconds", "long", &error) && !error.empty());
    CHECK(!doc.SetNodeField({1}, "children", nlohmann::json::array(), &error));
    CHECK(doc.SetNodeField({1}, "name", "Idle") && doc.Node({1})->Label() == "Idle");
    // Decorators.
    CHECK(doc.Node({0})->decorators.size() == 1 && doc.Node({0})->decorators[0].key == "Enemy"); // the first key by default
    CHECK(doc.SetDecoratorField({0}, 0, "abort", "Both") && doc.Node({0})->decorators[0].abort == BtAbort::Both);
    CHECK(!doc.SetDecoratorField({0}, 0, "abort", "Sideways", &error) && error.find("Sideways") != std::string::npos);
    CHECK(doc.AddDecorator({0}, BtDecoratorType::Cooldown) && doc.DecoratorJson({0}, 1)["seconds"] == 1.0);
    CHECK(doc.MoveDecorator({0}, 1, 0) && doc.Node({0})->decorators[0].type == BtDecoratorType::Cooldown);
    CHECK(doc.RemoveDecorator({0}, 0) && doc.Node({0})->decorators.size() == 1 && !doc.RemoveDecorator({0}, 5));
    // A comparison with a value, typed by its key.
    CHECK(doc.AddDecorator({1}, BtDecoratorType::BlackboardCondition));
    CHECK(doc.SetDecoratorField({1}, 0, "key", "Health") && doc.SetDecoratorField({1}, 0, "op", "Less"));
    CHECK(doc.SetDecoratorField({1}, 0, "value", 30.0) && std::get<f32>(doc.Node({1})->decorators[0].value) == 30.0f);
    CHECK(!doc.SetDecoratorField({1}, 0, "value", "low", &error));
    // Services.
    CHECK(doc.AddService({0}, BtServiceType::DistanceTo));
    CHECK(doc.SetServiceField({0}, 0, "key", "Enemy") && doc.SetServiceField({0}, 0, "out_key", "Health"));
    CHECK(doc.ServiceJson({0}, 0)["interval"] == 0.5);
    CHECK(doc.AddService({1}, BtServiceType::Blueprint) && doc.Node({1})->services[0].event == "Update");
    CHECK(doc.RemoveService({1}, 0) && doc.Node({1})->services.empty());
    CHECK(doc.Diagnostics().empty());

    // Keys: unique, renames that every reference follows, type changes, initials.
    CHECK(!doc.AddKey("Enemy", BlackboardType::Bool, &error) && !doc.AddKey("", BlackboardType::Bool, &error));
    CHECK(doc.UniqueKeyName("Enemy") == "Enemy1" && doc.UniqueKeyName("Fresh") == "Fresh");
    CHECK(doc.KeyUsers("Enemy").size() == 2); // the chase's condition and service, and the MoveTo
    CHECK(doc.RenameKey("Enemy", "Target"));
    CHECK(doc.Node({0})->decorators[0].key == "Target" && doc.Node({0, 0})->key == "Target" && doc.Node({0})->services[0].key == "Target");
    CHECK(!doc.RenameKey("Target", "Health", &error) && !doc.RenameKey("Nope", "X", &error));
    CHECK(doc.SetKeyType("Health", BlackboardType::Int)); // the Float comparison value is cleared
    CHECK(std::holds_alternative<std::monostate>(doc.Node({1})->decorators[0].value));
    CHECK(doc.ErrorCount() >= 1); // now nothing to compare with (BT004), and DistanceTo writes a Float (BT004)
    CHECK(doc.Undo() && doc.ErrorCount() == 0);
    CHECK(doc.SetKeyInitial("Health", 100.0) && std::get<f32>(doc.Get().blackboard.Find("Health")->initial) == 100.0f);
    CHECK(!doc.SetKeyInitial("Health", "full", &error));
    CHECK(doc.SetKeyDescription("Health", "Hit points") && doc.Get().blackboard.Find("Health")->description == "Hit points");
    CHECK(doc.RemoveKey("Target"));
    const auto& ds = doc.Diagnostics();
    CHECK(std::any_of(ds.begin(), ds.end(), [](const BtProblem& p) { return p.code == "BT003"; }));
    CHECK(doc.Undo() && doc.Diagnostics().empty());
    CHECK(BtProblemPath(doc.Get(), {0, 0}) == "root/Parallel[0]/MoveTo[0]"); // it became a Parallel above

    // Files.
    const auto dir = std::filesystem::temp_directory_path() / "aether_test_bt_editor";
    std::filesystem::create_directories(dir);
    CHECK(!doc.Save(&error)); // no file yet
    CHECK(doc.SaveAs(dir / "Guard.abt") && !doc.Dirty() && doc.Name() == "Guard");
    BehaviorTreeDocument loaded;
    CHECK(loaded.Load(dir / "Guard.abt", &error));
    CHECK(loaded.Text() == doc.Text());
    std::filesystem::remove_all(dir);
}

AETHER_TEST(AIEditor_LayoutAndView) {
    BehaviorTreeDocument doc = GuardDoc();
    const auto layout = LayoutBehaviorTree(doc.Get());
    CHECK(layout.size() == 5);
    // Depth-first ids; leaves in slots left to right; parents centred over their children.
    CHECK(layout[0].id == 1 && layout[1].path == BtPath{0} && layout[2].path == BtPath({0, 0}) && layout[4].path == BtPath{1});
    CHECK(layout[2].x == 0.0f && layout[3].x == kBtSlotWidth && layout[4].x == 2 * kBtSlotWidth);
    CHECK(layout[1].x == kBtSlotWidth * 0.5f && layout[0].x == (layout[1].x + layout[4].x) * 0.5f);
    CHECK(layout[2].y == 2 * kBtRowHeight && layout[2].parent == 1);
    // Titles and notes.
    CHECK(BtNodeTitle(*doc.Node({0, 0})) == "MoveTo Enemy");
    CHECK(BtNodeTitle(*doc.Node({1})) == "Wait 1s");
    CHECK(BtNodeNotes(*doc.Node({0})) == "if Enemy is set");
    doc.SetDecoratorField({0}, 0, "abort", "LowerPriority");
    doc.AddService({0}, BtServiceType::Luau);
    CHECK(BtNodeNotes(*doc.Node({0})) == "if Enemy is set, aborts lower\nservice Update()  every 0.5s");
    // The view: pins and wires parent -> child.
    GraphViewModel m = BuildBehaviorTreeView(doc.Get(), LayoutBehaviorTree(doc.Get()), doc.Diagnostics());
    CHECK(m.nodes.size() == 5 && m.links.size() == 4);
    CHECK(m.nodes[0].inputs.empty() && m.nodes[0].outputs.size() == 1);
    CHECK(m.nodes[2].inputs.size() == 1 && m.nodes[2].outputs.empty());
    CHECK(m.links[0].from_node == 1 && m.links[0].to_node == 2);
    // Errors show on their nodes.
    doc.SetNodeField({0, 0}, "key", "Health"); // not a place: BT009
    m = BuildBehaviorTreeView(doc.Get(), LayoutBehaviorTree(doc.Get()), doc.Diagnostics());
    CHECK(m.nodes[2].error.find("BT009") == 0);
    doc.Undo();

    // The debugger: active nodes highlighted, their wires lit, finished ones coloured.
    Blackboard bb(doc.Get().blackboard);
    BtHooks hooks;
    BehaviorTreeInstance inst(doc.Get());
    BtContext ctx{nullptr, kNullEntity, &bb, &hooks};
    inst.Tick(ctx, 0.1f); // no enemy: the chase fails its condition, the Wait runs
    m = BuildBehaviorTreeView(doc.Get(), LayoutBehaviorTree(doc.Get()), doc.Diagnostics(), &inst);
    CHECK(m.nodes[0].highlighted && m.nodes[4].highlighted && !m.nodes[1].highlighted);
    CHECK(m.nodes[1].header_color != m.nodes[4].header_color); // failed: red
    CHECK(m.links[3].glow == 1.0f && m.links[0].glow == 0.0f);
}

AETHER_TEST(AIEditor_GraphEdits) {
    BehaviorTreeDocument doc = GuardDoc();
    auto layout = LayoutBehaviorTree(doc.Get());
    // A wire from the root's out to the Log: it moves to the root's end.
    GraphViewResult r;
    r.connect = true;
    r.connect_from = {1, "out", true};
    r.connect_to = {4, "in", false}; // the Log (id 4)
    CHECK(ApplyBehaviorTreeEdits(doc, layout, r).empty());
    CHECK(doc.Node({2}) != nullptr && doc.Node({2})->type == BtNodeType::Log);
    // Wiring under a task is refused.
    layout = LayoutBehaviorTree(doc.Get());
    r.connect_from = {3, "out", true}; // the MoveTo
    CHECK(ApplyBehaviorTreeEdits(doc, layout, r).size() == 1);
    // Dragging the Log left of the Wait reorders them.
    layout = LayoutBehaviorTree(doc.Get());
    GraphViewResult drag;
    const u32 log_id = layout.back().id;
    drag.moved = {{log_id, {layout[3].x - 10.0f, 0.0f}}};
    CHECK(ApplyBehaviorTreeEdits(doc, layout, drag).empty());
    CHECK(doc.Node({1})->type == BtNodeType::Log && doc.Node({2})->type == BtNodeType::Wait);
    // Breaking a wire is refused with a hint.
    GraphViewResult cut;
    cut.disconnect = true;
    CHECK(ApplyBehaviorTreeEdits(doc, LayoutBehaviorTree(doc.Get()), cut).size() == 1);
    // Delete: the root is kept; a node and its descendant go together.
    layout = LayoutBehaviorTree(doc.Get());
    GraphViewResult del;
    del.deleted = {1, 2, 3}; // the root, the chase sequence and its MoveTo
    CHECK(ApplyBehaviorTreeEdits(doc, layout, del).size() == 1);
    CHECK(doc.Get().root.children.size() == 2 && doc.Node({0})->type == BtNodeType::Log);
}

AETHER_TEST(AIEditor_PanelHeadless) {
    HeadlessImGui ui;
    BehaviorTreeDocument doc = GuardDoc();
    BehaviorTreeEditor editor(doc);
    for (int i = 0; i < 3; ++i) ui.Frame([&] { editor.Draw(); });
    CHECK(editor.Layout().size() == 5);
    // Select a node; its details draw.
    editor.Select({0, 0});
    CHECK(editor.Selected() && *editor.Selected() == BtPath({0, 0}));
    CHECK(editor.View().selection.count(3) == 1);
    ui.Frame([&] { editor.Draw(); });
    // The palette only opens for composites; placing adds under it and selects it.
    editor.OpenPalette({0, 0});
    CHECK(!editor.PaletteOpen() && !editor.Status().empty());
    editor.OpenPalette({0});
    CHECK(editor.PaletteOpen());
    ui.Frame([&] { editor.Draw(); });
    const auto placed = editor.PlaceNode(BtNodeType::Wait);
    CHECK(placed && *placed == BtPath({0, 2}) && !editor.PaletteOpen());
    CHECK(editor.Selected() == placed);
    ui.Frame([&] { editor.Draw(); });
    // A selection that goes away is dropped.
    doc.RemoveNode({0, 2});
    ui.Frame([&] { editor.Draw(); });
    CHECK(!editor.Selected());
    // Debugging draws the live panel.
    Blackboard bb(doc.Get().blackboard);
    BtHooks hooks;
    BehaviorTreeInstance inst(doc.Get());
    BtContext ctx{nullptr, kNullEntity, &bb, &hooks};
    inst.Tick(ctx, 0.1f);
    editor.SetLiveInstance(&inst, &bb);
    for (int i = 0; i < 2; ++i) ui.Frame([&] { editor.Draw(); });
    editor.SetLiveInstance(nullptr);
    // Undo with Ctrl+Z (the window is focused).
    const usize count = doc.Get().root.children[0].children.size();
    doc.AddNode({0}, BtNodeType::Succeed);
    ui.Frame([&] { editor.Draw(); });
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_Z, true);
    ui.Frame([&] { editor.Draw(); });
    io.AddKeyEvent(ImGuiKey_Z, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    ui.Frame([&] { editor.Draw(); });
    CHECK(doc.Get().root.children[0].children.size() == count);
    // Saving without a file says why.
    CHECK(!editor.SaveNow() && !editor.Status().empty());
}

AETHER_TEST(AIEditor_NavigationPanel) {
    nav::NavMeshSettings s;
    s.agent_radius = 0.4f;
    s.tile_size = 32;
    nav::NavMeshSettings back;
    std::string error;
    CHECK(NavSettingsFromJson(NavSettingsToJson(s), back, &error));
    CHECK(back.agent_radius == 0.4f && back.tile_size == 32 && back.cell_size == s.cell_size);
    nlohmann::json bad = NavSettingsToJson(s);
    bad["tile_size"] = "big";
    CHECK(!NavSettingsFromJson(bad, back, &error) && !error.empty());

    World world;
    ModelRenderer floor;
    SetModelPath(floor, "floor");
    world.CreateEntity(Transform{}, floor);
    NavObstacle crate;
    world.CreateEntity(Transform{Vec3(3, 0.5f, 3), Quaternion::Identity()}, crate);
    nav::NavWorld nav(world, Floor);
    nav::NavCrowd crowd(world, nav);
    NavigationPanel panel(world, nav, &crowd);
    HeadlessImGui ui;
    ui.Frame([&] { panel.Draw(); });
    nav::NavDebugDraw draw;
    panel.BuildOverlay(draw);
    CHECK(draw.triangles.empty()); // not baked
    panel.settings = s;
    CHECK(panel.Bake());
    CHECK(panel.LastBake().tiles > 0 && panel.Status().find("Baked") == 0);
    ui.Frame([&] { panel.Draw(); });
    panel.BuildOverlay(draw);
    CHECK(!draw.triangles.empty());
    CHECK(std::count_if(draw.lines.begin(), draw.lines.end(), [](const nav::NavDebugLine& l) { return l.color == nav::nav_colors::kObstacle; }) == 12);
    panel.show_overlay = false;
    panel.BuildOverlay(draw);
    CHECK(draw.triangles.empty() && draw.lines.empty());
    // Bad settings: the bake fails and says so.
    panel.settings.cell_size = 0.0f;
    CHECK(!panel.Bake() && panel.Status().find("Bake failed") == 0);
    ui.Frame([&] { panel.Draw(); });
}
