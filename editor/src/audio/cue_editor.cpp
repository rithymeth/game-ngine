#include "audio/cue_editor.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cmath>

namespace aether::editor {

using audio::CueNode;
using audio::CueNodeType;
using audio::SoundCue;

namespace {

constexpr f32 kRightWidth = 330.0f;
constexpr f32 kBottomHeight = 120.0f;

constexpr CueNodeType kTypes[] = {CueNodeType::Wave,         CueNodeType::Random, CueNodeType::Sequence, CueNodeType::Modulator,
                                  CueNodeType::Concatenator, CueNodeType::Loop,   CueNodeType::Mix,      CueNodeType::Delay};

u32 HeaderColor(CueNodeType t) {
    switch (t) {
    case CueNodeType::Wave: return IM_COL32(60, 120, 200, 255);
    case CueNodeType::Random:
    case CueNodeType::Sequence: return IM_COL32(150, 90, 190, 255);
    case CueNodeType::Modulator: return IM_COL32(200, 140, 50, 255);
    case CueNodeType::Concatenator:
    case CueNodeType::Loop: return IM_COL32(70, 160, 110, 255);
    case CueNodeType::Mix: return IM_COL32(180, 80, 80, 255);
    case CueNodeType::Delay: return IM_COL32(110, 110, 130, 255);
    }
    return IM_COL32(80, 80, 80, 255);
}

constexpr u32 kPinColor = IM_COL32(120, 200, 255, 255);

} // namespace

std::vector<std::string> CueInputPins(const CueNode& n) {
    if (n.type == CueNodeType::Wave) return {};
    if (!SoundCueDocument::IsMultiInput(n.type)) return {"in"};
    std::vector<std::string> pins;
    for (usize i = 0; i < n.children.size(); ++i) pins.push_back("in" + std::to_string(i));
    pins.push_back("add");
    return pins;
}

i32 CueInputIndex(const CueNode& n, const std::string& pin) {
    if (n.type == CueNodeType::Wave) return -1;
    if (!SoundCueDocument::IsMultiInput(n.type)) return pin == "in" ? 0 : -1;
    if (pin == "add") return static_cast<i32>(n.children.size());
    if (pin.size() < 3 || pin.compare(0, 2, "in") != 0) return -1;
    const std::string digits = pin.substr(2);
    if (!std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) return -1;
    const i32 i = std::stoi(digits);
    return i < static_cast<i32>(n.children.size()) ? i : -1;
}

std::string CueNodeTitle(const CueNode& n) {
    switch (n.type) {
    case CueNodeType::Wave: return "Wave: " + (n.sound.empty() ? std::string("(none)") : n.sound) + (n.looping ? " (loop)" : "");
    case CueNodeType::Loop: return n.count == 0 ? "Loop forever" : "Loop x" + std::to_string(n.count);
    case CueNodeType::Random: return n.no_repeat ? "Random (no repeats)" : "Random";
    default: return audio::CueNodeTypeName(n.type);
    }
}

GraphViewModel BuildCueGraphView(const SoundCue& cue, const std::vector<audio::CueDiagnostic>& diagnostics, f32 output_x, f32 output_y) {
    GraphViewModel m;
    for (const CueNode& n : cue.nodes) {
        GraphNodeView v;
        v.id = n.id;
        v.title = CueNodeTitle(n);
        v.header_color = HeaderColor(n.type);
        v.x = n.x, v.y = n.y;
        const std::vector<std::string> pins = CueInputPins(n);
        for (usize i = 0; i < pins.size(); ++i) {
            GraphPinView p;
            p.name = pins[i];
            p.color = kPinColor;
            if (pins[i] == "add") {
                p.label = "+";
            } else if (SoundCueDocument::IsMultiInput(n.type)) {
                p.label = std::to_string(i + 1);
                if (n.type == CueNodeType::Random && i < n.weights.size()) p.label += "  w" + std::to_string(static_cast<int>(std::lround(n.weights[i] * 100))) + "%";
            }
            p.connected = i < n.children.size();
            v.inputs.push_back(p);
        }
        GraphPinView out;
        out.name = "out";
        out.color = kPinColor;
        out.connected = n.id == cue.root || std::any_of(cue.nodes.begin(), cue.nodes.end(), [&](const CueNode& o) {
                            return std::find(o.children.begin(), o.children.end(), n.id) != o.children.end();
                        });
        v.outputs.push_back(out);
        for (const audio::CueDiagnostic& d : diagnostics) {
            if (d.node == n.id && d.error) {
                v.error = d.code + ": " + d.message;
                break;
            }
        }
        m.nodes.push_back(std::move(v));
        for (usize i = 0; i < n.children.size() && i < pins.size(); ++i) m.links.push_back({n.children[i], "out", n.id, pins[i], kPinColor, 0.0f});
    }
    GraphNodeView output;
    output.id = kCueOutputNode;
    output.title = "Output";
    output.header_color = IM_COL32(200, 60, 60, 255);
    output.x = output_x, output.y = output_y;
    GraphPinView sound;
    sound.name = "sound";
    sound.label = "Sound";
    sound.color = kPinColor;
    sound.connected = cue.Find(cue.root) != nullptr;
    output.inputs.push_back(sound);
    m.nodes.push_back(std::move(output));
    if (cue.Find(cue.root) != nullptr) m.links.push_back({cue.root, "out", kCueOutputNode, "sound", kPinColor, 0.0f});
    return m;
}

std::vector<std::string> ApplyCueGraphEdits(SoundCueDocument& doc, const GraphViewResult& r, f32& output_x, f32& output_y) {
    std::vector<std::string> errors;
    if (!r.moved.empty()) {
        doc.Edit("Move", [&](SoundCue& c) {
            for (const auto& [id, pos] : r.moved) {
                if (CueNode* n = c.Find(id)) n->x = pos.first, n->y = pos.second;
            }
        });
        for (const auto& [id, pos] : r.moved) {
            if (id == kCueOutputNode) output_x = pos.first, output_y = pos.second;
        }
    }
    if (r.disconnect) {
        if (r.disconnected.to_node == kCueOutputNode) {
            doc.SetRoot(0);
        } else if (const CueNode* n = doc.Get().Find(r.disconnected.to_node)) {
            const i32 i = CueInputIndex(*n, r.disconnected.to_pin);
            if (i >= 0) doc.Disconnect(n->id, static_cast<usize>(i));
        }
    }
    if (r.connect) {
        const GraphPinRef& from = r.connect_from.output ? r.connect_from : r.connect_to;
        const GraphPinRef& to = r.connect_from.output ? r.connect_to : r.connect_from;
        std::string error;
        if (from.output == to.output) {
            errors.push_back("Link an output to an input.");
        } else if (to.node == kCueOutputNode) {
            if (!doc.SetRoot(from.node)) errors.push_back("That node no longer exists.");
        } else if (const CueNode* n = doc.Get().Find(to.node)) {
            const i32 i = CueInputIndex(*n, to.pin);
            if (i < 0 || !doc.Connect(from.node, to.node, static_cast<usize>(i), &error)) errors.push_back(error.empty() ? "That input doesn't exist." : error);
        }
    }
    std::vector<u32> doomed;
    for (u32 id : r.deleted) {
        if (id != kCueOutputNode) doomed.push_back(id);
    }
    doc.DeleteNodes(doomed);
    return errors;
}

std::vector<f32> AttenuationCurve(const audio::AttenuationSettings& s, u32 samples, f32 range) {
    std::vector<f32> out;
    for (u32 i = 0; i < samples; ++i) out.push_back(audio::Attenuate(s, range * static_cast<f32>(i) / static_cast<f32>(std::max(samples - 1, 1u))));
    return out;
}

// --- The editor -----------------------------------------------------------------------------------

SoundCueEditor::SoundCueEditor(SoundCueDocument& document) : doc_(document) { PlaceOutput(); }

void SoundCueEditor::PlaceOutput() {
    // To the right of the nodes, level with the one the output plays.
    f32 right = 0.0f, y = 0.0f;
    for (const CueNode& n : doc_.Get().nodes) right = std::max(right, n.x);
    if (const CueNode* root = doc_.Get().Find(doc_.Get().root)) y = root->y;
    output_x_ = right + 260.0f;
    output_y_ = y;
}

void SoundCueEditor::Select(u32 id) {
    selected_ = id;
    view_.selection.clear();
    if (id != 0) view_.selection.insert(id);
}

bool SoundCueEditor::PlayPreview() {
    if (player_ == nullptr) return false;
    StopPreview();
    if (doc_.ErrorCount() > 0) {
        status_ = "Fix the errors to preview.";
        return false;
    }
    preview_ = player_->Play(doc_.Get());
    if (preview_ == 0) status_ = "The cue plays nothing.";
    return preview_ != 0;
}

void SoundCueEditor::StopPreview() {
    if (player_ != nullptr && preview_ != 0) player_->Stop(preview_, 0.02f);
    preview_ = 0;
}

bool SoundCueEditor::Previewing() const { return player_ != nullptr && preview_ != 0 && player_->IsPlaying(preview_); }

void SoundCueEditor::OpenPalette(f32 x, f32 y) {
    palette_open_ = palette_popup_ = true;
    palette_x_ = x, palette_y_ = y;
    palette_filter_.clear();
}

u32 SoundCueEditor::PlaceNode(CueNodeType type, const std::string& sound) {
    const u32 id = doc_.AddNode(type, palette_x_, palette_y_, sound);
    palette_open_ = false;
    Select(id);
    return id;
}

bool SoundCueEditor::SaveNow() {
    std::string error;
    if (!doc_.Save(&error)) {
        status_ = "Save failed: " + error;
        return false;
    }
    status_ = "Saved " + doc_.Path().filename().string();
    return true;
}

void SoundCueEditor::Draw() {
    ImGui::PushID(this);
    if (player_ != nullptr) player_->Update();
    if (selected_ != 0 && selected_ != kCueOutputNode && doc_.Get().Find(selected_) == nullptr) Select(0); // undone or deleted
    HandleKeys();
    DrawToolbar();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const f32 top = std::max(100.0f, avail.y - kBottomHeight - ImGui::GetStyle().ItemSpacing.y);
    const f32 center = std::max(200.0f, avail.x - kRightWidth - ImGui::GetStyle().ItemSpacing.x);
    ImGui::BeginChild("##graph", ImVec2(center, top), ImGuiChildFlags_None);
    DrawGraph();
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

void SoundCueEditor::HandleKeys() {
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || ImGui::GetIO().WantTextInput) return;
    const bool ctrl = ImGui::GetIO().KeyCtrl;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) doc_.Undo();
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) doc_.Redo();
    else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) SaveNow();
    else if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) Previewing() ? StopPreview() : (void)PlayPreview();
}

void SoundCueEditor::DrawToolbar() {
    if (ImGui::Button(doc_.Dirty() ? "Save*" : "Save")) SaveNow();
    ImGui::SameLine();
    ImGui::BeginDisabled(!doc_.CanUndo());
    if (ImGui::Button("Undo")) doc_.Undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!doc_.CanRedo());
    if (ImGui::Button("Redo")) doc_.Redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(player_ == nullptr);
    if (ImGui::Button(Previewing() ? "Stop" : "Play")) Previewing() ? StopPreview() : (void)PlayPreview();
    ImGui::EndDisabled();
    ImGui::SameLine();
    const usize errors = doc_.ErrorCount();
    if (errors > 0) ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1), "%zu error(s)", errors);
    else ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1), "OK");
    ImGui::SameLine();
    ImGui::TextDisabled("|  %s", doc_.Name().c_str());
    if (!status_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("  %s", status_.c_str());
    }
}

void SoundCueEditor::DrawGraph() {
    const GraphViewModel model = BuildCueGraphView(doc_.Get(), doc_.Diagnostics(), output_x_, output_y_);
    const GraphViewResult r = DrawGraphView("##cue_graph", model, view_);
    const std::vector<std::string> errors = ApplyCueGraphEdits(doc_, r, output_x_, output_y_);
    if (!errors.empty()) status_ = errors.front();
    if (r.open_palette) OpenPalette(r.palette_x, r.palette_y);
    if (r.selection_changed || !r.deleted.empty()) selected_ = view_.selection.size() == 1 ? *view_.selection.begin() : 0;
}

void SoundCueEditor::DrawDetails() {
    if (selected_ == 0 || selected_ == kCueOutputNode) {
        DrawOutputDetails();
        return;
    }
    if (const CueNode* n = doc_.Get().Find(selected_)) DrawNodeDetails(*n);
}

void SoundCueEditor::DrawNodeDetails(const CueNode& node) {
    const u32 id = node.id;
    const std::string key = "node" + std::to_string(id) + ".";
    auto edit = [&](const char* label, const char* field, const std::function<void(CueNode&)>& change) {
        doc_.Edit(label, [&](SoundCue& c) {
            if (CueNode* n = c.Find(id)) change(*n);
        }, key + field);
    };
    ImGui::SeparatorText(CueNodeTitle(node).c_str());
    ImGui::TextDisabled("%s node %u", audio::CueNodeTypeName(node.type), id);
    if (doc_.Get().root != id && ImGui::Button("Make Output")) doc_.SetRoot(id);
    switch (node.type) {
    case CueNodeType::Wave: {
        std::string sound = node.sound;
        if (!doc_.Sounds().empty()) {
            if (ImGui::BeginCombo("Sound", sound.empty() ? "(none)" : sound.c_str())) {
                for (const std::string& s : doc_.Sounds()) {
                    if (ImGui::Selectable(s.c_str(), s == sound)) edit("Set sound", "sound", [&](CueNode& n) { n.sound = s; });
                }
                ImGui::EndCombo();
            }
        } else if (ImGui::InputText("Sound", &sound, ImGuiInputTextFlags_EnterReturnsTrue)) {
            edit("Set sound", "sound", [&](CueNode& n) { n.sound = sound; });
        }
        bool looping = node.looping;
        if (ImGui::Checkbox("Looping", &looping)) edit("Looping", "looping", [&](CueNode& n) { n.looping = looping; });
        break;
    }
    case CueNodeType::Random: {
        bool no_repeat = node.no_repeat;
        if (ImGui::Checkbox("Never twice in a row", &no_repeat)) edit("No repeats", "no_repeat", [&](CueNode& n) { n.no_repeat = no_repeat; });
        for (usize i = 0; i < node.children.size(); ++i) {
            f32 w = i < node.weights.size() ? node.weights[i] : 1.0f;
            if (ImGui::DragFloat(("Weight " + std::to_string(i + 1)).c_str(), &w, 0.01f, 0.0f, 100.0f)) {
                edit("Weight", ("w" + std::to_string(i)).c_str(), [&](CueNode& n) {
                    n.weights.resize(std::max(n.weights.size(), i + 1), 1.0f);
                    n.weights[i] = std::max(w, 0.0f);
                });
            }
        }
        break;
    }
    case CueNodeType::Modulator: {
        f32 v0 = node.volume_min_db, v1 = node.volume_max_db, p0 = node.pitch_min, p1 = node.pitch_max;
        if (ImGui::DragFloatRange2("Volume (dB)", &v0, &v1, 0.1f, -60.0f, 12.0f)) {
            edit("Volume range", "volume", [&](CueNode& n) { n.volume_min_db = v0, n.volume_max_db = v1; });
        }
        if (ImGui::DragFloatRange2("Pitch", &p0, &p1, 0.005f, 0.1f, 4.0f)) edit("Pitch range", "pitch", [&](CueNode& n) { n.pitch_min = p0, n.pitch_max = p1; });
        break;
    }
    case CueNodeType::Loop: {
        int count = static_cast<int>(node.count);
        if (ImGui::InputInt("Count (0 = forever)", &count)) edit("Loop count", "count", [&](CueNode& n) { n.count = static_cast<u32>(std::max(count, 0)); });
        break;
    }
    case CueNodeType::Mix:
        for (usize i = 0; i < node.children.size(); ++i) {
            f32 db = i < node.input_db.size() ? node.input_db[i] : 0.0f;
            if (ImGui::DragFloat(("Input " + std::to_string(i + 1) + " (dB)").c_str(), &db, 0.1f, -60.0f, 12.0f)) {
                edit("Input volume", ("db" + std::to_string(i)).c_str(), [&](CueNode& n) {
                    n.input_db.resize(std::max(n.input_db.size(), i + 1), 0.0f);
                    n.input_db[i] = db;
                });
            }
        }
        break;
    case CueNodeType::Delay: {
        f32 d0 = node.delay_min, d1 = node.delay_max;
        if (ImGui::DragFloatRange2("Delay (s)", &d0, &d1, 0.005f, 0.0f, 10.0f)) edit("Delay", "delay", [&](CueNode& n) { n.delay_min = d0, n.delay_max = d1; });
        break;
    }
    case CueNodeType::Sequence: ImGui::TextWrapped("Plays its inputs in turn, one each time the cue plays."); break;
    case CueNodeType::Concatenator: ImGui::TextWrapped("Plays its inputs one after another."); break;
    }
    if (!node.children.empty()) {
        ImGui::SeparatorText("Inputs");
        for (usize i = 0; i < node.children.size(); ++i) {
            const CueNode* c = doc_.Get().Find(node.children[i]);
            ImGui::BulletText("%zu: %s", i + 1, c != nullptr ? CueNodeTitle(*c).c_str() : "(missing)");
        }
    }
}

void SoundCueEditor::DrawOutputDetails() {
    const SoundCue& c = doc_.Get();
    auto edit = [&](const char* label, const char* field, const std::function<void(SoundCue&)>& change) { doc_.Edit(label, change, std::string("output.") + field); };
    ImGui::SeparatorText("Output");
    std::string name = c.name;
    if (ImGui::InputText("Name", &name, ImGuiInputTextFlags_EnterReturnsTrue)) edit("Rename", "name", [&](SoundCue& s) { s.name = name; });
    f32 volume = c.volume_db, pitch = c.pitch;
    if (ImGui::DragFloat("Volume (dB)", &volume, 0.1f, -60.0f, 12.0f)) edit("Volume", "volume", [&](SoundCue& s) { s.volume_db = volume; });
    if (ImGui::DragFloat("Pitch", &pitch, 0.005f, 0.1f, 4.0f)) edit("Pitch", "pitch", [&](SoundCue& s) { s.pitch = std::max(pitch, 0.01f); });
    std::string bus = c.bus;
    if (ImGui::InputText("Bus", &bus, ImGuiInputTextFlags_EnterReturnsTrue)) edit("Bus", "bus", [&](SoundCue& s) { s.bus = bus; });
    int priority = c.priority;
    if (ImGui::SliderInt("Priority", &priority, 0, 255)) edit("Priority", "priority", [&](SoundCue& s) { s.priority = static_cast<u8>(priority); });
    const char* modes[] = {"Continue", "Restart", "Stop"};
    int mode = static_cast<int>(c.virtual_mode);
    if (ImGui::Combo("When virtual", &mode, modes, 3)) edit("Virtual mode", "virtual", [&](SoundCue& s) { s.virtual_mode = static_cast<audio::VirtualMode>(mode); });

    ImGui::SeparatorText("3D");
    bool spatial = c.spatial;
    if (ImGui::Checkbox("Spatial", &spatial)) edit("Spatial", "spatial", [&](SoundCue& s) { s.spatial = spatial; });
    if (!c.spatial) return;
    f32 blend = c.spatial_blend, doppler = c.doppler;
    if (ImGui::SliderFloat("Spatial blend", &blend, 0.0f, 1.0f)) edit("Spatial blend", "blend", [&](SoundCue& s) { s.spatial_blend = blend; });
    if (ImGui::SliderFloat("Doppler", &doppler, 0.0f, 4.0f)) edit("Doppler", "doppler", [&](SoundCue& s) { s.doppler = doppler; });
    bool occlusion = c.occlusion;
    if (ImGui::Checkbox("Occlusion", &occlusion)) edit("Occlusion", "occlusion", [&](SoundCue& s) { s.occlusion = occlusion; });
    const audio::AttenuationSettings& a = c.attenuation;
    const char* models[] = {"None", "Inverse", "Linear", "Logarithmic", "Custom"};
    int model = static_cast<int>(a.model);
    if (ImGui::Combo("Attenuation", &model, models, 5)) {
        edit("Attenuation", "model", [&](SoundCue& s) {
            s.attenuation.model = static_cast<audio::AttenuationModel>(model);
            if (s.attenuation.model == audio::AttenuationModel::Custom && s.attenuation.curve.empty()) s.attenuation.curve = {{0.0f, 1.0f}, {1.0f, 0.0f}};
        });
    }
    f32 lo = a.min_distance, hi = a.max_distance;
    if (ImGui::DragFloatRange2("Distance (m)", &lo, &hi, 0.1f, 0.0f, 10000.0f)) {
        edit("Distances", "distance", [&](SoundCue& s) { s.attenuation.min_distance = lo, s.attenuation.max_distance = std::max(hi, lo); });
    }
    if (a.model == audio::AttenuationModel::Inverse) {
        f32 rolloff = a.rolloff;
        if (ImGui::DragFloat("Rolloff", &rolloff, 0.01f, 0.0f, 10.0f)) edit("Rolloff", "rolloff", [&](SoundCue& s) { s.attenuation.rolloff = rolloff; });
    }
    f32 lowpass = a.lowpass_at_max_hz;
    if (ImGui::DragFloat("Air absorption (Hz at max)", &lowpass, 10.0f, 0.0f, 20000.0f, lowpass <= 0.0f ? "off" : "%.0f Hz")) {
        edit("Air absorption", "lowpass", [&](SoundCue& s) { s.attenuation.lowpass_at_max_hz = std::max(lowpass, 0.0f); });
    }
    if (a.model == audio::AttenuationModel::Custom) {
        for (usize i = 0; i < a.curve.size(); ++i) {
            f32 point[2] = {a.curve[i].first, a.curve[i].second};
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::DragFloat2("##point", point, 0.005f, 0.0f, 1.0f)) {
                edit("Curve point", ("curve" + std::to_string(i)).c_str(), [&](SoundCue& s) {
                    s.attenuation.curve[i] = {point[0], point[1]};
                    std::sort(s.attenuation.curve.begin(), s.attenuation.curve.end());
                });
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) edit("Remove point", "curve-", [&](SoundCue& s) { s.attenuation.curve.erase(s.attenuation.curve.begin() + static_cast<std::ptrdiff_t>(i)); });
            ImGui::PopID();
        }
        if (ImGui::SmallButton("+ Point")) edit("Add point", "curve+", [&](SoundCue& s) {
            s.attenuation.curve.push_back({0.5f, 0.5f});
            std::sort(s.attenuation.curve.begin(), s.attenuation.curve.end());
        });
    }
    // The curve out to a little past the max distance.
    const f32 range = std::max(a.max_distance * 1.2f, 1.0f);
    const std::vector<f32> curve = AttenuationCurve(a, 64, range);
    ImGui::PlotLines("##falloff", curve.data(), static_cast<int>(curve.size()), 0, nullptr, 0.0f, 1.0f, ImVec2(-1, 80));
    ImGui::TextDisabled("0 m .. %.0f m", range);
}

void SoundCueEditor::DrawDiagnostics() {
    const auto& d = doc_.Diagnostics();
    ImGui::TextDisabled("Diagnostics (%zu)", d.size());
    for (usize i = 0; i < d.size(); ++i) {
        ImGui::PushStyleColor(ImGuiCol_Text, d[i].error ? ImVec4(0.9f, 0.35f, 0.35f, 1) : ImVec4(0.9f, 0.75f, 0.2f, 1));
        if (ImGui::Selectable((d[i].code + "  " + d[i].message + "##diag" + std::to_string(i)).c_str()) && doc_.Get().Find(d[i].node) != nullptr) {
            Select(d[i].node);
            view_.request_fit = view_.request_fit_selection = true;
        }
        ImGui::PopStyleColor();
    }
}

void SoundCueEditor::DrawPalette() {
    if (!palette_open_) return;
    if (palette_popup_) {
        ImGui::SetNextWindowPos(ImVec2(view_.CanvasToScreenX(palette_x_), view_.CanvasToScreenY(palette_y_)), ImGuiCond_Always);
        ImGui::OpenPopup("##cue_palette");
        palette_popup_ = false;
    }
    if (!ImGui::BeginPopup("##cue_palette")) {
        palette_open_ = false;
        return;
    }
    ImGui::InputTextWithHint("##filter", "Search", &palette_filter_);
    // Node types, then a Wave for each sound.
    for (CueNodeType t : kTypes) {
        const char* name = audio::CueNodeTypeName(t);
        if (!palette_filter_.empty() && FuzzyScore(palette_filter_, name) == 0) continue;
        if (ImGui::Selectable(name)) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            PlaceNode(t);
            return;
        }
    }
    for (const std::string& s : doc_.Sounds()) {
        if (!palette_filter_.empty() && FuzzyScore(palette_filter_, s) == 0) continue;
        if (ImGui::Selectable(("Wave: " + s).c_str())) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            PlaceNode(CueNodeType::Wave, s);
            return;
        }
    }
    ImGui::EndPopup();
}

} // namespace aether::editor
