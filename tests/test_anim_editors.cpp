#include "anim/anim_graph_editor.h"
#include "anim/anim_viewer.h"
#include "anim/blend_space_editor.h"
#include "test_framework.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <map>

using namespace aether;
using namespace aether::editor;
using namespace aether::anim;

// Phase 16 step 6: the animation graph, blend space and clip editors (headless).

namespace {

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGui::GetIO().ConfigMacOSXBehaviors = false; // tests press Ctrl on every platform
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
        ImGui::Begin("Anim", nullptr, ImGuiWindowFlags_NoMove);
        body();
        ImGui::End();
        ImGui::Render();
    }

private:
    ImGuiContext* context_ = nullptr;
};

// A locomotion graph built through the document: Slot <- State Machine (Idle <-> Walk by Speed).
struct Built {
    AnimGraphDocument doc;
    u32 slot = 0, machine_node = 0;
    std::string machine;
    Built() {
        doc.AddVariable(VarType::Float);
        doc.RenameVariable("NewVar", "Speed");
        slot = doc.AddNode(AnimNodeKind::Slot, 400, 0);
        machine = doc.AddMachine(100, 0, &machine_node);
        doc.ConnectPose(machine_node, slot, 0);
        doc.SetOutput(slot);
        doc.AddState(machine, 0, 0);
        doc.AddState(machine, 300, 0);
        doc.RenameState(machine, "State", "Idle");
        doc.RenameState(machine, "State_1", "Walk");
        const i32 t = doc.AddTransition(machine, 0, 1);
        doc.Edit("Condition", [&](AnimGraph& g) { g.machines[0].transitions[static_cast<usize>(t)].conditions.push_back({"Speed", CompareOp::Greater, 0.1f}); });
        doc.AddTransition(machine, 1, 0);
        doc.Edit("Clips", [&](AnimGraph& g) {
            for (AnimNode& n : g.nodes) {
                if (n.kind == AnimNodeKind::Clip) n.asset = n.id == g.machines[0].states[0].pose ? "Idle" : "Walk";
            }
        });
    }
};

const AnimNode* FindKind(const AnimGraph& g, AnimNodeKind k) {
    for (const AnimNode& n : g.nodes) {
        if (n.kind == k) return &n;
    }
    return nullptr;
}

} // namespace

AETHER_TEST(AnimEditors_DocumentKeepsReferencesStraight) {
    Built b;
    AnimGraphDocument& doc = b.doc;
    const AnimGraph& g = doc.Get();
    AETHER_CHECK(g.machines.size() == 1 && g.machines[0].states.size() == 2 && g.nodes.size() == 4 && g.output == b.slot);
    AETHER_CHECK(g.machines[0].states[1].pose != 0 && g.Find(g.machines[0].states[1].pose)->kind == AnimNodeKind::Clip);
    AETHER_CHECK(doc.ErrorCount() == 0);

    // Variables: renames reach conditions and nodes; retypes drop conditions that no longer fit; removal clears references.
    u32 blend = doc.AddNode(AnimNodeKind::Blend, 0, 0);
    doc.Edit("Var", [&](AnimGraph& x) { x.nodes.back().variable = "Speed"; });
    std::string error;
    AETHER_CHECK(doc.RenameVariable("Speed", "Velocity", &error));
    AETHER_CHECK(g.machines[0].transitions[0].conditions[0].variable == "Velocity" && g.Find(blend)->variable == "Velocity");
    AETHER_CHECK(!doc.RenameVariable("Velocity", "", &error) && !doc.RenameVariable("Nope", "X", &error));
    doc.AddVariable(VarType::Bool);
    AETHER_CHECK(!doc.RenameVariable("NewVar", "Velocity", &error)); // taken
    AETHER_CHECK(doc.SetVariableType("Velocity", VarType::Bool) && g.machines[0].transitions[0].conditions.empty());
    AETHER_CHECK(doc.Undo() && g.machines[0].transitions[0].conditions.size() == 1 && g.FindVariable("Velocity")->type == VarType::Float);
    AETHER_CHECK(doc.RemoveVariable("Velocity") && g.Find(blend)->variable.empty() && g.machines[0].transitions[0].conditions.empty());
    AETHER_CHECK(doc.Undo() && doc.Redo() && !doc.RemoveVariable("Velocity"));
    doc.Undo();

    // Pose links: into an input, refusing loops and non-inputs; deleting unsets users.
    AETHER_CHECK(doc.ConnectPose(b.slot, blend, 1, &error) && g.Find(blend)->inputs[1] == b.slot);
    AETHER_CHECK(!doc.ConnectPose(blend, b.slot, 0, &error) && error.find("loop") != std::string::npos);
    AETHER_CHECK(!doc.ConnectPose(blend, b.machine_node, 0, &error) && !doc.ConnectPose(blend, blend, 0, &error));
    const u32 by_int = doc.AddNode(AnimNodeKind::BlendByInt, 0, 0);
    AETHER_CHECK(doc.ConnectPose(b.slot, by_int, 3) && g.Find(by_int)->inputs.size() == 4); // grows
    doc.DeleteNodes({b.slot});
    AETHER_CHECK(g.output == 0 && g.Find(blend)->inputs[1] == 0 && g.Find(by_int)->inputs[3] == 0);
    AETHER_CHECK(doc.Undo() && g.output == b.slot);

    // States: removal fixes transition indices and the entry.
    AETHER_CHECK(doc.AddState(b.machine, 600, 0, true) == 2 && g.machines[0].states[2].conduit && g.machines[0].states[2].pose == 0);
    doc.AddTransition(b.machine, 2, 0);
    doc.AddTransition(b.machine, anim::kAnyState, 1);
    AETHER_CHECK(doc.AddTransition(b.machine, 1, 1) < 0 && doc.AddTransition(b.machine, 0, 9) < 0);
    AETHER_CHECK(doc.SetEntry(b.machine, "Walk") && g.machines[0].entry == 1);
    AETHER_CHECK(doc.RemoveState(b.machine, "Idle"));
    const StateMachine& m = g.machines[0];
    AETHER_CHECK(m.states.size() == 2 && m.entry == 0 && m.states[0].name == "Walk" && m.transitions.size() == 1);
    AETHER_CHECK(m.transitions[0].from == anim::kAnyState && m.transitions[0].to == 0);
    AETHER_CHECK(!doc.RenameState(b.machine, "Walk", "Conduit", &error) && doc.RenameState(b.machine, "Walk", "Run", &error));
    AETHER_CHECK(doc.RemoveTransition(b.machine, 0) && !doc.RemoveTransition(b.machine, 0));
    // Machines: renames reach their nodes; removal takes the nodes along.
    AETHER_CHECK(doc.RenameMachine(b.machine, "Locomotion", &error) && g.Find(b.machine_node)->machine == "Locomotion");
    AETHER_CHECK(doc.RemoveMachine("Locomotion") && g.machines.empty() && g.Find(b.machine_node) == nullptr);
    AETHER_CHECK(doc.Undo() && g.Find(b.machine_node) != nullptr && g.machines.size() == 1);

    // Files.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_anim_editor_test";
    std::filesystem::create_directories(dir);
    AETHER_CHECK(!doc.Save(&error) && doc.SaveAs(dir / "ABP_Hero.aanim", &error) && doc.Name() == "ABP_Hero" && !doc.Dirty());
    AnimGraphDocument back;
    AETHER_CHECK(back.Load(dir / "ABP_Hero.aanim", &error) && anim::AnimGraphToJson(back.Get()) == anim::AnimGraphToJson(doc.Get()));
    AETHER_CHECK(back.Get().machines[0].states[0].x == doc.Get().machines[0].states[0].x);
    AETHER_CHECK(!back.Load(dir / "missing.aanim", &error));
    std::filesystem::remove_all(dir);
}

AETHER_TEST(AnimEditors_ViewsAndGraphEdits) {
    Built b;
    AnimGraphDocument& doc = b.doc;
    GraphViewModel pose = BuildPoseGraphView(doc.Get(), doc.Diagnostics());
    const GraphNodeView* slot = pose.Find(b.slot);
    AETHER_CHECK(slot != nullptr && slot->inputs.size() == 1 && slot->inputs[0].name == "Source" && slot->inputs[0].connected);
    AETHER_CHECK(pose.Find(kOutputNode) != nullptr && pose.Find(kOutputNode)->inputs[0].connected);
    AETHER_CHECK(std::any_of(pose.links.begin(), pose.links.end(), [&](const GraphLinkView& l) { return l.from_node == b.slot && l.to_node == kOutputNode; }));
    AETHER_CHECK(PoseInputPins(*FindKind(doc.Get(), AnimNodeKind::StateMachine)).empty() && PoseNodeTitle(*doc.Get().Find(b.slot)) == "Slot: Default");

    // Pose edits: connect into a named pin, set the output, break, delete (the output node can't be).
    const u32 blend = doc.AddNode(AnimNodeKind::Blend, 0, 0);
    GraphViewResult r;
    r.connect = true;
    r.connect_from = {b.machine_node, "pose", true};
    r.connect_to = {blend, "B", false};
    AETHER_CHECK(ApplyPoseGraphEdits(doc, r).empty() && doc.Get().Find(blend)->inputs[1] == b.machine_node);
    r.connect_to = {blend, "Nope", false};
    AETHER_CHECK(ApplyPoseGraphEdits(doc, r).size() == 1);
    r.connect_from = {blend, "pose", true};
    r.connect_to = {kOutputNode, "pose", false};
    AETHER_CHECK(ApplyPoseGraphEdits(doc, r).empty() && doc.Get().output == blend);
    GraphViewResult br;
    br.disconnect = true;
    br.disconnected = {b.machine_node, "pose", blend, "B", 0, 0};
    ApplyPoseGraphEdits(doc, br);
    AETHER_CHECK(doc.Get().Find(blend)->inputs[1] == 0);
    GraphViewResult mv;
    mv.moved = {{blend, {12.0f, 34.0f}}};
    mv.deleted = {kOutputNode};
    ApplyPoseGraphEdits(doc, mv);
    AETHER_CHECK(doc.Get().Find(blend)->x == 12.0f && doc.Get().Find(blend)->y == 34.0f);
    // Errors show on their nodes.
    pose = BuildPoseGraphView(doc.Get(), doc.Diagnostics());
    AETHER_CHECK(!pose.Find(blend)->error.empty()); // no variable, no inputs

    // The machine view: Entry, Any State, transitions, the live state highlighted.
    const StateMachine& m = doc.Get().machines[0];
    GraphViewModel mview = BuildStateMachineView(m, "Walk", true);
    AETHER_CHECK(mview.nodes.size() == 4 && mview.Find(kEntryNode) != nullptr && mview.Find(kAnyStateNode) != nullptr);
    AETHER_CHECK(mview.Find(kStateNodeBase + 1)->highlighted && !mview.Find(kStateNodeBase)->highlighted);
    AETHER_CHECK(mview.links.size() == 3); // entry + 2 transitions
    AETHER_CHECK(std::any_of(mview.links.begin(), mview.links.end(), [](const GraphLinkView& l) { return l.to_node == kStateNodeBase + 1 && l.glow > 0; }));
    // Machine edits: a state-to-state link adds a transition; Entry sets the entry; Any State adds an any-state transition.
    GraphViewResult e;
    e.connect = true;
    e.connect_from = {kEntryNode, "out", true};
    e.connect_to = {kStateNodeBase + 1, "in", false};
    AETHER_CHECK(ApplyStateMachineEdits(doc, b.machine, e).empty() && doc.Get().machines[0].entry == 1);
    e.connect_from = {kAnyStateNode, "out", true};
    e.connect_to = {kStateNodeBase, "in", false};
    ApplyStateMachineEdits(doc, b.machine, e);
    AETHER_CHECK(doc.Get().machines[0].transitions.back().from == anim::kAnyState);
    e.connect_from = {kStateNodeBase, "out", true};
    AETHER_CHECK(ApplyStateMachineEdits(doc, b.machine, e).size() == 1); // to itself
    GraphViewResult d;
    d.disconnect = true;
    d.disconnected = {kAnyStateNode, "out", kStateNodeBase, "in", 0, 0};
    ApplyStateMachineEdits(doc, b.machine, d);
    AETHER_CHECK(doc.Get().machines[0].transitions.size() == 2);
    d.disconnected = {kEntryNode, "out", kStateNodeBase + 1, "in", 0, 0};
    AETHER_CHECK(ApplyStateMachineEdits(doc, b.machine, d).size() == 1); // entry stays
    GraphViewResult del;
    del.moved = {{kStateNodeBase, {5.0f, 6.0f}}};
    del.deleted = {kStateNodeBase + 1, kEntryNode};
    ApplyStateMachineEdits(doc, b.machine, del);
    AETHER_CHECK(doc.Get().machines[0].states.size() == 1 && doc.Get().machines[0].states[0].x == 5.0f && doc.Get().machines[0].transitions.empty());
}

AETHER_TEST(AnimEditors_BlendSpaceEditing) {
    BlendSpace space;
    space.dimensions = 2;
    space.x = {"Right", -1, 1};
    space.y = {"Forward", 0, 2};
    BlendSpaceDocument doc(space);
    AETHER_CHECK(doc.AddSample("Idle", 0, 0) == 0 && doc.AddSample("Fwd", 0, 2) == 1 && doc.AddSample("Right", 1, 1) == 2);
    AETHER_CHECK(doc.Get().triangles.size() == 1 && doc.Diagnostics().empty());
    // Snapped moves (a quarter grid), clamped to the axes, one undo step per drag.
    AETHER_CHECK(doc.MoveSample(2, 0.9f, 1.1f, 4, "drag") && doc.Get().samples[2].x == 1.0f && doc.Get().samples[2].y == 1.0f);
    doc.MoveSample(2, 0.3f, 5.0f, 4, "drag");
    AETHER_CHECK(doc.Get().samples[2].x == 0.5f && doc.Get().samples[2].y == 2.0f);
    AETHER_CHECK(doc.Undo() && doc.Get().samples[2].x == 1.0f && doc.Get().samples[2].y == 1.0f);
    AETHER_CHECK(doc.Redo() && doc.Get().samples[2].x == 0.5f);
    doc.MoveSample(2, 0.0f, 1.0f); // onto the line between the others: collinear
    AETHER_CHECK(doc.Get().triangles.empty() && std::any_of(doc.Diagnostics().begin(), doc.Diagnostics().end(), [](const BlendDiagnostic& x) { return x.code == "BS003"; }));
    AETHER_CHECK(doc.RemoveSample(2) && !doc.RemoveSample(9) && doc.Get().samples.size() == 2 && doc.Dirty());

    // The canvas: parameters to the screen and back, y up; hit testing.
    BlendSpaceCanvas c{100, 50, 400, 200};
    f32 sx, sy, px, py;
    c.ParamToScreen(doc.Get(), -1, 0, sx, sy);
    AETHER_CHECK(sx == 100 && sy == 250); // bottom left
    c.ParamToScreen(doc.Get(), 1, 2, sx, sy);
    AETHER_CHECK(sx == 500 && sy == 50);
    c.ScreenToParam(doc.Get(), 300, 150, px, py);
    AETHER_CHECK(std::fabs(px) < 1e-5f && std::fabs(py - 1) < 1e-5f);
    c.ParamToScreen(doc.Get(), 0, 2, sx, sy);
    AETHER_CHECK(c.HitTest(doc.Get(), sx + 3, sy - 2) == 1 && c.HitTest(doc.Get(), sx + 30, sy) == -1);
    BlendSpaceEditor editor(doc);
    editor.preview_x = 0;
    editor.preview_y = 1;
    AETHER_CHECK(editor.PreviewWeights().size() == 2);
}

AETHER_TEST(AnimEditors_ClipViewerPlaybackAndNotifies) {
    Skeleton s;
    s.bones = {{"root", -1, {}, Mat4::Identity()}, {"arm", 0, {}, Mat4::Identity()}};
    AnimationClip clip;
    clip.duration = 1.0f;
    clip.tracks.resize(2);
    clip.tracks[1].translation.times = {0.0f, 1.0f};
    clip.tracks[1].translation.values = {Vec3(0, 0, 0), Vec3(0, 2, 0)};
    clip.notifies = {{"Step", 0.5f}};
    ClipViewer viewer(s, clip);
    viewer.SetTime(0.25f);
    AETHER_CHECK(std::fabs(viewer.CurrentPose().local[1].translation.y - 0.5f) < 1e-5f);
    viewer.Advance(0.5f); // paused: nothing moves
    AETHER_CHECK(viewer.Time() == 0.25f && viewer.Fired().empty());
    viewer.playing = true;
    viewer.Advance(0.5f);
    AETHER_CHECK(std::fabs(viewer.Time() - 0.75f) < 1e-5f && viewer.Fired() == (std::vector<std::string>{"Step"}));
    viewer.Advance(0.5f); // loops
    AETHER_CHECK(std::fabs(viewer.Time() - 0.25f) < 1e-5f && viewer.playing);
    viewer.loop = false;
    viewer.Advance(2.0f); // stops at the end
    AETHER_CHECK(viewer.Time() == 1.0f && !viewer.playing);

    // Notify editing with undo.
    const usize hit = viewer.AddNotify("Hit", 0.8f, 0.1f);
    AETHER_CHECK(clip.notifies.size() == 2 && clip.notifies[hit].duration == 0.1f && viewer.Dirty());
    AETHER_CHECK(viewer.MoveNotify(hit, 9.0f) && clip.notifies[hit].time == 1.0f); // clamped
    AETHER_CHECK(viewer.RenameNotify(hit, "Impact") && !viewer.RenameNotify(hit, "") && !viewer.MoveNotify(9, 0));
    AETHER_CHECK(viewer.Undo() && clip.notifies[hit].name == "Hit" && viewer.Undo() && clip.notifies[hit].time == 0.8f);
    AETHER_CHECK(viewer.Redo() && clip.notifies[hit].time == 1.0f);
    AETHER_CHECK(viewer.RemoveNotify(0) && clip.notifies.size() == 1 && viewer.Undo() && clip.notifies.size() == 2);
    // Curves.
    const std::vector<f32> curve = viewer.Curve(1, 1, 5);
    AETHER_CHECK(curve.size() == 5 && curve.front() == 0.0f && std::fabs(curve[2] - 1.0f) < 1e-5f && std::fabs(curve.back() - 2.0f) < 1e-5f);
    AETHER_CHECK(viewer.Curve(7, 0).empty());
}

AETHER_TEST(AnimEditors_PanelsDrawHeadless) {
    HeadlessImGui imgui;
    Built b;
    AnimGraphEditor editor(b.doc);
    for (int i = 0; i < 2; ++i) imgui.Frame([&] { editor.Draw(); });
    AETHER_CHECK(editor.ActiveTab().empty() && editor.Tabs().size() == 1);
    // Every Details panel and the machine tab.
    editor.OpenTab(b.machine);
    AETHER_CHECK(editor.ActiveTab() == b.machine && editor.Tabs().size() == 2);
    editor.OpenTab("Nope");
    AETHER_CHECK(editor.Tabs().size() == 2);
    const std::vector<AnimGraphEditor::Item> items = {
        {AnimGraphEditor::ItemKind::Variable, "Speed", 0, -1},
        {AnimGraphEditor::ItemKind::Node, {}, b.slot, -1},
        {AnimGraphEditor::ItemKind::Node, {}, b.machine_node, -1},
        {AnimGraphEditor::ItemKind::State, b.machine, 0, 0},
        {AnimGraphEditor::ItemKind::Transition, b.machine, 0, 0},
    };
    for (const auto& item : items) {
        editor.Select(item);
        imgui.Frame([&] { editor.Draw(); });
        AETHER_CHECK(editor.Selected() == item);
    }
    for (AnimNodeKind k : {AnimNodeKind::Clip, AnimNodeKind::BlendSpace, AnimNodeKind::Blend, AnimNodeKind::BlendByBool, AnimNodeKind::BlendByInt,
                           AnimNodeKind::Layered, AnimNodeKind::Additive}) {
        editor.OpenTab("");
        editor.OpenPalette(50, 60);
        imgui.Frame([&] { editor.Draw(); });
        AETHER_CHECK(editor.PaletteOpen());
        const u32 id = editor.PlaceNode(k);
        AETHER_CHECK(id != 0 && b.doc.Get().Find(id)->kind == k && b.doc.Get().Find(id)->x == 50.0f && !editor.PaletteOpen());
        imgui.Frame([&] { editor.Draw(); });
    }
    editor.OpenTab(b.machine);
    editor.OpenPalette(10, 20);
    const i32 conduit = editor.PlaceState(true);
    AETHER_CHECK(conduit == 2 && b.doc.Get().machines[0].states[2].conduit);
    // Live (PIE): the instance's current state is highlighted; undoing a selection's subject forgets it.
    std::map<std::string, AnimationClip> clips;
    clips["Idle"].duration = 1.0f;
    clips["Walk"].duration = 1.0f;
    Skeleton rig;
    rig.bones = {{"root", -1, {}, Mat4::Identity()}};
    AnimAssets assets{[&](const std::string& n) -> const AnimationClip* { return clips.count(n) ? &clips[n] : nullptr; }, {}};
    AnimGraphInstance instance(b.doc.Get(), rig, assets);
    Pose pose;
    instance.SetFloat("Speed", 1.0f);
    instance.Update(0.05f, pose);
    editor.SetLiveInstance(&instance);
    imgui.Frame([&] { editor.Draw(); });
    AETHER_CHECK(instance.CurrentState(b.machine_node) == "Walk");
    editor.Select({AnimGraphEditor::ItemKind::State, b.machine, 0, 2});
    b.doc.Undo(); // the conduit
    imgui.Frame([&] { editor.Draw(); });
    AETHER_CHECK(editor.Selected().kind == AnimGraphEditor::ItemKind::None); // the state it showed is gone
    editor.SetLiveInstance(nullptr);
    AETHER_CHECK(!editor.SaveNow() && editor.Status().rfind("Save failed", 0) == 0);

    // The blend space editor and clip viewer draw too.
    BlendSpace space;
    space.dimensions = 2;
    space.samples = {{"Idle", 0, 0}, {"Walk", 1, 0}, {"Run", 0, 1}};
    BlendSpaceDocument bs(space);
    BlendSpaceEditor bse(bs);
    bse.SetClipNames({"Idle", "Walk", "Run"});
    bse.Select(1);
    for (int i = 0; i < 2; ++i) imgui.Frame([&] { bse.Draw(); });
    AETHER_CHECK(bse.Canvas().width > 0 && bse.Selected() == 1);
    AnimationClip clip;
    clip.duration = 1.0f;
    clip.tracks.resize(1);
    clip.notifies = {{"Step", 0.5f}};
    ClipViewer viewer(rig, clip);
    viewer.selected_bone = 0;
    viewer.selected_notify = 0;
    viewer.playing = true;
    for (int i = 0; i < 3; ++i) imgui.Frame([&] { viewer.Draw(); });
    AETHER_CHECK(viewer.Time() > 0.0f);
}
