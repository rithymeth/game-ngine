#include "ai/bt_editor.h"

#include "core/json_edit.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cstdio>

namespace aether::editor {

using nlohmann::json;
using namespace aether::ai;

namespace {

constexpr u32 kCompositeColor = 0xFF805030; // ABGR
constexpr u32 kTaskColor = 0xFF703070;
constexpr u32 kSuccessColor = 0xFF30A030;
constexpr u32 kFailureColor = 0xFF3030C0;
constexpr u32 kWireColor = 0xFFC0C0C0;
constexpr u32 kActiveWireColor = 0xFF40E0FF;

const char* const kDecoratorTypes[] = {"BlackboardCondition", "Cooldown", "Loop", "TimeLimit", "Inverter", "ForceSuccess", "ForceFailure"};
const char* const kServiceTypes[] = {"Blueprint", "Luau", "DistanceTo"};
const char* const kKeyTypes[] = {"Bool", "Int", "Float", "String", "Vector", "Entity"};

std::string Fmt(f32 v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
    return buf;
}

void Layout(const BtNode& n, BtPath& path, i32 parent, i32 depth, f32& next_slot, std::vector<BtLayoutNode>& out) {
    const usize me = out.size();
    out.push_back({path, &n, static_cast<u32>(me + 1), 0.0f, static_cast<f32>(depth) * kBtRowHeight, parent});
    if (n.children.empty()) {
        out[me].x = next_slot * kBtSlotWidth;
        next_slot += 1.0f;
        return;
    }
    f32 first = 0.0f, last = 0.0f;
    for (usize i = 0; i < n.children.size(); ++i) {
        path.push_back(i);
        const usize child = out.size();
        Layout(n.children[i], path, static_cast<i32>(me), depth + 1, next_slot, out);
        path.pop_back();
        if (i == 0) first = out[child].x;
        last = out[child].x;
    }
    out[me].x = (first + last) * 0.5f;
}

const BtLayoutNode* ById(const std::vector<BtLayoutNode>& layout, u32 id) { return id >= 1 && id <= layout.size() ? &layout[id - 1] : nullptr; }

bool Within(const BtPath& path, const BtPath& ancestor) {
    return path.size() >= ancestor.size() && std::equal(ancestor.begin(), ancestor.end(), path.begin());
}

} // namespace

std::vector<BtLayoutNode> LayoutBehaviorTree(const BehaviorTreeAsset& tree) {
    std::vector<BtLayoutNode> out;
    BtPath path;
    f32 slot = 0.0f;
    Layout(tree.root, path, -1, 0, slot, out);
    return out;
}

std::string BtNodeTitle(const BtNode& n) {
    std::string t = n.name.empty() ? std::string(BtNodeTypeName(n.type)) : n.name + " (" + BtNodeTypeName(n.type) + ")";
    switch (n.type) {
    case BtNodeType::Parallel: return t + (n.succeed_on_one ? ": any" : ": all");
    case BtNodeType::Wait: return t + " " + Fmt(n.seconds) + "s" + (n.deviation > 0.0f ? " +-" + Fmt(n.deviation) : "");
    case BtNodeType::MoveTo: return t + " " + n.key;
    case BtNodeType::SetBlackboard: return t + " " + n.key + " = " + ValueToString(n.value);
    case BtNodeType::ClearBlackboard: return t + " " + n.key;
    case BtNodeType::RunBlueprint:
    case BtNodeType::RunLuau:
    case BtNodeType::Log: return t + " " + n.event;
    default: return t;
    }
}

std::string BtNodeNotes(const BtNode& n) {
    static const char* const kOps[] = {"is set", "is not set", "==", "!=", "<", "<=", ">", ">="};
    static const char* const kAborts[] = {"", ", aborts self", ", aborts lower", ", aborts both"};
    std::string out;
    const auto line = [&](const std::string& s) { out += (out.empty() ? "" : "\n") + s; };
    for (const BtDecorator& d : n.decorators) {
        switch (d.type) {
        case BtDecoratorType::BlackboardCondition: {
            std::string s = "if " + d.key + " " + kOps[static_cast<usize>(d.op)];
            if (d.op != BtCompare::IsSet && d.op != BtCompare::IsNotSet) s += " " + ValueToString(d.value);
            line(s + kAborts[static_cast<usize>(d.abort)]);
            break;
        }
        case BtDecoratorType::Cooldown: line("cooldown " + Fmt(d.seconds) + "s"); break;
        case BtDecoratorType::Loop: line(d.count == 0 ? std::string("loop forever") : "loop " + std::to_string(d.count) + "x"); break;
        case BtDecoratorType::TimeLimit: line("time limit " + Fmt(d.seconds) + "s"); break;
        case BtDecoratorType::Inverter: line("invert"); break;
        case BtDecoratorType::ForceSuccess: line("force success"); break;
        case BtDecoratorType::ForceFailure: line("force failure"); break;
        }
    }
    for (const BtService& s : n.services) {
        const std::string every = " every " + Fmt(s.interval) + "s";
        switch (s.type) {
        case BtServiceType::Blueprint: line("service " + s.event + every); break;
        case BtServiceType::Luau: line("service " + s.event + "() " + every); break;
        case BtServiceType::DistanceTo: line("service " + s.out_key + " = distance to " + s.key + every); break;
        }
    }
    return out;
}

GraphViewModel BuildBehaviorTreeView(const BehaviorTreeAsset& tree, const std::vector<BtLayoutNode>& layout, const std::vector<BtProblem>& problems,
                                     const BehaviorTreeInstance* live) {
    GraphViewModel m;
    const bool can_debug = live != nullptr && live->Nodes().size() == layout.size();
    for (usize i = 0; i < layout.size(); ++i) {
        const BtLayoutNode& l = layout[i];
        const BtNode& n = *l.node;
        GraphNodeView v;
        v.id = l.id;
        v.title = BtNodeTitle(n);
        v.x = l.x;
        v.y = l.y;
        v.header_color = IsComposite(n.type) ? kCompositeColor : kTaskColor;
        if (l.parent >= 0) v.inputs.push_back({"in", "", true, false, kWireColor, true});
        if (IsComposite(n.type)) v.outputs.push_back({"out", "", true, false, kWireColor, !n.children.empty()});
        v.comment = BtNodeNotes(n);
        const std::string where = BtProblemPath(tree, l.path);
        for (const BtProblem& p : problems) {
            if (p.path == where && !p.warning) {
                v.error = p.code + ": " + p.message;
                break;
            }
        }
        if (can_debug) {
            v.highlighted = live->IsActive(i);
            if (!v.highlighted && live->LastStatus(i) != BtStatus::Running) v.header_color = live->LastStatus(i) == BtStatus::Success ? kSuccessColor : kFailureColor;
        }
        m.nodes.push_back(std::move(v));
        if (l.parent >= 0) {
            GraphLinkView link{layout[static_cast<usize>(l.parent)].id, "out", l.id, "in", kWireColor, 0.0f};
            if (can_debug && live->IsActive(i)) link.color = kActiveWireColor, link.glow = 1.0f;
            m.links.push_back(link);
        }
    }
    return m;
}

std::vector<std::string> ApplyBehaviorTreeEdits(BehaviorTreeDocument& doc, const std::vector<BtLayoutNode>& layout, const GraphViewResult& e) {
    std::vector<std::string> messages;
    std::string error;
    // A wire from a composite's out to a node's in: the node moves under it (at the end).
    if (e.connect) {
        const BtLayoutNode* from = ById(layout, e.connect_from.node);
        const BtLayoutNode* to = ById(layout, e.connect_to.node);
        if (from != nullptr && to != nullptr) {
            const BtNode* parent = doc.Node(from->path);
            if (!doc.MoveNode(to->path, from->path, parent != nullptr ? parent->children.size() : 0, &error)) messages.push_back(error);
        }
        return messages; // the layout is stale now
    }
    if (e.disconnect) messages.push_back("every node needs a parent: wire it to another composite, or delete it");
    // A sideways drag reorders the node among its siblings.
    if (!e.moved.empty()) {
        const auto& [id, pos] = e.moved.front();
        const BtLayoutNode* n = ById(layout, id);
        if (n != nullptr && n->parent >= 0 && e.moved.size() == 1) {
            usize index = 0;
            for (const BtLayoutNode& s : layout)
                if (s.parent == n->parent && s.id != n->id && s.x < pos.first) ++index;
            const BtPath parent(n->path.begin(), n->path.end() - 1);
            if (!doc.MoveNode(n->path, parent, index, &error)) messages.push_back(error);
            return messages;
        }
    }
    if (!e.deleted.empty()) {
        std::vector<BtPath> paths;
        for (u32 id : e.deleted) {
            const BtLayoutNode* n = ById(layout, id);
            if (n == nullptr) continue;
            if (n->path.empty()) {
                messages.push_back("the root can't be removed");
                continue;
            }
            paths.push_back(n->path);
        }
        // Deepest and last first, and nothing under something already going.
        std::sort(paths.begin(), paths.end(), std::greater<>());
        for (const BtPath& p : paths) {
            if (std::any_of(paths.begin(), paths.end(), [&](const BtPath& q) { return q != p && Within(p, q); })) continue;
            if (!doc.RemoveNode(p, &error)) messages.push_back(error);
        }
        return messages;
    }
    return messages;
}

BehaviorTreeEditor::BehaviorTreeEditor(BehaviorTreeDocument& document) : doc_(document) { view_.request_fit = true; }

const std::vector<BtLayoutNode>& BehaviorTreeEditor::Layout() {
    if (layout_revision_ != doc_.Revision()) {
        layout_ = LayoutBehaviorTree(doc_.Get());
        layout_revision_ = doc_.Revision();
        if (selected_ && doc_.Node(*selected_) == nullptr) ClearSelection();
    }
    return layout_;
}

void BehaviorTreeEditor::Select(const BtPath& path) {
    if (doc_.Node(path) == nullptr) return;
    selected_ = path;
    view_.selection.clear();
    for (const BtLayoutNode& l : Layout())
        if (l.path == path) view_.selection.insert(l.id);
}

void BehaviorTreeEditor::ClearSelection() {
    selected_.reset();
    view_.selection.clear();
}

void BehaviorTreeEditor::SetLiveInstance(const BehaviorTreeInstance* instance, const Blackboard* blackboard) {
    live_ = instance;
    live_board_ = blackboard;
}

bool BehaviorTreeEditor::SaveNow() {
    std::string error;
    if (!doc_.Save(&error)) {
        status_ = error;
        return false;
    }
    status_ = "Saved " + doc_.Path().filename().string();
    return true;
}

void BehaviorTreeEditor::OpenPalette(const BtPath& parent) {
    const BtNode* p = doc_.Node(parent);
    if (p == nullptr || !IsComposite(p->type)) {
        status_ = "new nodes go under a composite (Selector, Sequence or Parallel)";
        return;
    }
    palette_parent_ = parent;
    palette_open_ = true;
    palette_needs_popup_ = true;
}

std::optional<BtPath> BehaviorTreeEditor::PlaceNode(BtNodeType type) {
    if (!palette_open_) return std::nullopt;
    palette_open_ = false;
    std::string error;
    const auto path = doc_.AddNode(palette_parent_, type, std::nullopt, &error);
    if (!path) {
        status_ = error;
        return std::nullopt;
    }
    Select(*path);
    return path;
}

const std::vector<const char*>& BehaviorTreeEditor::KeyNames() {
    key_names_.clear();
    for (const BlackboardKey& k : doc_.Get().blackboard.keys) key_names_.push_back(k.name);
    key_ptrs_.clear();
    for (const std::string& s : key_names_) key_ptrs_.push_back(s.c_str());
    return key_ptrs_;
}

void BehaviorTreeEditor::Draw() {
    Layout();
    DrawToolbar();
    const f32 total_h = ImGui::GetContentRegionAvail().y;
    const f32 top = std::max(100.0f, total_h - 170.0f);
    ImGui::BeginChild("##bt_blackboard", ImVec2(240, top), ImGuiChildFlags_Borders);
    DrawBlackboard();
    ImGui::EndChild();
    ImGui::SameLine();
    const f32 center = std::max(200.0f, ImGui::GetContentRegionAvail().x - 320.0f);
    ImGui::BeginChild("##bt_tree", ImVec2(center, top), ImGuiChildFlags_None);
    DrawTree();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##bt_details", ImVec2(0, top), ImGuiChildFlags_Borders);
    DrawDetails();
    ImGui::EndChild();
    ImGui::BeginChild("##bt_bottom", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (live_ != nullptr) {
        ImGui::BeginChild("##bt_diag", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0));
        DrawDiagnostics();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##bt_debug", ImVec2(0, 0));
        DrawDebugger();
        ImGui::EndChild();
    } else {
        DrawDiagnostics();
    }
    ImGui::EndChild();
    DrawPalette();
    HandleKeys();
}

void BehaviorTreeEditor::DrawToolbar() {
    if (ImGui::Button("Save")) SaveNow();
    ImGui::SameLine();
    ImGui::BeginDisabled(!doc_.CanUndo());
    if (ImGui::Button("Undo")) doc_.Undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!doc_.CanRedo());
    if (ImGui::Button("Redo")) doc_.Redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Fit")) view_.request_fit = true;
    ImGui::SameLine();
    ImGui::Text("%s%s", doc_.Name().c_str(), doc_.Dirty() ? " *" : "");
    if (live_ != nullptr) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "  debugging: %s", live_->ActiveLabel().c_str());
    }
    if (!status_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("  %s", status_.c_str());
    }
}

void BehaviorTreeEditor::DrawBlackboard() {
    ImGui::TextUnformatted("Blackboard");
    ImGui::Separator();
    const auto keys = doc_.Get().blackboard.keys; // a copy: edits below change the document
    for (const BlackboardKey& k : keys) {
        ImGui::PushID(k.name.c_str());
        std::string name = k.name;
        ImGui::SetNextItemWidth(110);
        if (ImGui::InputText("##name", &name, ImGuiInputTextFlags_EnterReturnsTrue)) {
            std::string error;
            if (!doc_.RenameKey(k.name, name, &error)) status_ = error;
        }
        ImGui::SameLine();
        int type = static_cast<int>(k.type);
        ImGui::SetNextItemWidth(70);
        if (ImGui::Combo("##type", &type, kKeyTypes, IM_ARRAYSIZE(kKeyTypes))) doc_.SetKeyType(k.name, static_cast<BlackboardType>(type));
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) doc_.RemoveKey(k.name);
        if (k.type != BlackboardType::Entity) {
            json initial = ValueToJson(k.initial);
            if (initial.is_null()) {
                if (ImGui::SmallButton("Set initial")) {
                    json def = k.type == BlackboardType::Bool     ? json(false)
                               : k.type == BlackboardType::Int    ? json(0)
                               : k.type == BlackboardType::Float  ? json(0.0)
                               : k.type == BlackboardType::String ? json("")
                                                                  : json::array({0, 0, 0});
                    std::string error;
                    if (!doc_.SetKeyInitial(k.name, def, &error)) status_ = error;
                }
            } else {
                bool dragging = false;
                if (EditJsonField("initial", initial, dragging)) {
                    std::string error;
                    if (!doc_.SetKeyInitial(k.name, initial, &error)) status_ = error;
                }
            }
        }
        const usize users = doc_.KeyUsers(k.name).size();
        ImGui::TextDisabled("used by %zu node%s", users, users == 1 ? "" : "s");
        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::SetNextItemWidth(110);
    ImGui::InputText("##new_key", &new_key_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    ImGui::Combo("##new_type", &new_key_type_, kKeyTypes, IM_ARRAYSIZE(kKeyTypes));
    if (ImGui::Button("Add key")) {
        std::string error;
        if (!doc_.AddKey(doc_.UniqueKeyName(new_key_), static_cast<BlackboardType>(new_key_type_), &error)) status_ = error;
    }
}

void BehaviorTreeEditor::DrawTree() {
    const GraphViewModel model = BuildBehaviorTreeView(doc_.Get(), Layout(), doc_.Diagnostics(), live_);
    const GraphViewResult r = DrawGraphView("##bt_graph", model, view_);
    if (r.selection_changed) {
        if (view_.selection.size() == 1) {
            if (const BtLayoutNode* n = ById(layout_, *view_.selection.begin())) selected_ = n->path;
        } else {
            selected_.reset();
        }
    }
    if (r.open_palette) {
        BtPath parent;
        if (r.palette_from_pin && r.palette_from.output) {
            if (const BtLayoutNode* n = ById(layout_, r.palette_from.node)) parent = n->path;
        } else if (selected_ && doc_.Node(*selected_) != nullptr && IsComposite(doc_.Node(*selected_)->type)) {
            parent = *selected_;
        }
        OpenPalette(parent);
    }
    if (r.duplicate && selected_) {
        std::string error;
        if (const auto p = doc_.DuplicateNode(*selected_, &error)) Select(*p);
        else status_ = error;
    }
    const std::vector<BtLayoutNode> layout = layout_; // edits refresh the layout
    for (const std::string& m : ApplyBehaviorTreeEdits(doc_, layout, r)) status_ = m;
}

void BehaviorTreeEditor::DrawPalette() {
    if (!palette_open_) return;
    if (palette_needs_popup_) {
        ImGui::OpenPopup("##bt_palette");
        palette_needs_popup_ = false;
    }
    if (!ImGui::BeginPopup("##bt_palette")) {
        palette_open_ = false;
        return;
    }
    ImGui::TextDisabled("Composites");
    std::optional<BtNodeType> chosen;
    for (BtNodeType t : BtCompositeTypes())
        if (ImGui::Selectable(BtNodeTypeName(t))) chosen = t;
    ImGui::Separator();
    ImGui::TextDisabled("Tasks");
    for (BtNodeType t : BtTaskTypes())
        if (ImGui::Selectable(BtNodeTypeName(t))) chosen = t;
    if (chosen) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    if (chosen) PlaceNode(*chosen);
}

void BehaviorTreeEditor::DrawDetails() {
    if (!selected_ || doc_.Node(*selected_) == nullptr) {
        ImGui::TextDisabled("Select a node.");
        if (ImGui::Button("Add under the root")) OpenPalette({});
        return;
    }
    const BtPath path = *selected_;
    const BtNode& n = *doc_.Node(path);
    ImGui::Text("%s", BtNodeTypeName(n.type));
    ImGui::TextDisabled("%s", BtProblemPath(doc_.Get(), path).c_str());
    if (!ImGui::IsAnyItemActive()) name_edit_ = n.name;
    if (ImGui::InputText("Name", &name_edit_, ImGuiInputTextFlags_EnterReturnsTrue)) {
        std::string error;
        if (!doc_.SetNodeField(path, "name", name_edit_, &error)) status_ = error;
    }
    // The type (composites stay composites while they have children).
    if (ImGui::BeginCombo("Type", BtNodeTypeName(n.type))) {
        std::vector<BtNodeType> types = BtCompositeTypes();
        if (n.children.empty())
            for (BtNodeType t : BtTaskTypes()) types.push_back(t);
        for (BtNodeType t : types) {
            if (ImGui::Selectable(BtNodeTypeName(t), t == n.type)) {
                std::string error;
                if (!doc_.SetNodeType(path, t, &error)) status_ = error;
            }
        }
        ImGui::EndCombo();
    }
    JsonEnums enums;
    enums["key"] = KeyNames();
    const json fields = doc_.NodeJson(path);
    for (const auto& [key, value] : fields.items()) {
        if (key == "type" || key == "name" || key == "decorators" || key == "services") continue;
        ImGui::PushID(key.c_str());
        json out = value;
        bool dragging = false;
        if (EditJsonField(key, out, dragging, &enums)) {
            std::string error;
            if (!doc_.SetNodeField(path, key, out, &error, dragging ? "node:" + key : std::string())) status_ = error;
        }
        ImGui::PopID();
    }
    if (IsComposite(n.type) && ImGui::Button("Add child")) OpenPalette(path);
    if (!path.empty()) {
        if (IsComposite(n.type)) ImGui::SameLine();
        if (ImGui::Button("Duplicate")) {
            if (const auto p = doc_.DuplicateNode(path)) Select(*p);
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete")) {
            doc_.RemoveNode(path);
            ClearSelection();
            return;
        }
    }
    DrawDecorators(path);
    DrawServices(path);
}

void BehaviorTreeEditor::DrawDecorators(const BtPath& path) {
    if (!ImGui::CollapsingHeader("Decorators", ImGuiTreeNodeFlags_DefaultOpen)) return;
    JsonEnums enums;
    enums["key"] = KeyNames();
    enums["type"] = std::vector<const char*>(std::begin(kDecoratorTypes), std::end(kDecoratorTypes));
    enums["op"] = {"IsSet", "IsNotSet", "Equal", "NotEqual", "Less", "LessEqual", "Greater", "GreaterEqual"};
    enums["abort"] = {"None", "Self", "LowerPriority", "Both"};
    const usize count = doc_.Node(path)->decorators.size();
    for (usize i = 0; i < count; ++i) {
        ImGui::PushID(static_cast<int>(i));
        const json d = doc_.DecoratorJson(path, i);
        ImGui::Text("%zu. %s", i + 1, d.value("type", std::string()).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("up") && i > 0) doc_.MoveDecorator(path, i, i - 1);
        ImGui::SameLine();
        if (ImGui::SmallButton("down") && i + 1 < count) doc_.MoveDecorator(path, i, i + 1);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            doc_.RemoveDecorator(path, i);
            ImGui::PopID();
            break;
        }
        for (const auto& [key, value] : d.items()) {
            ImGui::PushID(key.c_str());
            json out = value;
            bool dragging = false;
            if (EditJsonField(key, out, dragging, &enums)) {
                std::string error;
                if (!doc_.SetDecoratorField(path, i, key, out, &error, dragging ? "decorator:" + key : std::string())) status_ = error;
            }
            ImGui::PopID();
        }
        if (d.value("type", std::string()) == "BlackboardCondition" && !d.contains("value") && ImGui::SmallButton("Compare with a value")) {
            std::string error;
            if (!doc_.SetDecoratorField(path, i, "value", 0, &error)) status_ = error;
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    static int add = 0;
    ImGui::SetNextItemWidth(150);
    ImGui::Combo("##add_decorator", &add, kDecoratorTypes, IM_ARRAYSIZE(kDecoratorTypes));
    ImGui::SameLine();
    if (ImGui::Button("Add decorator")) doc_.AddDecorator(path, static_cast<BtDecoratorType>(add));
}

void BehaviorTreeEditor::DrawServices(const BtPath& path) {
    if (!ImGui::CollapsingHeader("Services", ImGuiTreeNodeFlags_DefaultOpen)) return;
    JsonEnums enums;
    enums["key"] = KeyNames();
    enums["out_key"] = KeyNames();
    enums["type"] = std::vector<const char*>(std::begin(kServiceTypes), std::end(kServiceTypes));
    const usize count = doc_.Node(path)->services.size();
    for (usize i = 0; i < count; ++i) {
        ImGui::PushID(static_cast<int>(i + 1000));
        const json s = doc_.ServiceJson(path, i);
        ImGui::Text("%zu. %s", i + 1, s.value("type", std::string()).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            doc_.RemoveService(path, i);
            ImGui::PopID();
            break;
        }
        for (const auto& [key, value] : s.items()) {
            ImGui::PushID(key.c_str());
            json out = value;
            bool dragging = false;
            if (EditJsonField(key, out, dragging, &enums)) {
                std::string error;
                if (!doc_.SetServiceField(path, i, key, out, &error, dragging ? "service:" + key : std::string())) status_ = error;
            }
            ImGui::PopID();
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    static int add = 0;
    ImGui::SetNextItemWidth(150);
    ImGui::Combo("##add_service", &add, kServiceTypes, IM_ARRAYSIZE(kServiceTypes));
    ImGui::SameLine();
    if (ImGui::Button("Add service")) doc_.AddService(path, static_cast<BtServiceType>(add));
}

void BehaviorTreeEditor::DrawDiagnostics() {
    const auto& ds = doc_.Diagnostics();
    ImGui::Text("Diagnostics (%zu)", ds.size());
    for (usize i = 0; i < ds.size(); ++i) {
        const BtProblem& p = ds[i];
        ImGui::PushID(static_cast<int>(i));
        const std::string text = p.code + " " + p.path + ": " + p.message;
        ImGui::PushStyleColor(ImGuiCol_Text, p.warning ? ImVec4(1, 0.8f, 0.3f, 1) : ImVec4(1, 0.4f, 0.4f, 1));
        if (ImGui::Selectable(text.c_str())) {
            for (const BtLayoutNode& l : Layout())
                if (BtProblemPath(doc_.Get(), l.path) == p.path) Select(l.path);
        }
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
}

void BehaviorTreeEditor::DrawDebugger() {
    ImGui::Text("Debugger: %.2fs, %llu runs", live_->Time(), static_cast<unsigned long long>(live_->RootRuns()));
    ImGui::TextDisabled("Active:");
    for (usize i : live_->ActivePath()) {
        if (i >= live_->Nodes().size()) continue;
        const auto& info = live_->Nodes()[i];
        ImGui::Text("%*s%s", info.depth * 2, "", info.node->Label().c_str());
    }
    if (live_board_ != nullptr) {
        ImGui::Separator();
        ImGui::TextDisabled("Blackboard:");
        for (const std::string& k : live_board_->Names()) ImGui::Text("%s = %s", k.c_str(), ValueToString(*live_board_->Get(k)).c_str());
    }
}

void BehaviorTreeEditor::HandleKeys() {
    ImGuiIO& io = ImGui::GetIO();
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || io.WantTextInput) return;
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) io.KeyShift ? (void)doc_.Redo() : (void)doc_.Undo();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) doc_.Redo();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) SaveNow();
}

} // namespace aether::editor
