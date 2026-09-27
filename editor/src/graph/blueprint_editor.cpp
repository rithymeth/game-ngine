#include "graph/blueprint_editor.h"

#include "aether/blueprint/nodes.h"

#include <imgui.h>
#include <imgui_internal.h> // ImRect, BeginDragDropTargetCustom
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>

namespace aether::editor {

using bp::Graph;
using bp::GraphKind;
using bp::NodeId;
using nlohmann::json;

namespace {

constexpr float kLeftWidth = 230.0f;
constexpr float kRightWidth = 280.0f;
constexpr float kResultsHeight = 130.0f;
const char* const kTypeNames[] = {"bool", "int", "float", "string", "Vec3", "Quat", "Entity"};

bool HasEdits(const GraphViewResult& r) {
    return !r.moved.empty() || r.connect || r.disconnect || !r.deleted.empty() || !r.comments_changed.empty() ||
           r.deleted_comment >= 0;
}

const char* KindLabel(GraphKind kind) {
    switch (kind) {
    case GraphKind::EventGraph: return "Event Graph";
    case GraphKind::Function: return "Function";
    case GraphKind::Macro: return "Macro";
    }
    return "";
}

// A type picker: the base type and an Array toggle. Returns true on change.
bool TypePicker(const char* id, bp::PinType& type, bool allow_exec) {
    ImGui::PushID(id);
    bool changed = false;
    const std::string current = type.IsExec() ? "exec" : bp::TypeName(bp::PinType::Of(type.type));
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::BeginCombo("##type", current.c_str())) {
        if (allow_exec && ImGui::Selectable("exec", type.IsExec())) {
            type = bp::PinType::Exec();
            changed = true;
        }
        for (const char* name : kTypeNames) {
            if (ImGui::Selectable(name, !type.IsExec() && current == name)) {
                const bool array = type.is_array && !type.IsExec();
                type = *bp::ParseType(name);
                type.is_array = array;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    if (!type.IsExec()) {
        ImGui::SameLine();
        bool array = type.is_array;
        if (ImGui::Checkbox("Array", &array)) {
            type.is_array = array;
            changed = true;
        }
    }
    ImGui::PopID();
    return changed;
}

// Edits a literal of `type` in place. Entities, structs and arrays have no
// literal form, so they show as read-only text.
bool ValueEditor(const char* id, const bp::PinType& type, bp::Value& value) {
    ImGui::PushID(id);
    bool changed = false;
    if (type.is_array || type.type == bp::ValueType::Entity || type.type == bp::ValueType::Struct ||
        type.type == bp::ValueType::Wildcard || type.type == bp::ValueType::None) {
        ImGui::TextDisabled("%s", type.is_array ? "(empty array)" : "(none)");
    } else {
        if (value.index() == 0) value = bp::DefaultValue(type);
        switch (type.type) {
        case bp::ValueType::Bool: {
            bool v = std::holds_alternative<bool>(value) && std::get<bool>(value);
            if (ImGui::Checkbox("##v", &v)) value = v, changed = true;
            break;
        }
        case bp::ValueType::Int: {
            int v = std::holds_alternative<i32>(value) ? std::get<i32>(value) : 0;
            if (ImGui::InputInt("##v", &v)) value = static_cast<i32>(v), changed = true;
            break;
        }
        case bp::ValueType::Float: {
            float v = std::holds_alternative<f32>(value) ? std::get<f32>(value) : 0.0f;
            if (ImGui::DragFloat("##v", &v, 0.1f)) value = v, changed = true;
            break;
        }
        case bp::ValueType::String: {
            std::string v = std::holds_alternative<std::string>(value) ? std::get<std::string>(value) : std::string();
            if (ImGui::InputText("##v", &v)) value = v, changed = true;
            break;
        }
        case bp::ValueType::Vec3: {
            Vec3 v = std::holds_alternative<Vec3>(value) ? std::get<Vec3>(value) : Vec3{};
            float f[3] = {v.x, v.y, v.z};
            if (ImGui::DragFloat3("##v", f, 0.1f)) value = Vec3{f[0], f[1], f[2]}, changed = true;
            break;
        }
        case bp::ValueType::Quat: {
            Quaternion q = std::holds_alternative<Quaternion>(value) ? std::get<Quaternion>(value) : Quaternion{};
            float f[4] = {q.x, q.y, q.z, q.w};
            if (ImGui::DragFloat4("##v", f, 0.01f)) value = Quaternion{f[0], f[1], f[2], f[3]}, changed = true;
            break;
        }
        default: break;
        }
    }
    ImGui::PopID();
    return changed;
}

} // namespace

BlueprintEditor::BlueprintEditor(BlueprintDocument& document) : doc_(document) {
    for (const Graph& g : doc_.Get().graphs) {
        if (g.kind == GraphKind::EventGraph) {
            OpenGraph(g.name);
            break;
        }
    }
}

// --- Selection and tabs ------------------------------------------------------------

void BlueprintEditor::Select(const Item& item) {
    if (!(item == selected_)) {
        selected_ = item;
        name_edit_ = item.kind == ItemKind::Node || item.kind == ItemKind::Comment ? std::string() : item.name;
    }
}

void BlueprintEditor::OpenGraph(const std::string& name) {
    if (doc_.Get().FindGraph(name) == nullptr) return;
    if (std::find(tabs_.begin(), tabs_.end(), name) == tabs_.end()) tabs_.push_back(name);
    active_ = name;
    select_tab_ = name;
}

void BlueprintEditor::CloseGraph(const std::string& name) {
    const Graph* g = doc_.Get().FindGraph(name);
    if (g != nullptr && g->kind == GraphKind::EventGraph) return;
    tabs_.erase(std::remove(tabs_.begin(), tabs_.end(), name), tabs_.end());
    if (active_ == name) active_ = tabs_.empty() ? std::string() : tabs_.front();
    if (palette_.graph == name) palette_ = {};
}

void BlueprintEditor::FocusNode(const std::string& graph, NodeId node) {
    OpenGraph(graph);
    const Graph* g = doc_.Get().FindGraph(graph);
    if (g == nullptr || g->Find(node) == nullptr) return;
    GraphViewState& view = views_[graph];
    view.selection = {node};
    view.selected_comment = -1;
    view.request_fit = view.request_fit_selection = true;
    Select({ItemKind::Node, graph, node, -1});
}

void BlueprintEditor::ForgetMissing() {
    const bp::Blueprint& b = doc_.Get();
    tabs_.erase(std::remove_if(tabs_.begin(), tabs_.end(), [&](const std::string& t) { return b.FindGraph(t) == nullptr; }),
                tabs_.end());
    if (b.FindGraph(active_) == nullptr) active_ = tabs_.empty() ? std::string() : tabs_.front();
    for (auto& [name, view] : views_) {
        const Graph* g = b.FindGraph(name);
        if (g == nullptr) continue;
        for (auto it = view.selection.begin(); it != view.selection.end();) {
            it = g->Find(*it) == nullptr ? view.selection.erase(it) : std::next(it);
        }
        if (view.selected_comment >= static_cast<int>(g->comments.size())) view.selected_comment = -1;
    }
    bool gone = false;
    switch (selected_.kind) {
    case ItemKind::Variable: gone = b.FindVariable(selected_.name) == nullptr; break;
    case ItemKind::Graph: gone = b.FindGraph(selected_.name) == nullptr; break;
    case ItemKind::Dispatcher: gone = b.FindDispatcher(selected_.name) == nullptr; break;
    case ItemKind::Node: {
        const Graph* g = b.FindGraph(selected_.name);
        gone = g == nullptr || g->Find(selected_.node) == nullptr;
        break;
    }
    case ItemKind::Comment: {
        const Graph* g = b.FindGraph(selected_.name);
        gone = g == nullptr || selected_.comment >= static_cast<int>(g->comments.size());
        break;
    }
    case ItemKind::None: break;
    }
    if (gone) Select({});
    if (palette_.open && b.FindGraph(palette_.graph) == nullptr) palette_ = {};
}

// --- Actions -------------------------------------------------------------------

void BlueprintEditor::CompileNow() {
    const bp::CompileResult& r = doc_.Compile();
    status_ = r.Ok() ? "Compiled: " + std::to_string(r.diagnostics.warnings) + " warning(s)"
                     : "Compile failed: " + std::to_string(r.diagnostics.errors) + " error(s)";
}

bool BlueprintEditor::SaveNow() {
    std::string error;
    if (!doc_.Save(&error)) {
        status_ = "Save failed: " + error;
        return false;
    }
    status_ = "Saved " + doc_.Path().filename().string();
    return true;
}

bool BlueprintEditor::RenameGraph(const std::string& from, const std::string& to) {
    std::string error;
    if (!doc_.RenameGraph(from, to, &error)) {
        status_ = error;
        return false;
    }
    std::replace(tabs_.begin(), tabs_.end(), from, to);
    if (active_ == from) active_ = to;
    if (select_tab_ == from) select_tab_ = to;
    if (auto it = views_.find(from); it != views_.end()) {
        views_[to] = it->second;
        views_.erase(from);
    }
    if (palette_.graph == from) palette_.graph = to;
    if ((selected_.kind == ItemKind::Graph || selected_.kind == ItemKind::Node || selected_.kind == ItemKind::Comment) &&
        selected_.name == from) {
        selected_.name = to;
    }
    return true;
}

bool BlueprintEditor::RenameVariable(const std::string& from, const std::string& to) {
    std::string error;
    if (!doc_.RenameVariable(from, to, &error)) {
        status_ = error;
        return false;
    }
    if (selected_.kind == ItemKind::Variable && selected_.name == from) selected_.name = to;
    return true;
}

bool BlueprintEditor::RenameDispatcher(const std::string& from, const std::string& to) {
    std::string error;
    if (!doc_.RenameDispatcher(from, to, &error)) {
        status_ = error;
        return false;
    }
    if (selected_.kind == ItemKind::Dispatcher && selected_.name == from) selected_.name = to;
    return true;
}

// --- Palette ---------------------------------------------------------------------

void BlueprintEditor::OpenPalette(float x, float y, const GraphPinRef* from) {
    if (active_.empty()) return;
    palette_ = {};
    palette_.open = palette_.needs_popup = true;
    palette_.graph = active_;
    palette_.x = x;
    palette_.y = y;
    if (from != nullptr) palette_.from = *from;
}

std::vector<bp::PaletteEntry> BlueprintEditor::PaletteResults() const {
    const Graph* g = doc_.Get().FindGraph(palette_.graph);
    if (!palette_.open || g == nullptr) return {};
    return SearchPalette(doc_.Get(), *g, palette_.query, palette_.from ? &*palette_.from : nullptr);
}

NodeId BlueprintEditor::PlaceFromPalette(const std::string& type) {
    if (!palette_.open || doc_.Get().FindGraph(palette_.graph) == nullptr) return 0;
    const Palette p = palette_;
    palette_ = {};
    NodeId node = 0;
    bool linked = true;
    doc_.Edit("Add node", [&](bp::Blueprint& b) {
        Graph& g = *b.FindGraph(p.graph);
        node = AddNode(g, type, p.x, p.y);
        if (p.from) linked = ConnectToNewNode(b, g, *p.from, node);
    });
    if (!linked) status_ = "The new node has no pin that fits.";
    GraphViewState& view = views_[p.graph];
    view.selection = {node};
    Select({ItemKind::Node, p.graph, node, -1});
    return node;
}

void BlueprintEditor::DrawPalette() {
    if (!palette_.open) return;
    if (palette_.needs_popup) {
        const GraphViewState& view = views_[palette_.graph];
        ImGui::SetNextWindowPos(ImVec2(view.CanvasToScreenX(palette_.x), view.CanvasToScreenY(palette_.y)), ImGuiCond_Always);
        ImGui::OpenPopup("##palette");
        palette_.needs_popup = false;
    }
    ImGui::SetNextWindowSize(ImVec2(320, 380), ImGuiCond_Appearing);
    if (!ImGui::BeginPopup("##palette")) {
        palette_ = {}; // closed by clicking outside or Esc
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { // even while typing in the search box
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        palette_ = {};
        return;
    }
    if (palette_.from) ImGui::TextDisabled("Nodes that fit '%s'", palette_.from->pin.c_str());
    else ImGui::TextDisabled("All actions");
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    const bool enter = ImGui::InputTextWithHint("##search", "Search", &palette_.query, ImGuiInputTextFlags_EnterReturnsTrue);
    const std::vector<bp::PaletteEntry> results = PaletteResults();
    std::string chosen;
    if (enter && !results.empty()) chosen = results.front().id;
    ImGui::BeginChild("##results");
    std::string last_category;
    for (const bp::PaletteEntry& e : results) {
        if (e.category != last_category) {
            ImGui::SeparatorText(e.category.c_str());
            last_category = e.category;
        }
        if (ImGui::Selectable((e.title + "##" + e.id).c_str())) chosen = e.id;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", e.id.c_str());
    }
    if (results.empty()) ImGui::TextDisabled("No matching nodes.");
    ImGui::EndChild();
    if (!chosen.empty()) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        PlaceFromPalette(chosen);
        return;
    }
    ImGui::EndPopup();
}

// --- Drawing ---------------------------------------------------------------------

void BlueprintEditor::Draw() {
    ImGui::PushID(this);
    HandleKeys();
    DrawToolbar();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float top = std::max(100.0f, avail.y - kResultsHeight - ImGui::GetStyle().ItemSpacing.y);
    const float center = std::max(200.0f, avail.x - kLeftWidth - kRightWidth - 2 * ImGui::GetStyle().ItemSpacing.x);

    ImGui::BeginChild("##my_blueprint", ImVec2(kLeftWidth, top), ImGuiChildFlags_Borders);
    DrawMyBlueprint();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##graphs", ImVec2(center, top), ImGuiChildFlags_None);
    DrawGraphTabs();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##details", ImVec2(0, top), ImGuiChildFlags_Borders);
    DrawDetails();
    ImGui::EndChild();
    ImGui::BeginChild("##results", ImVec2(0, 0), ImGuiChildFlags_Borders);
    DrawResults();
    ImGui::EndChild();
    DrawPalette();
    ImGui::PopID();
}

void BlueprintEditor::HandleKeys() {
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || ImGui::GetIO().WantTextInput) return;
    const bool ctrl = ImGui::GetIO().KeyCtrl;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        if (doc_.Undo()) ForgetMissing();
    } else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
        if (doc_.Redo()) ForgetMissing();
    } else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        SaveNow();
    } else if (ImGui::IsKeyPressed(ImGuiKey_F7, false)) {
        CompileNow();
    }
}

void BlueprintEditor::DrawToolbar() {
    const char* label = "Compile";
    ImVec4 color(0.35f, 0.35f, 0.38f, 1.0f);
    switch (doc_.CompileStatus()) {
    case BlueprintDocument::Status::NotCompiled:
    case BlueprintDocument::Status::Stale: label = "Compile (?)"; color = ImVec4(0.75f, 0.6f, 0.15f, 1.0f); break;
    case BlueprintDocument::Status::UpToDate: label = "Compile (OK)"; color = ImVec4(0.2f, 0.55f, 0.25f, 1.0f); break;
    case BlueprintDocument::Status::Warnings: label = "Compile (!)"; color = ImVec4(0.75f, 0.6f, 0.15f, 1.0f); break;
    case BlueprintDocument::Status::Errors: label = "Compile (X)"; color = ImVec4(0.7f, 0.15f, 0.15f, 1.0f); break;
    }
    ImGui::PushStyleColor(ImGuiCol_Button, color);
    if (ImGui::Button(label)) CompileNow();
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::Button(doc_.Dirty() ? "Save*" : "Save")) SaveNow();
    ImGui::SameLine();
    ImGui::BeginDisabled(!doc_.CanUndo());
    if (ImGui::Button("Undo") && doc_.Undo()) ForgetMissing();
    ImGui::EndDisabled();
    if (doc_.CanUndo() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Undo %s", doc_.UndoLabel().c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(!doc_.CanRedo());
    if (ImGui::Button("Redo") && doc_.Redo()) ForgetMissing();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("|  %s   Parent: %s", doc_.Name().c_str(), doc_.Get().parent.c_str());
    if (!status_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("  %s", status_.c_str());
    }
}

void BlueprintEditor::DrawMyBlueprint() {
    const bp::Blueprint& b = doc_.Get();
    auto section = [&](const char* title, const char* add_id, auto&& add) {
        const bool open = ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
        if (add_id != nullptr) {
            ImGui::SameLine(ImGui::GetWindowWidth() - 34.0f);
            if (ImGui::SmallButton(add_id)) add();
        }
        return open;
    };
    auto row = [&](const std::string& label, const Item& item, const char* suffix) {
        const bool selected = selected_ == item;
        const std::string text = suffix != nullptr && *suffix ? label + "  " + suffix : label;
        if (ImGui::Selectable((text + "##" + label).c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
            Select(item);
            if (item.kind == ItemKind::Graph && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) OpenGraph(item.name);
        }
    };

    ImGui::TextUnformatted("MY BLUEPRINT");
    ImGui::Separator();
    if (section("Graphs", nullptr, [] {})) {
        for (const Graph& g : b.graphs) {
            if (g.kind == GraphKind::EventGraph) row(g.name, {ItemKind::Graph, g.name, 0, -1}, nullptr);
        }
    }
    if (section("Functions", "+##fn", [&] {
            const std::string name = doc_.AddFunction();
            OpenGraph(name);
            Select({ItemKind::Graph, name, 0, -1});
        })) {
        for (const Graph& g : b.graphs) {
            if (g.kind == GraphKind::Function) row(g.name, {ItemKind::Graph, g.name, 0, -1}, g.pure ? "pure" : nullptr);
        }
    }
    if (section("Macros", "+##macro", [&] {
            const std::string name = doc_.AddMacro();
            OpenGraph(name);
            Select({ItemKind::Graph, name, 0, -1});
        })) {
        for (const Graph& g : b.graphs) {
            if (g.kind == GraphKind::Macro) row(g.name, {ItemKind::Graph, g.name, 0, -1}, nullptr);
        }
    }
    if (section("Variables", "+##var", [&] { Select({ItemKind::Variable, doc_.AddVariable(), 0, -1}); })) {
        for (const bp::Variable& v : b.variables) {
            const std::string type = bp::TypeName(v.type);
            row(v.name, {ItemKind::Variable, v.name, 0, -1}, type.c_str());
            if (ImGui::BeginDragDropSource()) { // drop on the graph for a Get node
                ImGui::SetDragDropPayload("AETHER_BP_VAR", v.name.data(), v.name.size());
                ImGui::Text("Get %s", v.name.c_str());
                ImGui::EndDragDropSource();
            }
        }
    }
    if (section("Event Dispatchers", "+##disp", [&] { Select({ItemKind::Dispatcher, doc_.AddDispatcher(), 0, -1}); })) {
        for (const bp::Dispatcher& d : b.dispatchers) row(d.name, {ItemKind::Dispatcher, d.name, 0, -1}, nullptr);
    }
    if (!b.interfaces.empty() && section("Interfaces", nullptr, [] {})) {
        for (const std::string& i : b.interfaces) ImGui::BulletText("%s", i.c_str());
    }
}

void BlueprintEditor::DrawGraphTabs() {
    if (tabs_.empty()) {
        ImGui::TextDisabled("Double-click a graph in My Blueprint to open it.");
        return;
    }
    if (!ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_AutoSelectNewTabs)) return;
    std::string close;
    const std::vector<std::string> tabs = tabs_;
    for (const std::string& name : tabs) {
        const Graph* g = doc_.Get().FindGraph(name);
        if (g == nullptr) continue;
        bool open = true;
        const ImGuiTabItemFlags flags = select_tab_ == name ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        const std::string label = (g->kind == GraphKind::Function ? "fn " : g->kind == GraphKind::Macro ? "m " : "") + name;
        if (ImGui::BeginTabItem((label + "###" + name).c_str(), g->kind == GraphKind::EventGraph ? nullptr : &open, flags)) {
            if (select_tab_.empty() || select_tab_ == name) active_ = name;
            DrawGraph(name);
            ImGui::EndTabItem();
        }
        if (!open) close = name;
    }
    select_tab_.clear();
    ImGui::EndTabBar();
    if (!close.empty()) CloseGraph(close);
}

void BlueprintEditor::DrawGraph(const std::string& name) {
    const Graph* g = doc_.Get().FindGraph(name);
    if (g == nullptr) return;
    BlueprintViewOptions options = debug;
    if (doc_.CompileStatus() != BlueprintDocument::Status::NotCompiled) options.diagnostics = &doc_.LastCompile().diagnostics;
    const GraphViewModel model = BuildBlueprintView(doc_.Get(), *g, options);
    GraphViewState& view = views_[name];
    const ImVec2 hint_at = ImGui::GetCursorScreenPos();
    const GraphViewResult result = DrawGraphView(("graph_" + name).c_str(), model, view);
    if (g->nodes.empty()) {
        ImGui::GetWindowDrawList()->AddText(ImVec2(hint_at.x + 16, hint_at.y + 16), IM_COL32(170, 170, 170, 255),
                                            "Right-click or press Tab to add your first node. Try Event BeginPlay.");
    }
    // A variable dragged from My Blueprint drops as a Get node.
    const ImRect canvas(view.origin_x, view.origin_y, view.origin_x + view.width, view.origin_y + view.height);
    if (ImGui::BeginDragDropTargetCustom(canvas, ImGui::GetID(("drop_" + name).c_str()))) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("AETHER_BP_VAR")) {
            const std::string var(static_cast<const char*>(payload->Data), static_cast<usize>(payload->DataSize));
            const ImVec2 m = ImGui::GetIO().MousePos;
            doc_.Edit("Add Get " + var, [&](bp::Blueprint& b) {
                AddNode(*b.FindGraph(name), "Var.Get:" + var, view.ScreenToCanvasX(m.x), view.ScreenToCanvasY(m.y));
            });
        }
        ImGui::EndDragDropTarget();
    }
    HandleGraphResult(name, result, model);
}

void BlueprintEditor::HandleGraphResult(const std::string& name, const GraphViewResult& r, const GraphViewModel& model) {
    GraphViewState& view = views_[name];
    std::vector<NodeId> selected(view.selection.begin(), view.selection.end());
    if (r.copy || r.cut) {
        clipboard_ = doc_.CopyNodes(name, selected);
        ImGui::SetClipboardText(clipboard_.c_str());
    }
    if (r.cut) {
        doc_.Edit("Cut", [&](bp::Blueprint& b) { DeleteNodes(*b.FindGraph(name), selected); });
        view.selection.clear();
    }
    if (r.paste) {
        const char* system = ImGui::GetClipboardText();
        const std::string text = system != nullptr && *system ? std::string(system) : clipboard_;
        const std::vector<NodeId> pasted = doc_.PasteNodes(name, text, r.mouse_x, r.mouse_y);
        if (!pasted.empty()) view.selection = std::set<u32>(pasted.begin(), pasted.end());
    }
    if (r.duplicate) {
        const std::vector<NodeId> copies = doc_.DuplicateNodes(name, selected);
        if (!copies.empty()) view.selection = std::set<u32>(copies.begin(), copies.end());
    }
    if (r.comment_selection) {
        float x0, y0, x1, y1;
        if (NodeBounds(model, view.selection, view.font_size, x0, y0, x1, y1)) doc_.AddComment(name, x0, y0, x1, y1);
    }
    if (HasEdits(r)) {
        std::vector<std::string> errors;
        const bool only_moves = r.moved.size() + r.comments_changed.size() > 0 && !r.connect && !r.disconnect &&
                                r.deleted.empty() && r.deleted_comment < 0;
        doc_.Edit(only_moves ? "Move" : r.connect ? "Connect" : r.disconnect ? "Break link" : "Delete",
                  [&](bp::Blueprint& b) { errors = ApplyGraphEdits(b, *b.FindGraph(name), r); });
        if (!errors.empty()) {
            status_ = errors.front();
            if (r.connect && !r.disconnect && r.moved.empty() && r.deleted.empty() && r.comments_changed.empty() &&
                r.deleted_comment < 0) {
                doc_.CancelEdit(); // a refused link changed nothing: drop its undo step
            }
        }
        ForgetMissing();
    }
    if (r.open_palette) OpenPalette(r.palette_x, r.palette_y, r.palette_from_pin ? &r.palette_from : nullptr);
    if (r.double_clicked != 0) {
        if (const bp::Node* n = doc_.Get().FindGraph(name)->Find(r.double_clicked)) {
            for (const char* prefix : {"Call.Self:", "Macro:"}) {
                if (n->type.rfind(prefix, 0) == 0) OpenGraph(n->type.substr(std::string(prefix).size()));
            }
        }
    }
    if (r.selection_changed || r.connect || !r.deleted.empty()) {
        if (view.selected_comment >= 0) Select({ItemKind::Comment, name, 0, view.selected_comment});
        else if (view.selection.size() == 1) Select({ItemKind::Node, name, *view.selection.begin(), -1});
        else if (selected_.kind == ItemKind::Node || selected_.kind == ItemKind::Comment) Select({});
    }
}

// --- Details -----------------------------------------------------------------------

void BlueprintEditor::DrawDetails() {
    ImGui::TextUnformatted("DETAILS");
    ImGui::Separator();
    const Item item = selected_; // the panels may change the selection (renames, deletes)
    switch (item.kind) {
    case ItemKind::None: ImGui::TextDisabled("Select a variable, graph or node."); break;
    case ItemKind::Variable: DrawVariableDetails(item.name); break;
    case ItemKind::Graph: DrawGraphDetails(item.name); break;
    case ItemKind::Dispatcher: DrawDispatcherDetails(item.name); break;
    case ItemKind::Node: DrawNodeDetails(item.name, item.node); break;
    case ItemKind::Comment: DrawCommentDetails(item.name, item.comment); break;
    }
}

void BlueprintEditor::DrawVariableDetails(const std::string& name) {
    const bp::Variable* found = doc_.Get().FindVariable(name);
    if (found == nullptr) return;
    bp::Variable v = *found;
    ImGui::Text("Variable");
    ImGui::InputText("Name", &name_edit_);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (!RenameVariable(name, name_edit_)) name_edit_ = name;
        return; // the rest shows next frame, under the new name
    }
    auto update = [&](const std::string& label, const std::string& key) {
        doc_.Edit(label, [&](bp::Blueprint& b) {
            for (bp::Variable& x : b.variables) {
                if (x.name == name) x = v;
            }
        }, key);
    };
    ImGui::TextUnformatted("Type");
    ImGui::SameLine(90);
    bp::PinType type = v.type;
    if (TypePicker("type", type, false)) {
        doc_.SetVariableType(name, type);
        return;
    }
    ImGui::TextUnformatted("Default");
    ImGui::SameLine(90);
    if (ValueEditor("default", v.type, v.default_value)) update("Set default of " + name, "default:" + name);
    bool editable = (v.flags & bp::Var_InstanceEditable) != 0, expose = (v.flags & bp::Var_ExposeOnSpawn) != 0;
    if (ImGui::Checkbox("Instance Editable", &editable)) {
        v.flags = editable ? (v.flags | bp::Var_InstanceEditable) : (v.flags & ~u32(bp::Var_InstanceEditable));
        update("Instance Editable", {});
    }
    if (ImGui::Checkbox("Expose on Spawn", &expose)) {
        v.flags = expose ? (v.flags | bp::Var_ExposeOnSpawn) : (v.flags & ~u32(bp::Var_ExposeOnSpawn));
        update("Expose on Spawn", {});
    }
    if (ImGui::InputText("Tooltip", &v.tooltip)) update("Set tooltip", "tooltip:" + name);
    if (ImGui::InputText("Category", &v.category)) update("Set category", "category:" + name);
    ImGui::Spacing();
    if (ImGui::Button("Delete variable")) doc_.RemoveVariable(name), ForgetMissing();
}

void BlueprintEditor::DrawParams(const std::string& id, std::vector<bp::Variable>& params, bool allow_exec,
                                 const std::string& undo_key,
                                 const std::function<std::vector<bp::Variable>&(bp::Blueprint&)>& locate) {
    ImGui::PushID(id.c_str());
    int remove = -1;
    bool changed = false;
    for (usize i = 0; i < params.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetNextItemWidth(90);
        std::string name = params[i].name;
        ImGui::InputText("##name", &name);
        if (ImGui::IsItemDeactivatedAfterEdit() && BlueprintDocument::IsValidName(name) &&
            std::none_of(params.begin(), params.end(), [&](const bp::Variable& p) { return p.name == name; })) {
            params[i].name = name;
            changed = true;
        }
        ImGui::SameLine();
        if (TypePicker("t", params[i].type, allow_exec)) {
            params[i].default_value = params[i].type.IsExec() ? bp::Value{} : bp::DefaultValue(params[i].type);
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) remove = static_cast<int>(i);
        ImGui::PopID();
    }
    if (remove >= 0) {
        params.erase(params.begin() + remove);
        changed = true;
    }
    if (ImGui::SmallButton("+ Add")) {
        bp::Variable p;
        int n = static_cast<int>(params.size());
        do p.name = "Param" + std::to_string(n++);
        while (std::any_of(params.begin(), params.end(), [&](const bp::Variable& x) { return x.name == p.name; }));
        p.type = bp::PinType::Of(bp::ValueType::Float);
        p.default_value = bp::DefaultValue(p.type);
        params.push_back(p);
        changed = true;
    }
    if (changed) doc_.Edit("Edit " + undo_key, [&](bp::Blueprint& b) { locate(b) = params; });
    ImGui::PopID();
}

void BlueprintEditor::DrawGraphDetails(const std::string& name) {
    const Graph* g = doc_.Get().FindGraph(name);
    if (g == nullptr) return;
    ImGui::Text("%s", KindLabel(g->kind));
    if (g->kind == GraphKind::EventGraph) {
        ImGui::TextUnformatted(name.c_str());
        ImGui::TextDisabled("%zu nodes", g->nodes.size());
        return;
    }
    ImGui::InputText("Name", &name_edit_);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (!RenameGraph(name, name_edit_)) name_edit_ = name;
        return;
    }
    const std::string current = name;
    if (g->kind == GraphKind::Function) {
        bool pure = g->pure;
        if (ImGui::Checkbox("Pure", &pure)) {
            doc_.Edit("Pure", [&](bp::Blueprint& b) { b.FindGraph(current)->pure = pure; });
        }
    }
    const bool macro = g->kind == GraphKind::Macro;
    ImGui::SeparatorText("Inputs");
    std::vector<bp::Variable> inputs = doc_.Get().FindGraph(current)->inputs;
    DrawParams("in", inputs, macro, "inputs", [current](bp::Blueprint& b) -> std::vector<bp::Variable>& {
        return b.FindGraph(current)->inputs;
    });
    ImGui::SeparatorText("Outputs");
    std::vector<bp::Variable> outputs = doc_.Get().FindGraph(current)->outputs;
    DrawParams("out", outputs, macro, "outputs", [current](bp::Blueprint& b) -> std::vector<bp::Variable>& {
        return b.FindGraph(current)->outputs;
    });
    ImGui::Spacing();
    if (ImGui::Button("Open")) OpenGraph(current);
    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
        std::string error;
        if (!doc_.RemoveGraph(current, &error)) status_ = error;
        ForgetMissing();
    }
}

void BlueprintEditor::DrawDispatcherDetails(const std::string& name) {
    const bp::Dispatcher* d = doc_.Get().FindDispatcher(name);
    if (d == nullptr) return;
    ImGui::Text("Event Dispatcher");
    ImGui::InputText("Name", &name_edit_);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (!RenameDispatcher(name, name_edit_)) name_edit_ = name;
        return;
    }
    const std::string current = name;
    ImGui::SeparatorText("Parameters");
    std::vector<bp::Variable> params = doc_.Get().FindDispatcher(current)->params;
    DrawParams("params", params, false, "parameters", [current](bp::Blueprint& b) -> std::vector<bp::Variable>& {
        for (bp::Dispatcher& x : b.dispatchers) {
            if (x.name == current) return x.params;
        }
        return b.dispatchers.front().params; // unreachable: it was found above
    });
    ImGui::Spacing();
    if (ImGui::Button("Delete")) doc_.RemoveDispatcher(current), ForgetMissing();
}

void BlueprintEditor::DrawNodeDetails(const std::string& graph, NodeId id) {
    const Graph* g = doc_.Get().FindGraph(graph);
    const bp::Node* n = g != nullptr ? g->Find(id) : nullptr;
    if (n == nullptr) return;
    bp::NodeError error;
    const std::optional<bp::NodeSignature> sig = bp::ResolveNode(doc_.Get(), *g, *n, &error);
    ImGui::Text("%s", sig ? sig->title.c_str() : n->type.c_str());
    ImGui::TextDisabled("%s  (node %u)", n->type.c_str(), n->id);
    if (!sig) ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1), "%s %s", error.code.c_str(), error.message.c_str());
    if (doc_.CompileStatus() != BlueprintDocument::Status::NotCompiled) {
        for (const bp::Diagnostic* d : doc_.LastCompile().diagnostics.For(graph, id)) {
            ImGui::TextColored(d->severity == bp::Severity::Error ? ImVec4(1, 0.35f, 0.35f, 1) : ImVec4(1, 0.8f, 0.3f, 1),
                               "%s %s", d->code.c_str(), d->message.c_str());
        }
    }
    std::string comment = n->comment;
    if (ImGui::InputText("Comment", &comment)) {
        doc_.Edit("Node comment", [&](bp::Blueprint& b) { b.FindGraph(graph)->Find(id)->comment = comment; },
                  "node-comment:" + graph + ":" + std::to_string(id));
    }
    if (!sig) return;
    // Defaults for the data inputs that aren't linked.
    bool header = false;
    for (const bp::PinDesc& p : sig->pins) {
        if (p.dir != bp::PinDir::In || p.type.IsExec()) continue;
        const bool linked = std::any_of(g->links.begin(), g->links.end(),
                                        [&](const bp::Link& l) { return l.to.node == id && l.to.pin == p.name; });
        if (linked) continue;
        if (!header) {
            ImGui::SeparatorText("Pin defaults");
            header = true;
        }
        bp::Value value = p.default_value;
        if (n->defaults.contains(p.name)) bp::ValueFromJson(n->defaults[p.name], p.type, value);
        ImGui::TextUnformatted(p.name.c_str());
        ImGui::SameLine(110);
        ImGui::SetNextItemWidth(-1);
        if (ValueEditor(p.name.c_str(), p.type, value)) {
            doc_.Edit("Set " + p.name, [&](bp::Blueprint& b) { b.FindGraph(graph)->Find(id)->defaults[p.name] = bp::ValueToJson(value); },
                      "pin:" + graph + ":" + std::to_string(id) + ":" + p.name);
        }
    }
}

void BlueprintEditor::DrawCommentDetails(const std::string& graph, int index) {
    const Graph* g = doc_.Get().FindGraph(graph);
    if (g == nullptr || index < 0 || index >= static_cast<int>(g->comments.size())) return;
    bp::CommentBox box = g->comments[static_cast<usize>(index)];
    ImGui::Text("Comment");
    const std::string key = "comment:" + graph + ":" + std::to_string(index);
    auto store = [&](const char* label) {
        doc_.Edit(label, [&](bp::Blueprint& b) { b.FindGraph(graph)->comments[static_cast<usize>(index)] = box; }, key);
    };
    if (ImGui::InputTextMultiline("Text", &box.text, ImVec2(-1, 60))) store("Comment text");
    ImVec4 color = ImGui::ColorConvertU32ToFloat4(box.color);
    if (ImGui::ColorEdit4("Color", &color.x, ImGuiColorEditFlags_NoInputs)) {
        box.color = ImGui::ColorConvertFloat4ToU32(color);
        store("Comment color");
    }
    if (ImGui::Button("Delete comment")) {
        doc_.Edit("Delete comment", [&](bp::Blueprint& b) {
            auto& comments = b.FindGraph(graph)->comments;
            comments.erase(comments.begin() + index);
        });
        views_[graph].selected_comment = -1;
        ForgetMissing();
    }
}

// --- Compiler results -------------------------------------------------------------------

void BlueprintEditor::DrawResults() {
    const BlueprintDocument::Status status = doc_.CompileStatus();
    ImGui::TextUnformatted("COMPILER RESULTS");
    ImGui::SameLine();
    if (status == BlueprintDocument::Status::NotCompiled) {
        ImGui::TextDisabled("Not compiled yet (F7).");
        return;
    }
    const bp::ValidationResult& d = doc_.LastCompile().diagnostics;
    ImGui::TextDisabled("%s%zu error(s), %zu warning(s)%s", d.errors == 0 ? "OK: " : "", d.errors, d.warnings,
                        status == BlueprintDocument::Status::Stale ? "  (changed since; recompile)" : "");
    ImGui::Separator();
    for (usize i = 0; i < d.diagnostics.size(); ++i) {
        const bp::Diagnostic& diag = d.diagnostics[i];
        const bool error = diag.severity == bp::Severity::Error;
        ImGui::PushStyleColor(ImGuiCol_Text, error ? ImVec4(1, 0.4f, 0.4f, 1) : ImVec4(1, 0.8f, 0.3f, 1));
        const std::string text = std::string(error ? "Error " : "Warning ") + diag.code + "  [" + diag.graph + "]  " +
                                 diag.message + "##diag" + std::to_string(i);
        if (ImGui::Selectable(text.c_str()) && diag.node != 0) FocusNode(diag.graph, diag.node);
        ImGui::PopStyleColor();
    }
}

} // namespace aether::editor
