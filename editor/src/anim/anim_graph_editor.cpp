#include "anim/anim_graph_editor.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cstdio>

namespace aether::editor {

using anim::AnimGraph;
using anim::AnimNode;
using anim::AnimNodeKind;
using anim::CompareOp;
using anim::VarType;

namespace {

constexpr f32 kLeftWidth = 220.0f;
constexpr f32 kRightWidth = 300.0f;
constexpr f32 kBottomHeight = 120.0f;

const AnimNodeKind kKinds[] = {AnimNodeKind::Clip,        AnimNodeKind::BlendSpace, AnimNodeKind::Blend,    AnimNodeKind::BlendByBool,
                               AnimNodeKind::BlendByInt,  AnimNodeKind::Layered,    AnimNodeKind::Additive, AnimNodeKind::StateMachine,
                               AnimNodeKind::Slot};
const VarType kTypes[] = {VarType::Bool, VarType::Int, VarType::Float, VarType::Trigger};
const char* kOpLabels[] = {"==", "!=", "<", "<=", ">", ">=", "is true", "is false", "triggered"};

u32 HeaderColor(AnimNodeKind k) {
    switch (k) {
    case AnimNodeKind::Clip: return IM_COL32(40, 105, 55, 255);
    case AnimNodeKind::BlendSpace: return IM_COL32(40, 90, 130, 255);
    case AnimNodeKind::StateMachine: return IM_COL32(95, 70, 125, 255);
    case AnimNodeKind::Slot: return IM_COL32(150, 90, 30, 255);
    default: return IM_COL32(35, 75, 150, 255);
    }
}
constexpr u32 kPoseColor = IM_COL32(230, 230, 230, 255);

bool IsMachineTab(const std::string& tab) { return !tab.empty(); }

// Where the pseudo nodes go: left (or right) of the others.
void Bounds(const std::vector<std::pair<f32, f32>>& points, f32& min_x, f32& max_x, f32& mid_y) {
    min_x = 0, max_x = 0, mid_y = 0;
    if (points.empty()) return;
    min_x = max_x = points[0].first;
    for (const auto& [x, y] : points) {
        min_x = std::min(min_x, x), max_x = std::max(max_x, x), mid_y += y;
    }
    mid_y /= static_cast<f32>(points.size());
}

bool HasEdits(const GraphViewResult& r) { return !r.moved.empty() || r.connect || r.disconnect || !r.deleted.empty(); }

} // namespace

std::vector<std::string> PoseInputPins(const AnimNode& n) {
    switch (n.kind) {
    case AnimNodeKind::Blend: return {"A", "B"};
    case AnimNodeKind::BlendByBool: return {"False", "True"};
    case AnimNodeKind::BlendByInt: {
        std::vector<std::string> pins;
        for (usize i = 0; i < std::max<usize>(n.inputs.size(), 2); ++i) pins.push_back(std::to_string(i));
        return pins;
    }
    case AnimNodeKind::Layered: return {"Base", "Layer"};
    case AnimNodeKind::Additive: return {"Base", "Additive"};
    case AnimNodeKind::Slot: return {"Source"};
    default: return {};
    }
}

std::string PoseNodeTitle(const AnimNode& n) {
    auto with = [](const char* kind, const std::string& what) { return what.empty() ? std::string(kind) : std::string(kind) + ": " + what; };
    switch (n.kind) {
    case AnimNodeKind::Clip: return with("Clip", n.asset);
    case AnimNodeKind::BlendSpace: return with("Blend Space", n.asset);
    case AnimNodeKind::Blend: return with("Blend", n.variable);
    case AnimNodeKind::BlendByBool: return with("Blend by Bool", n.variable);
    case AnimNodeKind::BlendByInt: return with("Blend by Int", n.variable);
    case AnimNodeKind::Layered: return with("Layered", n.bone);
    case AnimNodeKind::Additive: return "Additive";
    case AnimNodeKind::StateMachine: return with("State Machine", n.machine);
    case AnimNodeKind::Slot: return with("Slot", n.slot);
    }
    return "";
}

GraphViewModel BuildPoseGraphView(const AnimGraph& g, const std::vector<anim::AnimDiagnostic>& diagnostics) {
    GraphViewModel model;
    std::vector<std::pair<f32, f32>> points;
    for (const AnimNode& n : g.nodes) {
        GraphNodeView v;
        v.id = n.id;
        v.title = PoseNodeTitle(n);
        v.header_color = HeaderColor(n.kind);
        v.x = n.x;
        v.y = n.y;
        const std::vector<std::string> pins = PoseInputPins(n);
        for (usize i = 0; i < pins.size(); ++i) {
            GraphPinView p;
            p.name = p.label = pins[i];
            p.color = kPoseColor;
            p.connected = i < n.inputs.size() && n.inputs[i] != 0;
            v.inputs.push_back(std::move(p));
        }
        GraphPinView out;
        out.name = "pose";
        out.color = kPoseColor;
        out.connected = g.output == n.id;
        v.outputs.push_back(std::move(out));
        for (const anim::AnimDiagnostic& d : diagnostics) {
            if (d.error && d.node == n.id && n.id != 0) v.error += (v.error.empty() ? "" : "\n") + d.code + ": " + d.message;
        }
        points.push_back({n.x, n.y});
        model.nodes.push_back(std::move(v));
        for (usize i = 0; i < n.inputs.size() && i < pins.size(); ++i) {
            if (n.inputs[i] != 0 && g.Find(n.inputs[i]) != nullptr) model.links.push_back({n.inputs[i], "pose", n.id, pins[i], kPoseColor, 0.0f});
        }
    }
    f32 min_x, max_x, mid_y;
    Bounds(points, min_x, max_x, mid_y);
    GraphNodeView out;
    out.id = kOutputNode;
    out.title = "Output Pose";
    out.header_color = IM_COL32(120, 80, 30, 255);
    out.x = max_x + 280.0f;
    out.y = mid_y;
    GraphPinView in;
    in.name = in.label = "pose";
    in.color = kPoseColor;
    in.connected = g.Find(g.output) != nullptr;
    out.inputs.push_back(std::move(in));
    model.nodes.push_back(std::move(out));
    if (g.Find(g.output) != nullptr) model.links.push_back({g.output, "pose", kOutputNode, "pose", kPoseColor, 0.0f});
    return model;
}

GraphViewModel BuildStateMachineView(const anim::StateMachine& m, const std::string& active, bool blending) {
    GraphViewModel model;
    std::vector<std::pair<f32, f32>> points;
    for (usize i = 0; i < m.states.size(); ++i) {
        const anim::AnimState& s = m.states[i];
        GraphNodeView v;
        v.id = kStateNodeBase + static_cast<u32>(i);
        v.title = s.conduit ? "Conduit: " + s.name : s.name;
        v.header_color = s.conduit ? IM_COL32(85, 85, 90, 255) : IM_COL32(35, 75, 150, 255);
        v.x = s.x;
        v.y = s.y;
        v.pure = true; // rounder: states read as boxes, not function calls
        v.highlighted = !active.empty() && s.name == active;
        v.inputs.push_back({"in", "", false, false, kPoseColor, false});
        v.outputs.push_back({"out", "", false, false, kPoseColor, false});
        points.push_back({s.x, s.y});
        model.nodes.push_back(std::move(v));
    }
    f32 min_x, max_x, mid_y;
    Bounds(points, min_x, max_x, mid_y);
    GraphNodeView entry;
    entry.id = kEntryNode;
    entry.title = "Entry";
    entry.header_color = IM_COL32(40, 105, 55, 255);
    entry.x = min_x - 220.0f;
    entry.y = mid_y - 80.0f;
    entry.outputs.push_back({"out", "", false, false, kPoseColor, true});
    model.nodes.push_back(std::move(entry));
    GraphNodeView any;
    any.id = kAnyStateNode;
    any.title = "Any State";
    any.header_color = IM_COL32(120, 80, 30, 255);
    any.x = min_x - 220.0f;
    any.y = mid_y + 80.0f;
    any.outputs.push_back({"out", "", false, false, kPoseColor, false});
    model.nodes.push_back(std::move(any));
    if (m.entry < m.states.size()) model.links.push_back({kEntryNode, "out", kStateNodeBase + m.entry, "in", IM_COL32(120, 200, 120, 255), 0.0f});
    for (const anim::Transition& t : m.transitions) {
        if (t.to >= m.states.size() || (t.from != anim::kAnyState && static_cast<usize>(t.from) >= m.states.size())) continue;
        const u32 from = t.from == anim::kAnyState ? kAnyStateNode : kStateNodeBase + static_cast<u32>(t.from);
        const bool live = blending && m.states[t.to].name == active;
        model.links.push_back({from, "out", kStateNodeBase + t.to, "in", t.when_finished ? IM_COL32(200, 170, 90, 255) : kPoseColor, live ? 1.0f : 0.0f});
    }
    return model;
}

std::vector<std::string> ApplyPoseGraphEdits(AnimGraphDocument& doc, const GraphViewResult& r) {
    std::vector<std::string> errors;
    if (!r.moved.empty()) {
        doc.Edit("Move", [&](AnimGraph& g) {
            for (const auto& [id, pos] : r.moved) {
                for (AnimNode& n : g.nodes) {
                    if (n.id == id) n.x = pos.first, n.y = pos.second;
                }
            }
        });
    }
    if (r.disconnect) {
        doc.Edit("Break link", [&](AnimGraph& g) {
            if (r.disconnected.to_node == kOutputNode) {
                g.output = 0;
                return;
            }
            for (AnimNode& n : g.nodes) {
                if (n.id != r.disconnected.to_node) continue;
                const std::vector<std::string> pins = PoseInputPins(n);
                for (usize i = 0; i < pins.size() && i < n.inputs.size(); ++i) {
                    if (pins[i] == r.disconnected.to_pin) n.inputs[i] = 0;
                }
            }
        });
    }
    if (r.connect) {
        const GraphPinRef& from = r.connect_from.output ? r.connect_from : r.connect_to;
        const GraphPinRef& to = r.connect_from.output ? r.connect_to : r.connect_from;
        std::string error;
        if (from.output == to.output) {
            errors.push_back("Link an output to an input.");
        } else if (to.node == kOutputNode) {
            if (!doc.SetOutput(from.node)) errors.push_back("That node no longer exists.");
        } else if (const AnimNode* n = doc.Get().Find(to.node)) {
            const std::vector<std::string> pins = PoseInputPins(*n);
            const auto it = std::find(pins.begin(), pins.end(), to.pin);
            if (it == pins.end() || !doc.ConnectPose(from.node, to.node, static_cast<usize>(it - pins.begin()), &error)) {
                errors.push_back(error.empty() ? "That input doesn't exist." : error);
            }
        }
    }
    std::vector<u32> doomed;
    for (u32 id : r.deleted) {
        if (id != kOutputNode) doomed.push_back(id);
    }
    if (!doomed.empty()) doc.DeleteNodes(doomed);
    return errors;
}

std::vector<std::string> ApplyStateMachineEdits(AnimGraphDocument& doc, const std::string& machine, const GraphViewResult& r) {
    std::vector<std::string> errors;
    const anim::StateMachine* m = doc.Get().FindMachine(machine);
    if (m == nullptr) return {"That state machine no longer exists."};
    auto state_of = [&](u32 node) { return node >= kStateNodeBase && node < kStateNodeBase + m->states.size() ? static_cast<i32>(node - kStateNodeBase) : -1; };
    if (!r.moved.empty()) {
        doc.Edit("Move", [&](AnimGraph& g) {
            for (anim::StateMachine& sm : g.machines) {
                if (sm.name != machine) continue;
                for (const auto& [id, pos] : r.moved) {
                    if (id >= kStateNodeBase && id < kStateNodeBase + sm.states.size()) {
                        sm.states[id - kStateNodeBase].x = pos.first;
                        sm.states[id - kStateNodeBase].y = pos.second;
                    }
                }
            }
        });
        m = doc.Get().FindMachine(machine);
    }
    if (r.disconnect) {
        const i32 to = state_of(r.disconnected.to_node);
        if (r.disconnected.from_node == kEntryNode) {
            errors.push_back("A machine always has an entry; link Entry to another state instead.");
        } else if (to >= 0) {
            const i32 from = r.disconnected.from_node == kAnyStateNode ? anim::kAnyState : state_of(r.disconnected.from_node);
            for (usize i = 0; i < m->transitions.size(); ++i) {
                if (m->transitions[i].from == from && static_cast<i32>(m->transitions[i].to) == to) {
                    doc.RemoveTransition(machine, i);
                    break;
                }
            }
            m = doc.Get().FindMachine(machine);
        }
    }
    if (r.connect) {
        const GraphPinRef& from = r.connect_from.output ? r.connect_from : r.connect_to;
        const GraphPinRef& to = r.connect_from.output ? r.connect_to : r.connect_from;
        const i32 target = state_of(to.node);
        if (from.output == to.output || target < 0) {
            errors.push_back("Link a state's right side to another state's left side.");
        } else if (from.node == kEntryNode) {
            doc.SetEntry(machine, m->states[static_cast<usize>(target)].name);
        } else {
            const i32 source = from.node == kAnyStateNode ? anim::kAnyState : state_of(from.node);
            if (source == target || doc.AddTransition(machine, source, static_cast<u32>(target)) < 0) errors.push_back("A state can't transition to itself.");
        }
        m = doc.Get().FindMachine(machine);
    }
    // Deleting states, highest index first so the others keep theirs.
    std::vector<i32> states;
    for (u32 id : r.deleted) {
        if (const i32 s = state_of(id); s >= 0) states.push_back(s);
    }
    std::sort(states.rbegin(), states.rend());
    std::vector<std::string> names;
    for (i32 s : states) names.push_back(m->states[static_cast<usize>(s)].name);
    for (const std::string& n : names) doc.RemoveState(machine, n);
    return errors;
}

// --- The editor ------------------------------------------------------------------------------------

AnimGraphEditor::AnimGraphEditor(AnimGraphDocument& document) : doc_(document) { views_[""].request_fit = true; }

void AnimGraphEditor::Draw() {
    ImGui::PushID(this);
    ForgetMissing(); // the document may have changed underneath (another panel, undo from a menu)
    HandleKeys();
    DrawToolbar();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const f32 spacing = ImGui::GetStyle().ItemSpacing.x;
    const f32 top = std::max(100.0f, avail.y - kBottomHeight - ImGui::GetStyle().ItemSpacing.y);
    const f32 center = std::max(200.0f, avail.x - kLeftWidth - kRightWidth - 2 * spacing);
    ImGui::BeginChild("##variables", ImVec2(kLeftWidth, top), ImGuiChildFlags_Borders);
    DrawVariables();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##graphs", ImVec2(center, top), ImGuiChildFlags_None);
    DrawTabs();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##details", ImVec2(0, top), ImGuiChildFlags_Borders);
    DrawDetails();
    ImGui::EndChild();
    ImGui::BeginChild("##diagnostics", ImVec2(0, 0), ImGuiChildFlags_Borders);
    DrawDiagnostics();
    ImGui::EndChild();
    DrawPalette();
    ImGui::PopID();
}

void AnimGraphEditor::HandleKeys() {
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || ImGui::GetIO().WantTextInput) return;
    const bool ctrl = ImGui::GetIO().KeyCtrl;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        if (doc_.Undo()) ForgetMissing();
    } else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
        if (doc_.Redo()) ForgetMissing();
    } else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        SaveNow();
    }
}

void AnimGraphEditor::DrawToolbar() {
    if (ImGui::Button(doc_.Dirty() ? "Save*" : "Save")) SaveNow();
    ImGui::SameLine();
    ImGui::BeginDisabled(!doc_.CanUndo());
    if (ImGui::Button("Undo") && doc_.Undo()) ForgetMissing();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!doc_.CanRedo());
    if (ImGui::Button("Redo") && doc_.Redo()) ForgetMissing();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("+ State Machine")) {
        u32 node = 0;
        const std::string name = doc_.AddMachine(0, 0, &node);
        OpenTab(name);
        Select({ItemKind::Node, {}, node, -1});
    }
    ImGui::SameLine();
    const usize errors = doc_.ErrorCount();
    if (errors > 0) ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1), "%zu error(s)", errors);
    else ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1), "OK");
    ImGui::SameLine();
    ImGui::TextDisabled("|  %s%s", doc_.Name().c_str(), live_ != nullptr ? "  (live)" : "");
    if (!status_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("  %s", status_.c_str());
    }
}

void AnimGraphEditor::DrawVariables() {
    ImGui::SeparatorText("Variables");
    for (VarType t : kTypes) {
        if (ImGui::SmallButton((std::string("+") + anim::VarTypeName(t)).c_str())) Select({ItemKind::Variable, doc_.AddVariable(t), 0, -1});
        ImGui::SameLine();
    }
    ImGui::NewLine();
    for (const anim::AnimVariable& v : doc_.Get().variables) {
        const bool selected = selected_.kind == ItemKind::Variable && selected_.name == v.name;
        char label[160];
        if (live_ != nullptr) std::snprintf(label, sizeof label, "%s  (%s = %g)##var", v.name.c_str(), anim::VarTypeName(v.type), live_->Get(v.name));
        else std::snprintf(label, sizeof label, "%s  (%s)##var", v.name.c_str(), anim::VarTypeName(v.type));
        if (ImGui::Selectable(label, selected)) Select({ItemKind::Variable, v.name, 0, -1});
    }
    if (doc_.Get().variables.empty()) ImGui::TextDisabled("No variables yet.");
    ImGui::SeparatorText("State Machines");
    for (const anim::StateMachine& m : doc_.Get().machines) {
        if (ImGui::Selectable((m.name + "##machine").c_str(), active_ == m.name)) OpenTab(m.name);
    }
}

void AnimGraphEditor::OpenTab(const std::string& machine) {
    if (!machine.empty() && doc_.Get().FindMachine(machine) == nullptr) return;
    if (std::find(tabs_.begin(), tabs_.end(), machine) == tabs_.end()) {
        tabs_.push_back(machine);
        views_[machine].request_fit = true;
    }
    active_ = machine;
    select_tab_ = machine;
}

void AnimGraphEditor::DrawTabs() {
    if (!ImGui::BeginTabBar("##anim_tabs")) return;
    std::string close;
    for (const std::string& tab : tabs_) {
        const ImGuiTabItemFlags flags = select_tab_ == tab ? ImGuiTabItemFlags_SetSelected : 0;
        bool open = true;
        const std::string label = (tab.empty() ? std::string("Anim Graph") : tab) + "###tab_" + tab;
        if (ImGui::BeginTabItem(label.c_str(), tab.empty() ? nullptr : &open, flags)) {
            if (select_tab_.empty() || select_tab_ == tab) active_ = tab;
            DrawGraph(tab);
            ImGui::EndTabItem();
        }
        if (!open) close = tab;
    }
    select_tab_ = "\x01"; // consumed
    ImGui::EndTabBar();
    if (!close.empty()) {
        tabs_.erase(std::remove(tabs_.begin(), tabs_.end(), close), tabs_.end());
        if (active_ == close) active_.clear();
    }
}

std::string AnimGraphEditor::ActiveStateOf(const std::string& machine, bool& blending) const {
    blending = false;
    if (live_ == nullptr) return {};
    for (const AnimNode& n : doc_.Get().nodes) {
        if (n.kind == AnimNodeKind::StateMachine && n.machine == machine) {
            blending = live_->InTransition(n.id);
            return live_->CurrentState(n.id);
        }
    }
    return {};
}

void AnimGraphEditor::DrawGraph(const std::string& tab) {
    GraphViewState& view = views_[tab];
    if (!IsMachineTab(tab)) {
        const GraphViewModel model = BuildPoseGraphView(doc_.Get(), doc_.Diagnostics());
        const GraphViewResult r = DrawGraphView("pose_graph", model, view);
        if (HasEdits(r)) {
            const std::vector<std::string> errors = ApplyPoseGraphEdits(doc_, r);
            if (!errors.empty()) status_ = errors.front();
            ForgetMissing();
        }
        if (r.open_palette) OpenPalette(r.palette_x, r.palette_y);
        if (r.double_clicked != 0) {
            if (const AnimNode* n = doc_.Get().Find(r.double_clicked); n != nullptr && n->kind == AnimNodeKind::StateMachine) OpenTab(n->machine);
        }
        if (r.selection_changed || !r.deleted.empty()) {
            if (view.selection.size() == 1 && *view.selection.begin() != kOutputNode) Select({ItemKind::Node, {}, *view.selection.begin(), -1});
            else if (selected_.kind == ItemKind::Node) Select({});
        }
        return;
    }
    const anim::StateMachine* m = doc_.Get().FindMachine(tab);
    if (m == nullptr) return;
    bool blending = false;
    const std::string active = ActiveStateOf(tab, blending);
    const GraphViewModel model = BuildStateMachineView(*m, active, blending);
    const GraphViewResult r = DrawGraphView(("machine_" + tab).c_str(), model, view);
    if (HasEdits(r)) {
        const std::vector<std::string> errors = ApplyStateMachineEdits(doc_, tab, r);
        if (!errors.empty()) status_ = errors.front();
        ForgetMissing();
    }
    if (r.open_palette) OpenPalette(r.palette_x, r.palette_y);
    if (r.selection_changed || !r.deleted.empty()) {
        const u32 id = view.selection.size() == 1 ? *view.selection.begin() : 0;
        if (id >= kStateNodeBase && id < kStateNodeBase + m->states.size()) Select({ItemKind::State, tab, 0, static_cast<i32>(id - kStateNodeBase)});
        else if (selected_.kind == ItemKind::State) Select({});
    }
    // Clicking a wire selects its transition (Alt+click breaks it).
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().KeyAlt) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const int link = HitTestLink(model, view, mouse.x, mouse.y);
        if (link >= 0) {
            const GraphLinkView& l = model.links[static_cast<usize>(link)];
            const i32 from = l.from_node == kAnyStateNode ? anim::kAnyState : static_cast<i32>(l.from_node - kStateNodeBase);
            for (usize i = 0; i < m->transitions.size(); ++i) {
                if (l.from_node != kEntryNode && m->transitions[i].from == from && m->transitions[i].to == l.to_node - kStateNodeBase) {
                    Select({ItemKind::Transition, tab, 0, static_cast<i32>(i)});
                    break;
                }
            }
        }
    }
}

void AnimGraphEditor::Select(const Item& item) {
    if (item == selected_) return;
    selected_ = item;
    name_edit_.clear();
    if (item.kind == ItemKind::Variable) name_edit_ = item.name;
    if (item.kind == ItemKind::State) {
        if (const anim::StateMachine* m = doc_.Get().FindMachine(item.name); m != nullptr && static_cast<usize>(item.index) < m->states.size()) {
            name_edit_ = m->states[static_cast<usize>(item.index)].name;
        }
    }
}

void AnimGraphEditor::ForgetMissing() {
    const AnimGraph& g = doc_.Get();
    auto machine_ok = [&](const std::string& n, i32 index, bool transition) {
        const anim::StateMachine* m = g.FindMachine(n);
        return m != nullptr && index >= 0 && static_cast<usize>(index) < (transition ? m->transitions.size() : m->states.size());
    };
    switch (selected_.kind) {
    case ItemKind::Variable:
        if (g.FindVariable(selected_.name) == nullptr) Select({});
        break;
    case ItemKind::Node:
        if (g.Find(selected_.node) == nullptr) Select({});
        break;
    case ItemKind::State:
        if (!machine_ok(selected_.name, selected_.index, false)) Select({});
        break;
    case ItemKind::Transition:
        if (!machine_ok(selected_.name, selected_.index, true)) Select({});
        break;
    case ItemKind::None: break;
    }
    tabs_.erase(std::remove_if(tabs_.begin(), tabs_.end(), [&](const std::string& t) { return !t.empty() && g.FindMachine(t) == nullptr; }), tabs_.end());
    if (std::find(tabs_.begin(), tabs_.end(), active_) == tabs_.end()) active_.clear();
}

// --- Details -----------------------------------------------------------------------------------------

void AnimGraphEditor::DrawDetails() {
    switch (selected_.kind) {
    case ItemKind::None: ImGui::TextDisabled("Select a node, state, transition or variable."); break;
    case ItemKind::Variable: DrawVariableDetails(selected_.name); break;
    case ItemKind::Node: DrawNodeDetails(selected_.node); break;
    case ItemKind::State: DrawStateDetails(selected_.name, selected_.index); break;
    case ItemKind::Transition: DrawTransitionDetails(selected_.name, selected_.index); break;
    }
}

namespace {

// A combo over names, with a free-text fallback. True on change.
bool NameCombo(const char* label, std::string& value, const std::vector<std::string>& names) {
    bool changed = false;
    if (ImGui::BeginCombo(label, value.empty() ? "(none)" : value.c_str())) {
        for (const std::string& n : names) {
            if (ImGui::Selectable(n.c_str(), n == value)) value = n, changed = true;
        }
        ImGui::EndCombo();
    }
    return changed;
}

std::vector<std::string> VariablesOf(const AnimGraph& g, std::initializer_list<VarType> types) {
    std::vector<std::string> out;
    for (const anim::AnimVariable& v : g.variables) {
        if (std::find(types.begin(), types.end(), v.type) != types.end()) out.push_back(v.name);
    }
    return out;
}

} // namespace

void AnimGraphEditor::DrawVariableDetails(const std::string& name) {
    const anim::AnimVariable* v = doc_.Get().FindVariable(name);
    if (v == nullptr) return;
    ImGui::SeparatorText("Variable");
    if (ImGui::InputText("Name", &name_edit_, ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit()) {
        std::string error;
        if (name_edit_ != name) {
            if (doc_.RenameVariable(name, name_edit_, &error)) selected_.name = name_edit_;
            else status_ = error, name_edit_ = name;
        }
        return;
    }
    int type = static_cast<int>(v->type);
    const char* types[] = {"bool", "int", "float", "trigger"};
    if (ImGui::Combo("Type", &type, types, 4)) {
        doc_.SetVariableType(name, static_cast<VarType>(type));
        return;
    }
    if (v->type != VarType::Trigger) {
        f32 value = v->default_value;
        bool changed = false;
        if (v->type == VarType::Bool) {
            bool b = value != 0.0f;
            changed = ImGui::Checkbox("Default", &b);
            value = b ? 1.0f : 0.0f;
        } else if (v->type == VarType::Int) {
            int i = static_cast<int>(value);
            changed = ImGui::DragInt("Default", &i);
            value = static_cast<f32>(i);
        } else {
            changed = ImGui::DragFloat("Default", &value, 0.01f);
        }
        if (changed) {
            doc_.Edit("Edit default", [&](AnimGraph& g) {
                for (anim::AnimVariable& x : g.variables) {
                    if (x.name == name) x.default_value = value;
                }
            }, "var_default:" + name);
        }
    }
    if (ImGui::Button("Delete")) {
        doc_.RemoveVariable(name);
        ForgetMissing();
    }
}

void AnimGraphEditor::DrawNodeDetails(u32 id) {
    const AnimNode* found = doc_.Get().Find(id);
    if (found == nullptr) return;
    const AnimNode n = *found; // a copy: edits replace the graph
    ImGui::SeparatorText(PoseNodeTitle(n).c_str());
    const std::string key = "node:" + std::to_string(id) + ":";
    auto edit = [&](const char* label, const std::function<void(AnimNode&)>& change, bool merge) {
        doc_.Edit(label, [&](AnimGraph& g) {
            for (AnimNode& x : g.nodes) {
                if (x.id == id) change(x);
            }
        }, merge ? key + label : std::string());
    };
    const AnimGraph& g = doc_.Get();
    std::string text;
    switch (n.kind) {
    case AnimNodeKind::Clip: {
        text = n.asset;
        if (NameCombo("Clip", text, doc_.ClipNames())) edit("Clip", [&](AnimNode& x) { x.asset = text; }, false);
        bool loop = n.loop;
        if (ImGui::Checkbox("Loop", &loop)) edit("Loop", [&](AnimNode& x) { x.loop = loop; }, false);
        f32 rate = n.rate;
        if (ImGui::DragFloat("Rate", &rate, 0.01f, 0.0f, 10.0f)) edit("Rate", [&](AnimNode& x) { x.rate = rate; }, true);
        break;
    }
    case AnimNodeKind::BlendSpace: {
        text = n.asset;
        if (NameCombo("Blend Space", text, doc_.BlendSpaceNames())) edit("Blend Space", [&](AnimNode& x) { x.asset = text; }, false);
        text = n.variable;
        if (NameCombo("X", text, VariablesOf(g, {VarType::Float, VarType::Int}))) edit("X", [&](AnimNode& x) { x.variable = text; }, false);
        text = n.variable_y;
        if (NameCombo("Y", text, VariablesOf(g, {VarType::Float, VarType::Int}))) edit("Y", [&](AnimNode& x) { x.variable_y = text; }, false);
        break;
    }
    case AnimNodeKind::Blend:
    case AnimNodeKind::BlendByBool:
    case AnimNodeKind::BlendByInt: {
        const std::vector<std::string> vars = n.kind == AnimNodeKind::Blend        ? VariablesOf(g, {VarType::Float})
                                              : n.kind == AnimNodeKind::BlendByBool ? VariablesOf(g, {VarType::Bool})
                                                                                    : VariablesOf(g, {VarType::Int});
        text = n.variable;
        if (NameCombo("Variable", text, vars)) edit("Variable", [&](AnimNode& x) { x.variable = text; }, false);
        if (n.kind != AnimNodeKind::Blend) {
            f32 blend = n.blend_time;
            if (ImGui::DragFloat("Blend Time", &blend, 0.01f, 0.0f, 5.0f)) edit("Blend Time", [&](AnimNode& x) { x.blend_time = blend; }, true);
        }
        if (n.kind == AnimNodeKind::BlendByInt) {
            if (ImGui::SmallButton("+ Input")) edit("Add input", [](AnimNode& x) { x.inputs.push_back(0); }, false);
            ImGui::SameLine();
            ImGui::BeginDisabled(n.inputs.size() <= 2);
            if (ImGui::SmallButton("- Input")) edit("Remove input", [](AnimNode& x) { x.inputs.pop_back(); }, false);
            ImGui::EndDisabled();
        }
        break;
    }
    case AnimNodeKind::Layered: {
        text = n.bone;
        if (ImGui::InputText("From Bone", &text)) edit("Bone", [&](AnimNode& x) { x.bone = text; }, true);
        int depth = static_cast<int>(n.depth);
        if (ImGui::SliderInt("Blend Depth", &depth, 0, 8)) edit("Depth", [&](AnimNode& x) { x.depth = static_cast<u32>(depth); }, true);
        f32 weight = n.weight;
        if (ImGui::SliderFloat("Weight", &weight, 0.0f, 1.0f)) edit("Weight", [&](AnimNode& x) { x.weight = weight; }, true);
        break;
    }
    case AnimNodeKind::Additive: {
        text = n.variable;
        if (NameCombo("Weight Variable", text, VariablesOf(g, {VarType::Float}))) edit("Variable", [&](AnimNode& x) { x.variable = text; }, false);
        f32 weight = n.weight;
        if (n.variable.empty() && ImGui::SliderFloat("Weight", &weight, 0.0f, 1.0f)) edit("Weight", [&](AnimNode& x) { x.weight = weight; }, true);
        break;
    }
    case AnimNodeKind::StateMachine: {
        std::vector<std::string> machines;
        for (const anim::StateMachine& m : g.machines) machines.push_back(m.name);
        text = n.machine;
        if (NameCombo("Machine", text, machines)) edit("Machine", [&](AnimNode& x) { x.machine = text; }, false);
        if (!n.machine.empty() && ImGui::Button("Open")) OpenTab(n.machine);
        break;
    }
    case AnimNodeKind::Slot: {
        text = n.slot;
        if (ImGui::InputText("Slot", &text)) edit("Slot", [&](AnimNode& x) { x.slot = text; }, true);
        break;
    }
    }
    ImGui::Separator();
    ImGui::BeginDisabled(g.output == id);
    if (ImGui::Button("Use as Output")) doc_.SetOutput(id);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
        doc_.DeleteNodes({id});
        ForgetMissing();
    }
}

void AnimGraphEditor::DrawStateDetails(const std::string& machine, i32 index) {
    const anim::StateMachine* m = doc_.Get().FindMachine(machine);
    if (m == nullptr || index < 0 || static_cast<usize>(index) >= m->states.size()) return;
    const anim::AnimState s = m->states[static_cast<usize>(index)];
    ImGui::SeparatorText(s.conduit ? "Conduit" : "State");
    if (ImGui::InputText("Name", &name_edit_, ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit()) {
        std::string error;
        if (name_edit_ != s.name && !doc_.RenameState(machine, s.name, name_edit_, &error)) status_ = error, name_edit_ = s.name;
        return;
    }
    if (!s.conduit) {
        std::vector<std::string> nodes;
        std::string current = "(none)";
        for (const AnimNode& n : doc_.Get().nodes) {
            nodes.push_back(std::to_string(n.id) + ": " + PoseNodeTitle(n));
            if (n.id == s.pose) current = nodes.back();
        }
        std::string chosen = current;
        if (NameCombo("Pose", chosen, nodes) && chosen != current) {
            const u32 pose = static_cast<u32>(std::stoul(chosen.substr(0, chosen.find(':'))));
            doc_.Edit("Set state pose", [&](AnimGraph& g) {
                for (anim::StateMachine& sm : g.machines) {
                    if (sm.name == machine) sm.states[static_cast<usize>(index)].pose = pose;
                }
            });
        }
    }
    ImGui::BeginDisabled(m->entry == static_cast<u32>(index) || s.conduit);
    if (ImGui::Button("Set as Entry")) doc_.SetEntry(machine, s.name);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
        doc_.RemoveState(machine, s.name);
        ForgetMissing();
    }
}

void AnimGraphEditor::DrawTransitionDetails(const std::string& machine, i32 index) {
    const anim::StateMachine* m = doc_.Get().FindMachine(machine);
    if (m == nullptr || index < 0 || static_cast<usize>(index) >= m->transitions.size()) return;
    const anim::Transition t = m->transitions[static_cast<usize>(index)];
    const std::string from = t.from == anim::kAnyState ? "Any State" : m->states[static_cast<usize>(t.from)].name;
    ImGui::SeparatorText(("Transition " + from + " -> " + m->states[t.to].name).c_str());
    const std::string key = "transition:" + machine + ":" + std::to_string(index) + ":";
    auto edit = [&](const char* label, const std::function<void(anim::Transition&)>& change, bool merge) {
        doc_.Edit(label, [&](AnimGraph& g) {
            for (anim::StateMachine& sm : g.machines) {
                if (sm.name == machine) change(sm.transitions[static_cast<usize>(index)]);
            }
        }, merge ? key + label : std::string());
    };
    f32 blend = t.blend_time;
    if (ImGui::DragFloat("Blend Time", &blend, 0.01f, 0.0f, 5.0f)) edit("Blend Time", [&](anim::Transition& x) { x.blend_time = blend; }, true);
    int priority = t.priority;
    if (ImGui::DragInt("Priority", &priority)) edit("Priority", [&](anim::Transition& x) { x.priority = priority; }, true);
    bool finished = t.when_finished;
    if (ImGui::Checkbox("When Animation Finishes", &finished)) edit("When finished", [&](anim::Transition& x) { x.when_finished = finished; }, false);
    ImGui::SeparatorText("Conditions (all must hold)");
    std::vector<std::string> vars;
    for (const anim::AnimVariable& v : doc_.Get().variables) vars.push_back(v.name);
    int remove = -1;
    for (usize c = 0; c < t.conditions.size(); ++c) {
        ImGui::PushID(static_cast<int>(c));
        const anim::Condition& cond = t.conditions[c];
        std::string var = cond.variable;
        ImGui::SetNextItemWidth(100);
        if (NameCombo("##var", var, vars)) edit("Condition", [&](anim::Transition& x) { x.conditions[c].variable = var; }, false);
        ImGui::SameLine();
        int op = static_cast<int>(cond.op);
        ImGui::SetNextItemWidth(80);
        if (ImGui::Combo("##op", &op, kOpLabels, 9)) edit("Condition", [&](anim::Transition& x) { x.conditions[c].op = static_cast<CompareOp>(op); }, false);
        if (cond.op != CompareOp::IsTrue && cond.op != CompareOp::IsFalse && cond.op != CompareOp::Triggered) {
            ImGui::SameLine();
            f32 value = cond.value;
            ImGui::SetNextItemWidth(60);
            if (ImGui::DragFloat("##value", &value, 0.01f)) edit("Condition value", [&](anim::Transition& x) { x.conditions[c].value = value; }, true);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) remove = static_cast<int>(c);
        ImGui::PopID();
    }
    if (remove >= 0) edit("Remove condition", [&](anim::Transition& x) { x.conditions.erase(x.conditions.begin() + remove); }, false);
    if (ImGui::SmallButton("+ Condition")) {
        edit("Add condition", [&](anim::Transition& x) { x.conditions.push_back({vars.empty() ? std::string() : vars.front(), CompareOp::IsTrue, 0.0f}); }, false);
    }
    if (ImGui::Button("Delete Transition")) {
        doc_.RemoveTransition(machine, static_cast<usize>(index));
        ForgetMissing();
    }
}

void AnimGraphEditor::DrawDiagnostics() {
    const auto& d = doc_.Diagnostics();
    ImGui::TextDisabled("Diagnostics (%zu)", d.size());
    for (usize i = 0; i < d.size(); ++i) {
        const ImVec4 color = d[i].error ? ImVec4(0.9f, 0.35f, 0.35f, 1) : ImVec4(0.9f, 0.75f, 0.2f, 1);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        if (ImGui::Selectable((d[i].code + "  " + d[i].message + "##diag" + std::to_string(i)).c_str())) {
            if (!d[i].machine.empty()) {
                OpenTab(d[i].machine);
            } else if (d[i].node != 0) {
                OpenTab("");
                GraphViewState& v = views_[""];
                v.selection = {d[i].node};
                v.request_fit = v.request_fit_selection = true;
                Select({ItemKind::Node, {}, d[i].node, -1});
            }
        }
        ImGui::PopStyleColor();
    }
}

// --- Palette ----------------------------------------------------------------------------------------

void AnimGraphEditor::OpenPalette(f32 x, f32 y) { palette_ = {true, true, active_, x, y}; }

u32 AnimGraphEditor::PlaceNode(AnimNodeKind kind) {
    const f32 x = palette_.open ? palette_.x : 0.0f, y = palette_.open ? palette_.y : 0.0f;
    palette_ = {};
    u32 id = 0;
    if (kind == AnimNodeKind::StateMachine) {
        doc_.AddMachine(x, y, &id);
    } else {
        id = doc_.AddNode(kind, x, y);
    }
    views_[""].selection = {id};
    Select({ItemKind::Node, {}, id, -1});
    return id;
}

i32 AnimGraphEditor::PlaceState(bool conduit) {
    const std::string machine = palette_.open ? palette_.tab : active_;
    const f32 x = palette_.open ? palette_.x : 0.0f, y = palette_.open ? palette_.y : 0.0f;
    palette_ = {};
    const i32 index = doc_.AddState(machine, x, y, conduit);
    if (index >= 0) {
        views_[machine].selection = {kStateNodeBase + static_cast<u32>(index)};
        Select({ItemKind::State, machine, 0, index});
    }
    return index;
}

void AnimGraphEditor::DrawPalette() {
    if (!palette_.open) return;
    if (palette_.needs_popup) {
        const GraphViewState& v = views_[palette_.tab];
        ImGui::SetNextWindowPos(ImVec2(v.CanvasToScreenX(palette_.x), v.CanvasToScreenY(palette_.y)), ImGuiCond_Always);
        ImGui::OpenPopup("##anim_palette");
        palette_.needs_popup = false;
    }
    if (!ImGui::BeginPopup("##anim_palette")) {
        palette_ = {};
        return;
    }
    if (IsMachineTab(palette_.tab)) {
        if (ImGui::Selectable("State")) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            PlaceState(false);
            return;
        }
        if (ImGui::Selectable("Conduit")) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            PlaceState(true);
            return;
        }
    } else {
        for (AnimNodeKind k : kKinds) {
            AnimNode probe;
            probe.kind = k;
            if (ImGui::Selectable(PoseNodeTitle(probe).c_str())) {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                PlaceNode(k);
                return;
            }
        }
    }
    ImGui::EndPopup();
}

bool AnimGraphEditor::SaveNow() {
    std::string error;
    if (!doc_.Save(&error)) {
        status_ = "Save failed: " + error;
        return false;
    }
    status_ = "Saved " + doc_.Path().filename().string();
    return true;
}

} // namespace aether::editor
