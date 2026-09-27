#include "graph/material_editor.h"

#include <imgui.h>
#include <imgui_internal.h> // ImRect, BeginDragDropTargetCustom
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cstdio>

namespace aether::editor {

using mat::Material;
using mat::NodeId;
using mat::PinType;
using nlohmann::json;

namespace {

constexpr float kLeftWidth = 230.0f;
constexpr float kRightWidth = 300.0f;
constexpr float kBottomHeight = 170.0f;

const PinType kValueTypes[] = {PinType::Float, PinType::Float2, PinType::Float3, PinType::Float4};
const PinType kAllTypes[] = {PinType::Float, PinType::Float2, PinType::Float3, PinType::Float4, PinType::Texture};

bool HasEdits(const GraphViewResult& r) { return !r.moved.empty() || r.connect || r.disconnect || !r.deleted.empty(); }

// A type combo over `types`. True on change.
template <usize N>
bool TypeCombo(const char* label, PinType& type, const PinType (&types)[N]) {
    bool changed = false;
    if (ImGui::BeginCombo(label, mat::TypeName(type))) {
        for (PinType t : types) {
            if (ImGui::Selectable(mat::TypeName(t), t == type)) {
                changed = t != type;
                type = t;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

Vec4 ReadVec(const json& j, const Vec4& fallback) {
    if (j.is_number()) {
        const float f = j.get<float>();
        return Vec4(f, f, f, f);
    }
    if (!j.is_array()) return fallback;
    float c[4] = {fallback.x, fallback.y, fallback.z, fallback.w};
    for (usize i = 0; i < j.size() && i < 4; ++i) {
        if (j[i].is_number()) c[i] = j[i].get<float>();
    }
    return Vec4(c[0], c[1], c[2], c[3]);
}

json VecJson(const Vec4& v, u32 n) {
    if (n <= 1) return v.x;
    json a = json::array();
    const float c[4] = {v.x, v.y, v.z, v.w};
    for (u32 i = 0; i < n; ++i) a.push_back(c[i]);
    return a;
}

// Drags up to 4 floats; true while changing.
bool DragVec(const char* label, Vec4& v, u32 n, float speed = 0.01f) {
    float c[4] = {v.x, v.y, v.z, v.w};
    const bool changed = ImGui::DragScalarN(label, ImGuiDataType_Float, c, static_cast<int>(std::max(n, 1u)), speed);
    if (changed) v = Vec4(c[0], c[1], c[2], c[3]);
    return changed;
}

} // namespace

MaterialEditor::MaterialEditor(MaterialDocument& document) : doc_(document) { view_.request_fit = true; }

void MaterialEditor::Draw() {
    ImGui::PushID(this);
    HandleKeys();
    DrawToolbar();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float top = std::max(100.0f, avail.y - kBottomHeight - ImGui::GetStyle().ItemSpacing.y);
    const float center = std::max(200.0f, avail.x - kLeftWidth - kRightWidth - 2 * spacing);
    ImGui::BeginChild("##parameters", ImVec2(kLeftWidth, top), ImGuiChildFlags_Borders);
    DrawParameters();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##graph", ImVec2(center, top), ImGuiChildFlags_None);
    DrawGraph();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##details", ImVec2(0, top), ImGuiChildFlags_Borders);
    DrawDetails();
    ImGui::EndChild();
    ImGui::BeginChild("##bottom", ImVec2(0, 0), ImGuiChildFlags_Borders);
    DrawBottom();
    ImGui::EndChild();
    DrawPalette();
    ImGui::PopID();
}

void MaterialEditor::HandleKeys() {
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

void MaterialEditor::DrawToolbar() {
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
    const mat::GeneratedMaterial& g = doc_.Generated();
    const mat::Analysis& a = g.analysis;
    const usize warnings = a.diagnostics.size() - a.errors;
    if (a.errors > 0) ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1), "%zu error(s)", a.errors);
    else if (warnings > 0) ImGui::TextColored(ImVec4(0.9f, 0.75f, 0.2f, 1), "%zu warning(s)", warnings);
    else ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1), "OK");
    ImGui::SameLine();
    ImGui::TextDisabled("|  %s%s", doc_.Name().c_str(), doc_.Get().is_function ? "  (material function)" : "");
    if (!status_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("  %s", status_.c_str());
    }
}

// --- Parameters ------------------------------------------------------------------------

void MaterialEditor::DrawParameters() {
    ImGui::SeparatorText("Parameters");
    if (ImGui::SmallButton("+ Scalar")) Select({ItemKind::Parameter, doc_.AddParameter(PinType::Float), 0});
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Vector")) Select({ItemKind::Parameter, doc_.AddParameter(PinType::Float3), 0});
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Texture")) Select({ItemKind::Parameter, doc_.AddParameter(PinType::Texture), 0});
    // Grouped as the Details panel of an instance will show them.
    std::vector<std::string> groups;
    for (const mat::Parameter& p : doc_.Get().parameters) {
        if (std::find(groups.begin(), groups.end(), p.group) == groups.end()) groups.push_back(p.group);
    }
    std::sort(groups.begin(), groups.end());
    for (const std::string& group : groups) {
        if (!group.empty()) ImGui::TextDisabled("%s", group.c_str());
        for (const mat::Parameter& p : doc_.Get().parameters) {
            if (p.group != group) continue;
            const bool selected = selected_.kind == ItemKind::Parameter && selected_.parameter == p.name;
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(MaterialPinColor(p.type)));
            ImGui::Bullet();
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::Selectable((p.name + "##param").c_str(), selected)) Select({ItemKind::Parameter, p.name, 0});
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("AETHER_MAT_PARAM", p.name.data(), p.name.size());
                ImGui::Text("%s", p.name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s  (drag into the graph)", mat::TypeName(p.type));
        }
    }
    if (doc_.Get().parameters.empty()) ImGui::TextDisabled("No parameters yet.");
}

// --- Graph -------------------------------------------------------------------------------

void MaterialEditor::DrawGraph() {
    const GraphViewModel model = BuildMaterialView(doc_.Get(), doc_.Analysis());
    const GraphViewResult result = DrawGraphView("material_graph", model, view_);
    const ImRect canvas(view_.origin_x, view_.origin_y, view_.origin_x + view_.width, view_.origin_y + view_.height);
    if (ImGui::BeginDragDropTargetCustom(canvas, ImGui::GetID("drop_param"))) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("AETHER_MAT_PARAM")) {
            const std::string name(static_cast<const char*>(payload->Data), static_cast<usize>(payload->DataSize));
            const ImVec2 m = ImGui::GetIO().MousePos;
            AddParameterNode(name, view_.ScreenToCanvasX(m.x), view_.ScreenToCanvasY(m.y));
        }
        ImGui::EndDragDropTarget();
    }
    HandleGraphResult(result);
}

void MaterialEditor::HandleGraphResult(const GraphViewResult& r) {
    std::vector<NodeId> selected(view_.selection.begin(), view_.selection.end());
    if (r.copy || r.cut) {
        clipboard_ = doc_.CopyNodes(selected);
        ImGui::SetClipboardText(clipboard_.c_str());
    }
    if (r.cut) {
        doc_.Edit("Cut", [&](Material& m) { DeleteMaterialNodes(m, selected); });
        view_.selection.clear();
    }
    if (r.paste) {
        const char* system = ImGui::GetClipboardText();
        const std::string text = system != nullptr && *system ? std::string(system) : clipboard_;
        const std::vector<NodeId> pasted = doc_.PasteNodes(text, r.mouse_x, r.mouse_y);
        if (!pasted.empty()) view_.selection = std::set<u32>(pasted.begin(), pasted.end());
    }
    if (r.duplicate) {
        const std::vector<NodeId> copies = doc_.DuplicateNodes(selected);
        if (!copies.empty()) view_.selection = std::set<u32>(copies.begin(), copies.end());
    }
    if (HasEdits(r)) {
        std::vector<std::string> errors;
        const bool only_moves = !r.moved.empty() && !r.connect && !r.disconnect && r.deleted.empty();
        doc_.Edit(only_moves ? "Move" : r.connect ? "Connect" : r.disconnect ? "Break link" : "Delete",
                  [&](Material& m) { errors = ApplyMaterialEdits(m, r); });
        if (!errors.empty()) {
            status_ = errors.front();
            if (r.connect && !r.disconnect && r.moved.empty() && r.deleted.empty()) doc_.CancelEdit();
        }
        ForgetMissing();
    }
    if (r.open_palette) OpenPalette(r.palette_x, r.palette_y, r.palette_from_pin ? &r.palette_from : nullptr);
    if (r.selection_changed || r.connect || !r.deleted.empty()) {
        if (view_.selection.size() == 1) Select({ItemKind::Node, {}, *view_.selection.begin()});
        else if (selected_.kind == ItemKind::Node) Select({});
    }
}

void MaterialEditor::FocusNode(NodeId node) {
    if (doc_.Get().Find(node) == nullptr) return;
    view_.selection = {node};
    view_.request_fit = view_.request_fit_selection = true;
    Select({ItemKind::Node, {}, node});
}

NodeId MaterialEditor::AddParameterNode(const std::string& name, float x, float y) {
    const mat::Parameter* p = doc_.Get().FindParameter(name);
    if (p == nullptr) return 0;
    const std::string type = MaterialDocument::ParameterNodeType(*p);
    NodeId node = 0;
    doc_.Edit("Add " + name, [&](Material& m) { node = AddMaterialNode(m, type, x, y); });
    view_.selection = {node};
    Select({ItemKind::Node, {}, node});
    return node;
}

// --- Palette -------------------------------------------------------------------------------

void MaterialEditor::OpenPalette(float x, float y, const GraphPinRef* from) {
    palette_ = {};
    palette_.open = palette_.needs_popup = true;
    palette_.x = x;
    palette_.y = y;
    if (from != nullptr) palette_.from = *from;
}

std::vector<mat::PaletteEntry> MaterialEditor::PaletteResults() const {
    return SearchMaterialPalette(doc_.Get(), palette_.query, palette_.from ? &*palette_.from : nullptr);
}

NodeId MaterialEditor::PlaceFromPalette(const std::string& type) {
    if (!palette_.open) return 0;
    const Palette p = palette_;
    palette_ = {};
    NodeId node = 0;
    bool linked = true;
    doc_.Edit("Add node", [&](Material& m) {
        node = AddMaterialNode(m, type, p.x, p.y);
        if (p.from) linked = ConnectToNewMaterialNode(m, *p.from, node);
    });
    if (!linked) status_ = "The new node has no pin that fits.";
    view_.selection = {node};
    Select({ItemKind::Node, {}, node});
    return node;
}

void MaterialEditor::DrawPalette() {
    if (!palette_.open) return;
    if (palette_.needs_popup) {
        ImGui::SetNextWindowPos(ImVec2(view_.CanvasToScreenX(palette_.x), view_.CanvasToScreenY(palette_.y)), ImGuiCond_Always);
        ImGui::OpenPopup("##material_palette");
        palette_.needs_popup = false;
    }
    ImGui::SetNextWindowSize(ImVec2(320, 380), ImGuiCond_Appearing);
    if (!ImGui::BeginPopup("##material_palette")) {
        palette_ = {};
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        palette_ = {};
        return;
    }
    if (palette_.from) ImGui::TextDisabled("Nodes that fit '%s'", palette_.from->pin.c_str());
    else ImGui::TextDisabled("All nodes");
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    const bool enter = ImGui::InputTextWithHint("##search", "Search", &palette_.query, ImGuiInputTextFlags_EnterReturnsTrue);
    const std::vector<mat::PaletteEntry> results = PaletteResults();
    std::string chosen;
    if (enter && !results.empty()) chosen = results.front().id;
    ImGui::BeginChild("##results");
    std::string last_category;
    for (const mat::PaletteEntry& e : results) {
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

// --- Details ----------------------------------------------------------------------------------

void MaterialEditor::Select(const Item& item) {
    if (item == selected_) return;
    selected_ = item;
    name_edit_ = item.kind == ItemKind::Parameter ? item.parameter : std::string();
}

void MaterialEditor::ForgetMissing() {
    if (selected_.kind == ItemKind::Parameter && doc_.Get().FindParameter(selected_.parameter) == nullptr) Select({});
    if (selected_.kind == ItemKind::Node && doc_.Get().Find(selected_.node) == nullptr) Select({});
    for (auto it = view_.selection.begin(); it != view_.selection.end();) {
        it = doc_.Get().Find(*it) == nullptr ? view_.selection.erase(it) : std::next(it);
    }
}

void MaterialEditor::DrawDetails() {
    switch (selected_.kind) {
    case ItemKind::None: DrawMaterialDetails(); break;
    case ItemKind::Parameter: DrawParameterDetails(selected_.parameter); break;
    case ItemKind::Node: DrawNodeDetails(selected_.node); break;
    }
}

void MaterialEditor::DrawMaterialDetails() {
    const Material& m = doc_.Get();
    if (m.is_function) {
        ImGui::SeparatorText("Material Function");
        std::string description = m.description;
        if (ImGui::InputTextMultiline("Description", &description, ImVec2(-1, 80))) {
            doc_.Edit("Edit description", [&](Material& x) { x.description = description; }, "description");
        }
        return;
    }
    ImGui::SeparatorText("Material");
    static const char* kShading[] = {"Default Lit", "Unlit"};
    int shading = static_cast<int>(m.shading);
    if (ImGui::Combo("Shading", &shading, kShading, 2)) {
        doc_.Edit("Change shading", [&](Material& x) { x.shading = static_cast<mat::ShadingModel>(shading); });
    }
    static const char* kBlend[] = {"Opaque", "Masked", "Translucent", "Additive"};
    int blend = static_cast<int>(m.blend);
    if (ImGui::Combo("Blend Mode", &blend, kBlend, 4)) {
        doc_.Edit("Change blend mode", [&](Material& x) { x.blend = static_cast<mat::BlendMode>(blend); });
    }
    bool two_sided = m.two_sided;
    if (ImGui::Checkbox("Two Sided", &two_sided)) doc_.Edit("Toggle two sided", [&](Material& x) { x.two_sided = two_sided; });
    if (m.blend == mat::BlendMode::Masked) {
        float clip = m.opacity_mask_clip;
        if (ImGui::SliderFloat("Mask Clip", &clip, 0.0f, 1.0f)) {
            doc_.Edit("Change mask clip", [&](Material& x) { x.opacity_mask_clip = clip; }, "clip");
        }
    }
    ImGui::TextDisabled("Select a node or parameter to edit it.");
}

void MaterialEditor::DrawParameterDetails(const std::string& name) {
    const mat::Parameter* p = doc_.Get().FindParameter(name);
    if (p == nullptr) return;
    ImGui::SeparatorText("Parameter");
    if (ImGui::InputText("Name", &name_edit_, ImGuiInputTextFlags_EnterReturnsTrue) || (ImGui::IsItemDeactivatedAfterEdit())) {
        if (name_edit_ != name && !RenameParameter(name, name_edit_)) name_edit_ = name;
        return; // the parameter may have a new name now
    }
    PinType type = p->type;
    if (TypeCombo("Type", type, kAllTypes)) {
        doc_.SetParameterType(name, type);
        return;
    }
    if (p->type == PinType::Texture) {
        std::string texture = p->texture;
        if (ImGui::InputText("Default Texture", &texture)) {
            doc_.Edit("Edit default", [&](Material& m) {
                for (mat::Parameter& q : m.parameters) {
                    if (q.name == name) q.texture = texture;
                }
            }, "param_default:" + name);
        }
        ImGui::TextDisabled("The texture's asset GUID.");
    } else {
        Vec4 v = p->default_value;
        const bool color = p->type == PinType::Float3 || p->type == PinType::Float4;
        float c[4] = {v.x, v.y, v.z, v.w};
        const bool changed = color ? (p->type == PinType::Float3 ? ImGui::ColorEdit3("Default", c, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR)
                                                                 : ImGui::ColorEdit4("Default", c, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR))
                                   : DragVec("Default", v, mat::Components(p->type));
        if (color && changed) v = Vec4(c[0], c[1], c[2], c[3]);
        if (changed) {
            doc_.Edit("Edit default", [&](Material& m) {
                for (mat::Parameter& q : m.parameters) {
                    if (q.name == name) q.default_value = v;
                }
            }, "param_default:" + name);
        }
    }
    std::string group = p->group;
    if (ImGui::InputText("Group", &group)) {
        doc_.Edit("Edit group", [&](Material& m) {
            for (mat::Parameter& q : m.parameters) {
                if (q.name == name) q.group = group;
            }
        }, "param_group:" + name);
    }
    if (ImGui::Button("Add to Graph")) {
        AddParameterNode(name, view_.ScreenToCanvasX(view_.origin_x + view_.width * 0.5f), view_.ScreenToCanvasY(view_.origin_y + view_.height * 0.5f));
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
        doc_.RemoveParameter(name);
        ForgetMissing();
    }
}

void MaterialEditor::DrawNodeDetails(NodeId id) {
    const mat::Node* node = doc_.Get().Find(id);
    if (node == nullptr) return;
    std::string error;
    const std::optional<mat::NodeSignature> sig = mat::ResolveNode(doc_.Get(), *node, &error);
    ImGui::SeparatorText(sig ? sig->title.c_str() : node->type.c_str());
    ImGui::TextDisabled("%s", node->type.c_str());
    if (!sig) ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1), "%s", error.c_str());
    const std::string key = "node:" + std::to_string(id) + ":";
    auto set_config = [&](const std::string& field, const json& value, bool merge) {
        doc_.Edit("Edit " + field, [&](Material& m) { m.Find(id)->config[field] = value; }, merge ? key + field : std::string());
    };
    const std::string& t = node->type;

    // Per-type settings.
    if (t.rfind("Const.Float", 0) == 0) {
        const u32 n = sig ? mat::Components(sig->outputs[0].type) : 1;
        Vec4 v = ReadVec(node->config.value("value", json(0)), Vec4(0, 0, 0, 0));
        if (DragVec("Value", v, n)) set_config("value", VecJson(v, n), true);
    } else if (t == "Math.ComponentMask") {
        std::string channels = node->config.value("channels", "r");
        if (ImGui::InputText("Channels", &channels)) set_config("channels", channels, true);
        ImGui::TextDisabled("1 to 4 of r, g, b, a.");
    } else if (t == "Input.TexCoord") {
        int index = node->config.value("index", 0);
        if (ImGui::SliderInt("UV Set", &index, 0, 3)) set_config("index", index, true);
    } else if (t == "Math.Noise") {
        int octaves = node->config.value("octaves", 1);
        if (ImGui::SliderInt("Octaves", &octaves, 1, 8)) set_config("octaves", octaves, true);
    } else if (t == "Utility.Reroute" || t == "Function.Input" || t == "Function.Output") {
        if (t != "Utility.Reroute") {
            std::string name = node->config.value("name", t == "Function.Input" ? "In" : "Out");
            if (ImGui::InputText("Name", &name)) set_config("name", name, true);
        }
        PinType type = mat::ParseType(node->config.value("type", "float")).value_or(PinType::Float);
        if (TypeCombo("Type", type, kAllTypes)) set_config("type", mat::TypeName(type), false);
        if (type != PinType::Texture && t != "Function.Output") {
            Vec4 v = ReadVec(node->config.value("default", json(0)), Vec4(0, 0, 0, 0));
            if (DragVec("Default", v, mat::Components(type))) set_config("default", VecJson(v, mat::Components(type)), true);
        }
    } else if (t == "Custom.HLSL") {
        std::string title = node->config.value("title", "Custom");
        if (ImGui::InputText("Title", &title)) set_config("title", title, true);
        PinType output = mat::ParseType(node->config.value("output", "float")).value_or(PinType::Float);
        if (TypeCombo("Output", output, kValueTypes)) set_config("output", mat::TypeName(output), false);
        json inputs = node->config.value("inputs", json::array());
        ImGui::SeparatorText("Inputs");
        int remove = -1;
        for (usize i = 0; i < inputs.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            std::string name = inputs[i].value("name", "");
            PinType type = mat::ParseType(inputs[i].value("type", "float")).value_or(PinType::Float);
            ImGui::SetNextItemWidth(110);
            if (ImGui::InputText("##name", &name)) {
                inputs[i]["name"] = name;
                set_config("inputs", inputs, true);
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90);
            if (TypeCombo("##type", type, kAllTypes)) {
                inputs[i]["type"] = mat::TypeName(type);
                set_config("inputs", inputs, false);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) remove = static_cast<int>(i);
            ImGui::PopID();
        }
        if (remove >= 0) {
            inputs.erase(inputs.begin() + remove);
            set_config("inputs", inputs, false);
        }
        if (ImGui::SmallButton("+ Input")) {
            inputs.push_back({{"name", "in" + std::to_string(inputs.size())}, {"type", "float"}});
            set_config("inputs", inputs, false);
        }
        ImGui::SeparatorText("Code");
        std::string code = node->config.value("code", "return 0.0;");
        if (ImGui::InputTextMultiline("##code", &code, ImVec2(-1, 120), ImGuiInputTextFlags_AllowTabInput)) set_config("code", code, true);
        ImGui::TextDisabled("The body of a function of the inputs.");
    }

    // Constants for unconnected inputs.
    if (!sig) return;
    bool header = false;
    for (const mat::PinDesc& p : sig->inputs) {
        const bool linked = std::any_of(doc_.Get().links.begin(), doc_.Get().links.end(),
                                        [&](const mat::Link& l) { return l.to.node == id && l.to.pin == p.name; });
        if (linked || p.type == PinType::Texture) continue;
        if (!header) {
            ImGui::SeparatorText("Inputs");
            header = true;
        }
        if (!p.implicit.empty()) {
            ImGui::TextDisabled("%s: %s", p.name.c_str(), p.implicit == "uv0" ? "UV 0" : p.implicit.c_str());
            continue;
        }
        const u32 n = p.type == PinType::Any ? 4 : mat::Components(p.type);
        Vec4 v = ReadVec(node->defaults.value(p.name, json()), p.default_value);
        if (DragVec(p.name.c_str(), v, n)) {
            doc_.Edit("Edit " + p.name, [&](Material& m) { m.Find(id)->defaults[p.name] = VecJson(v, n); }, key + "default:" + p.name);
        }
    }
}

// --- Stats, diagnostics and HLSL ----------------------------------------------------------------

void MaterialEditor::DrawBottom() {
    const mat::GeneratedMaterial& g = doc_.Generated();
    if (!ImGui::BeginTabBar("##bottom_tabs")) return;
    auto tab = [&](const char* label, BottomTab which) {
        const ImGuiTabItemFlags flags = bottom_tab == which && ImGui::GetFrameCount() <= 2 ? ImGuiTabItemFlags_SetSelected : 0;
        const bool open = ImGui::BeginTabItem(label, nullptr, flags);
        if (open) bottom_tab = which;
        return open;
    };
    if (tab("Stats", BottomTab::Stats)) {
        if (g.ok) {
            ImGui::Text("Nodes: %u live of %zu   Instructions: %u   Textures: %zu   Parameter buffer: %u bytes", g.live_nodes,
                        doc_.Get().nodes.size(), g.instructions, g.textures.size(), g.parameters.size);
            ImGui::Text("Permutation: %016llx", static_cast<unsigned long long>(g.permutation_key));
            std::string defines;
            for (const std::string& d : g.defines) defines += d + "  ";
            ImGui::TextWrapped("%s", defines.c_str());
        } else if (doc_.Get().is_function) {
            ImGui::TextDisabled("Material functions compile inside the materials that call them.");
        } else {
            ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1), "The material has errors; see Diagnostics.");
        }
        ImGui::EndTabItem();
    }
    const std::string diag_label = "Diagnostics (" + std::to_string(g.analysis.diagnostics.size()) + ")###diagnostics";
    if (tab(diag_label.c_str(), BottomTab::Diagnostics)) {
        for (usize i = 0; i < g.analysis.diagnostics.size(); ++i) {
            const mat::Diagnostic& d = g.analysis.diagnostics[i];
            const ImVec4 color = d.severity == mat::Severity::Error ? ImVec4(0.9f, 0.35f, 0.35f, 1) : ImVec4(0.9f, 0.75f, 0.2f, 1);
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            const std::string row = d.code + "  " + d.message + "##diag" + std::to_string(i);
            if (ImGui::Selectable(row.c_str()) && d.node != 0) FocusNode(d.node);
            ImGui::PopStyleColor();
        }
        if (g.analysis.diagnostics.empty()) ImGui::TextDisabled("No problems.");
        ImGui::EndTabItem();
    }
    if (tab("HLSL", BottomTab::Hlsl)) {
        std::string text = g.ok ? g.hlsl : std::string("// Fix the errors to see the generated shader.");
        ImGui::InputTextMultiline("##hlsl", &text, ImVec2(-1, -1), ImGuiInputTextFlags_ReadOnly);
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

// --- Actions ------------------------------------------------------------------------------------

bool MaterialEditor::SaveNow() {
    std::string error;
    if (!doc_.Save(&error)) {
        status_ = "Save failed: " + error;
        return false;
    }
    status_ = "Saved " + doc_.Path().filename().string();
    return true;
}

bool MaterialEditor::RenameParameter(const std::string& from, const std::string& to) {
    std::string error;
    if (!doc_.RenameParameter(from, to, &error)) {
        status_ = error;
        return false;
    }
    if (selected_.kind == ItemKind::Parameter && selected_.parameter == from) {
        selected_.parameter = to;
        name_edit_ = to;
    }
    return true;
}

} // namespace aether::editor
