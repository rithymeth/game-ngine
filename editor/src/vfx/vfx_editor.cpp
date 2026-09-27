#include "vfx/vfx_editor.h"

#include "core/json_edit.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace aether::editor {

using nlohmann::json;
using namespace vfx;

namespace {

constexpr f32 kLeftWidth = 200.0f;
constexpr f32 kStackWidth = 260.0f;
constexpr f32 kBottomHeight = 260.0f;
constexpr VfxStage kStages[] = {VfxStage::Spawn, VfxStage::Init, VfxStage::Update, VfxStage::Render};

const JsonEnums& Enums() {
    static const JsonEnums e = {
        {"shape", {"Point", "Sphere", "Hemisphere", "Box", "Cone", "Circle", "Edge"}},
        {"mode", {"Cone", "Radial", "Direction"}},
        {"blend", {"Alpha", "Additive", "Premultiplied", "Opaque"}},
        {"sort", {"None", "BackToFront", "FrontToBack", "OldestFirst", "NewestFirst"}},
        {"facing", {"Camera", "CameraPosition", "Velocity", "FixedAxis", "FixedPlane"}},
        {"flipbook", {"OverLife", "Rate", "Random"}},
        {"orientation", {"Rotation", "AlignVelocity", "FaceCamera"}},
        {"uv", {"Stretch", "Distance"}},
        {"space", {"World", "Local"}},
        {"target", {"Auto", "Cpu", "Gpu"}},
    };
    return e;
}

// Fields that are FloatRanges (a number when min == max): always edited as min/max.
bool IsRange(const std::string& key) {
    static const std::set<std::string> ranges = {"count", "seconds", "speed", "size", "angle", "spin"};
    return ranges.count(key) > 0;
}

ImU32 Col(const LinearColor& c) {
    auto b = [](f32 v) { return static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return IM_COL32(b(c.r), b(c.g), b(c.b), b(c.a));
}

// A curve: a plot to click (add), drag (move) and right-click (remove) keys on, and the keys as numbers.
bool CurveEditor(const char* id, FloatCurve& curve, bool& dragging) {
    bool changed = false;
    ImGui::PushID(id);
    ImGui::TextUnformatted(id);
    const char* interps[] = {"Linear", "Smooth", "Constant"};
    int interp = static_cast<int>(curve.interp);
    if (ImGui::Combo("Interpolation", &interp, interps, 3)) curve.interp = static_cast<CurveInterp>(interp), changed = true;
    // The plot, framed on the keys' values.
    f32 lo = 0.0f, hi = 1.0f;
    for (const auto& k : curve.keys) lo = std::min(lo, k.value), hi = std::max(hi, k.value);
    const ImVec2 size(ImGui::GetContentRegionAvail().x, 90.0f);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("plot", ImVec2(std::max(size.x, 40.0f), size.y), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(at, ImVec2(at.x + size.x, at.y + size.y), IM_COL32(30, 30, 36, 255));
    auto to_screen = [&](f32 t, f32 v) { return ImVec2(at.x + t * size.x, at.y + (1.0f - (v - lo) / std::max(hi - lo, 1e-6f)) * size.y); };
    ImVec2 prev = to_screen(0, curve.Evaluate(0));
    for (int s = 1; s <= 64; ++s) {
        const f32 t = static_cast<f32>(s) / 64.0f;
        const ImVec2 p = to_screen(t, curve.Evaluate(t));
        dl->AddLine(prev, p, IM_COL32(120, 200, 255, 255), 1.5f);
        prev = p;
    }
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    int hit = -1;
    for (usize i = 0; i < curve.keys.size(); ++i) {
        const ImVec2 p = to_screen(curve.keys[i].time, curve.keys[i].value);
        dl->AddCircleFilled(p, 4.0f, IM_COL32(255, 220, 120, 255));
        if (std::abs(mouse.x - p.x) < 6 && std::abs(mouse.y - p.y) < 6) hit = static_cast<int>(i);
    }
    const f32 mt = std::clamp((mouse.x - at.x) / std::max(size.x, 1.0f), 0.0f, 1.0f);
    const f32 mv = lo + (1.0f - (mouse.y - at.y) / size.y) * (hi - lo);
    static int drag_key = -1;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        if (hit >= 0) drag_key = hit;
        else changed = true, drag_key = static_cast<int>(AddCurveKey(curve, mt, mv));
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && hit >= 0) changed |= RemoveCurveKey(curve, static_cast<usize>(hit));
    if (drag_key >= 0 && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && static_cast<usize>(drag_key) < curve.keys.size()) {
        drag_key = static_cast<int>(MoveCurveKey(curve, static_cast<usize>(drag_key), mt, mv));
        changed = dragging = true;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) drag_key = -1;
    for (usize i = 0; i < curve.keys.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        f32 kv[2] = {curve.keys[i].time, curve.keys[i].value};
        if (ImGui::DragFloat2("key", kv, 0.01f)) {
            (void)MoveCurveKey(curve, i, kv[0], kv[1]);
            changed = true, dragging = ImGui::IsMouseDragging(0);
            ImGui::PopID();
            break; // the order may have changed
        }
        ImGui::PopID();
    }
    ImGui::PopID();
    return changed;
}

// A gradient: a bar showing it, colour keys and alpha keys as rows.
bool GradientEditor(const char* id, ColorGradient& g, bool& dragging) {
    bool changed = false;
    ImGui::PushID(id);
    ImGui::TextUnformatted(id);
    const ImVec2 size(ImGui::GetContentRegionAvail().x, 18.0f);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int s = 0; s < 32; ++s) {
        const f32 t0 = static_cast<f32>(s) / 32.0f, t1 = static_cast<f32>(s + 1) / 32.0f;
        dl->AddRectFilledMultiColor(ImVec2(at.x + t0 * size.x, at.y), ImVec2(at.x + t1 * size.x, at.y + size.y), Col(g.Evaluate(t0)), Col(g.Evaluate(t1)),
                                    Col(g.Evaluate(t1)), Col(g.Evaluate(t0)));
    }
    for (usize i = 0; i < g.colors.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        f32 rgb[3] = {g.colors[i].r, g.colors[i].g, g.colors[i].b};
        if (ImGui::ColorEdit3("##c", rgb, ImGuiColorEditFlags_NoInputs)) g.colors[i].r = rgb[0], g.colors[i].g = rgb[1], g.colors[i].b = rgb[2], changed = true;
        ImGui::SameLine();
        f32 t = g.colors[i].time;
        ImGui::SetNextItemWidth(80);
        if (ImGui::DragFloat("time", &t, 0.01f, 0.0f, 1.0f)) {
            (void)MoveColorKey(g, i, t);
            changed = true, dragging = ImGui::IsMouseDragging(0);
            ImGui::PopID();
            break;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x") && RemoveColorKey(g, i)) {
            changed = true;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (ImGui::SmallButton("+ colour")) AddColorKey(g, 0.5f, g.Evaluate(0.5f)), changed = true;
    for (usize i = 0; i < g.alphas.size(); ++i) {
        ImGui::PushID(1000 + static_cast<int>(i));
        f32 ta[2] = {g.alphas[i].time, g.alphas[i].a};
        if (ImGui::DragFloat2("alpha (time, a)", ta, 0.01f, 0.0f, 1.0f)) {
            g.alphas.erase(g.alphas.begin() + static_cast<std::ptrdiff_t>(i));
            (void)AddAlphaKey(g, ta[0], ta[1]);
            changed = true, dragging = ImGui::IsMouseDragging(0);
            ImGui::PopID();
            break;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x") && RemoveAlphaKey(g, i)) {
            changed = true;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (ImGui::SmallButton("+ alpha")) AddAlphaKey(g, 0.5f, g.Evaluate(0.5f).a), changed = true;
    ImGui::PopID();
    return changed;
}

} // namespace

ParticleEditor::ParticleEditor(ParticleSystemDocument& document) : doc_(document) {}

void ParticleEditor::SelectEmitter(usize i) {
    emitter_ = static_cast<i64>(i);
    module_.reset();
}

void ParticleEditor::SelectModule(const ModuleRef& ref) {
    emitter_ = static_cast<i64>(ref.emitter);
    module_ = ref;
}

void ParticleEditor::SetTime(f32 time) {
    preview_.Seek(doc_.Get(), time);
    preview_revision_ = doc_.Revision();
}

bool ParticleEditor::SaveNow() {
    std::string error;
    if (doc_.Save(&error)) {
        status_ = "Saved " + doc_.Path().filename().string();
        return true;
    }
    status_ = error;
    return false;
}

void ParticleEditor::Sync() {
    // Selections that went away (undo, removal).
    const i64 n = static_cast<i64>(doc_.Get().emitters.size());
    if (emitter_ >= n) emitter_ = n - 1;
    if (module_ && (module_->emitter >= static_cast<usize>(std::max<i64>(n, 0)) || module_->index >= doc_.ModuleCount(module_->emitter, module_->stage))) module_.reset();
    // Edits show at once, at the same moment of the preview.
    if (preview_revision_ != doc_.Revision()) {
        preview_.Seek(doc_.Get(), preview_.Instance() != nullptr ? preview_.Time() : 0.0f);
        preview_revision_ = doc_.Revision();
    }
}

void ParticleEditor::Draw() {
    Sync();
    if (playing_) {
        preview_.Advance(ImGui::GetIO().DeltaTime * std::max(speed, 0.0f));
        if (loop && loop_length > 0.0f && preview_.Time() >= loop_length) preview_.Reset(doc_.Get());
    }
    DrawToolbar();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const f32 top_h = std::max(120.0f, avail.y - kBottomHeight);
    ImGui::BeginChild("emitters", ImVec2(kLeftWidth, top_h), ImGuiChildFlags_Borders);
    DrawEmitters();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("stack", ImVec2(kStackWidth, top_h), ImGuiChildFlags_Borders);
    DrawStack();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("details", ImVec2(0, top_h), ImGuiChildFlags_Borders);
    DrawDetails();
    ImGui::EndChild();
    ImGui::BeginChild("bottom", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Preview")) {
            DrawPreview();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Parameters")) {
            DrawParameters();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Stats")) {
            DrawStats();
            ImGui::EndTabItem();
        }
        const std::string diag = "Diagnostics (" + std::to_string(doc_.Diagnostics().size()) + ")###diag";
        if (ImGui::BeginTabItem(diag.c_str())) {
            DrawDiagnostics();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    HandleKeys();
}

void ParticleEditor::DrawToolbar() {
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
    if (ImGui::Button(playing_ ? "Pause" : "Play")) playing_ = !playing_;
    ImGui::SameLine();
    if (ImGui::Button("Restart")) SetTime(0.0f);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    f32 t = preview_.Time();
    if (ImGui::SliderFloat("##time", &t, 0.0f, std::max(loop_length, 0.1f), "%.2f s")) SetTime(t);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    ImGui::DragFloat("Speed", &speed, 0.01f, 0.0f, 4.0f, "%.2fx");
    ImGui::SameLine();
    ImGui::Checkbox("Loop", &loop);
    ImGui::SameLine();
    ImGui::TextDisabled("%s%s  %zu particles  %s", doc_.Name().c_str(), doc_.Dirty() ? "*" : "", preview_.Count(), status_.c_str());
}

void ParticleEditor::DrawEmitters() {
    ImGui::TextUnformatted("Emitters");
    const auto& emitters = doc_.Get().emitters;
    for (usize i = 0; i < emitters.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        bool on = emitters[i].settings.enabled;
        if (ImGui::Checkbox("##on", &on)) doc_.SetEmitterSetting(i, "enabled", on);
        ImGui::SameLine();
        if (ImGui::Selectable(emitters[i].settings.name.c_str(), emitter_ == static_cast<i64>(i) && !module_)) SelectEmitter(i);
        ImGui::PopID();
    }
    if (ImGui::Button("Add")) SelectEmitter(doc_.AddEmitter("Emitter"));
    ImGui::SameLine();
    const bool any = emitter_ >= 0 && static_cast<usize>(emitter_) < emitters.size();
    ImGui::BeginDisabled(!any);
    if (ImGui::Button("Duplicate")) {
        if (const auto copy = doc_.DuplicateEmitter(static_cast<usize>(emitter_))) SelectEmitter(*copy);
    }
    ImGui::SameLine();
    if (ImGui::Button("Remove")) {
        doc_.RemoveEmitter(static_cast<usize>(emitter_));
        module_.reset();
    }
    if (ImGui::ArrowButton("up", ImGuiDir_Up) && emitter_ > 0 && doc_.MoveEmitter(static_cast<usize>(emitter_), static_cast<usize>(emitter_ - 1))) --emitter_;
    ImGui::SameLine();
    if (ImGui::ArrowButton("down", ImGuiDir_Down) && doc_.MoveEmitter(static_cast<usize>(emitter_), static_cast<usize>(emitter_ + 1))) ++emitter_;
    ImGui::EndDisabled();
}

void ParticleEditor::DrawStack() {
    if (emitter_ < 0 || static_cast<usize>(emitter_) >= doc_.Get().emitters.size()) {
        ImGui::TextDisabled("No emitter");
        return;
    }
    const usize e = static_cast<usize>(emitter_);
    for (VfxStage stage : kStages) {
        ImGui::PushID(static_cast<int>(stage));
        if (ImGui::CollapsingHeader(StageLabel(stage), ImGuiTreeNodeFlags_DefaultOpen)) {
            for (usize i = 0; i < doc_.ModuleCount(e, stage); ++i) {
                const ModuleRef ref{e, stage, i};
                ImGui::PushID(static_cast<int>(i));
                const json m = doc_.ModuleJson(ref);
                bool on = m.value("enabled", true);
                if (ImGui::Checkbox("##on", &on)) doc_.SetModuleEnabled(ref, on);
                ImGui::SameLine();
                if (ImGui::Selectable(m.value("module", std::string()).c_str(), module_ && *module_ == ref, 0, ImVec2(120, 0))) SelectModule(ref);
                ImGui::SameLine();
                if (ImGui::SmallButton("^") && i > 0) {
                    if (const auto moved = doc_.MoveModule(ref, i - 1)) SelectModule(*moved);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("v") && i + 1 < doc_.ModuleCount(e, stage)) {
                    if (const auto moved = doc_.MoveModule(ref, i + 1)) SelectModule(*moved);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) {
                    doc_.RemoveModule(ref);
                    module_.reset();
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            if (ImGui::BeginCombo("##add", "+ Add module")) {
                for (const std::string& name : StageModuleNames(stage)) {
                    if (ImGui::Selectable(name.c_str())) {
                        std::string error;
                        if (const auto added = doc_.AddModule(e, stage, name, &error)) SelectModule(*added);
                        else status_ = error;
                    }
                }
                ImGui::EndCombo();
            }
        }
        ImGui::PopID();
    }
}

void ParticleEditor::DrawDetails() {
    if (module_) DrawModuleDetails(*module_);
    else if (emitter_ >= 0 && static_cast<usize>(emitter_) < doc_.Get().emitters.size()) DrawEmitterDetails(static_cast<usize>(emitter_));
    else ImGui::TextDisabled("Select an emitter or a module");
}

void ParticleEditor::DrawModuleDetails(const ModuleRef& ref) {
    const json m = doc_.ModuleJson(ref);
    ImGui::Text("%s  (%s)", m.value("module", std::string()).c_str(), StageLabel(ref.stage));
    for (const auto& [key, value] : m.items()) {
        if (key == "module" || key == "enabled") continue;
        ImGui::PushID(key.c_str());
        bool dragging = false, changed = false;
        json out = value;
        if (value.is_object() && value.contains("colors")) {
            ColorGradient g;
            if (FromJson(value, g) && GradientEditor(key.c_str(), g, dragging)) out = ToJson(g), changed = true;
        } else if (key == "curve" || (value.is_object() && value.contains("keys"))) {
            FloatCurve c;
            if (FromJson(value, c) && CurveEditor(key.c_str(), c, dragging)) out = ToJson(c), changed = true;
        } else if (IsRange(key)) {
            FloatRange r;
            if (FromJson(value, r)) {
                f32 v[2] = {r.min, r.max};
                if (ImGui::DragFloat2((key + " (min, max)").c_str(), v, 0.01f)) out = ToJson(FloatRange{v[0], v[1]}), changed = true, dragging = ImGui::IsMouseDragging(0);
            }
        } else {
            changed = EditJsonField(key, out, dragging, &Enums());
        }
        if (changed) {
            std::string error;
            if (!doc_.SetModuleField(ref, key, out, &error, dragging ? "module:" + key : std::string())) status_ = error;
        }
        ImGui::PopID();
    }
}

void ParticleEditor::DrawEmitterDetails(usize e) {
    const Emitter& em = doc_.Get().emitters[e];
    if (!ImGui::IsAnyItemActive()) name_edit_ = em.settings.name;
    if (ImGui::InputText("Name", &name_edit_, ImGuiInputTextFlags_EnterReturnsTrue)) {
        std::string error;
        if (!doc_.RenameEmitter(e, name_edit_, &error)) status_ = error;
    }
    const json settings = doc_.EmitterSettings(e);
    for (const auto& [key, value] : settings.items()) {
        if (key == "name") continue;
        ImGui::PushID(key.c_str());
        json out = value;
        bool dragging = false;
        if (EditJsonField(key, out, dragging, &Enums())) {
            std::string error;
            if (!doc_.SetEmitterSetting(e, key, out, &error, dragging ? "setting:" + key : std::string())) status_ = error;
        }
        ImGui::PopID();
    }
    const GpuSupport gpu = CheckGpuSupport(em);
    ImGui::TextDisabled("Runs on the %s%s", ChooseSimTarget(em, true) == SimTarget::Gpu ? "GPU" : "CPU", gpu.supported ? "" : " (needs the CPU)");
    for (const std::string& r : gpu.reasons) ImGui::BulletText("%s", r.c_str());

    if (ImGui::CollapsingHeader("Sub-emitters")) {
        const char* events[] = {"Birth", "Death", "Collision"};
        for (usize i = 0; i < em.sub_emitters.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            SubEmitter s = em.sub_emitters[i];
            bool changed = false;
            int ev = static_cast<int>(s.event);
            if (ImGui::Combo("On", &ev, events, 3)) s.event = static_cast<ParticleEventKind>(ev), changed = true;
            if (ImGui::BeginCombo("Emitter", s.emitter.c_str())) {
                for (const Emitter& other : doc_.Get().emitters) {
                    if (&other != &em && ImGui::Selectable(other.settings.name.c_str(), other.settings.name == s.emitter)) s.emitter = other.settings.name, changed = true;
                }
                ImGui::EndCombo();
            }
            f32 count[2] = {s.count.min, s.count.max};
            if (ImGui::DragFloat2("Count", count, 0.1f, 0.0f, 10000.0f)) s.count = {count[0], count[1]}, changed = true;
            changed |= ImGui::SliderFloat("Probability", &s.probability, 0.0f, 1.0f);
            changed |= ImGui::SliderFloat("Inherit velocity", &s.inherit_velocity, 0.0f, 1.0f);
            changed |= ImGui::Checkbox("Inherit colour", &s.inherit_color);
            ImGui::SameLine();
            changed |= ImGui::Checkbox("Inherit size", &s.inherit_size);
            if (changed) doc_.SetSubEmitter(e, i, s);
            if (ImGui::SmallButton("Remove")) doc_.RemoveSubEmitter(e, i);
            ImGui::Separator();
            ImGui::PopID();
        }
        if (ImGui::Button("Add sub-emitter")) doc_.AddSubEmitter(e, SubEmitter{});
    }
    if (ImGui::CollapsingHeader("Bindings")) {
        for (usize i = 0; i < em.bindings.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::Text("%s -> %s", em.bindings[i].parameter.c_str(), em.bindings[i].field.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) doc_.RemoveBinding(e, i);
            ImGui::PopID();
        }
        std::string& field = binding_field_;
        int& param = binding_param_;
        const auto& params = doc_.Get().parameters;
        if (!params.empty()) {
            param = std::clamp(param, 0, static_cast<int>(params.size()) - 1);
            if (ImGui::BeginCombo("Parameter", params[static_cast<usize>(param)].name.c_str())) {
                for (usize p = 0; p < params.size(); ++p) {
                    if (ImGui::Selectable(params[p].name.c_str(), static_cast<int>(p) == param)) param = static_cast<int>(p);
                }
                ImGui::EndCombo();
            }
            ImGui::InputText("Field", &field);
            if (ImGui::Button("Bind")) doc_.AddBinding(e, {params[static_cast<usize>(param)].name, field});
        } else {
            ImGui::TextDisabled("Add parameters in the Parameters tab to bind them");
        }
    }
}

void ParticleEditor::DrawPreview() {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const f32 w = std::max(avail.x, 50.0f), h = std::max(avail.y, 50.0f);
    ImGui::InvisibleButton("##view", ImVec2(w, h), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        preview_.yaw -= io.MouseDelta.x * 0.01f;
        preview_.pitch = std::clamp(preview_.pitch + io.MouseDelta.y * 0.01f, -1.4f, 1.4f);
    }
    if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f) preview_.distance = std::clamp(preview_.distance * std::pow(0.9f, io.MouseWheel), 0.5f, 500.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(at, ImVec2(at.x + w, at.y + h), true);
    dl->AddRectFilled(at, ImVec2(at.x + w, at.y + h), IM_COL32(22, 24, 30, 255));
    auto project = [&](const Vec3& p, ImVec2& out) {
        f32 x, y, d;
        if (!preview_.Project(p, w, h, x, y, d)) return false;
        out = ImVec2(at.x + x, at.y + y);
        return true;
    };
    // A ground grid.
    for (int i = -5; i <= 5; ++i) {
        ImVec2 a, b;
        if (project({static_cast<f32>(i), 0, -5}, a) && project({static_cast<f32>(i), 0, 5}, b)) dl->AddLine(a, b, IM_COL32(60, 60, 70, 255));
        if (project({-5, 0, static_cast<f32>(i)}, a) && project({5, 0, static_cast<f32>(i)}, b)) dl->AddLine(a, b, IM_COL32(60, 60, 70, 255));
    }
    if (const ParticleSystemInstance* inst = preview_.Instance()) {
        ParticleRenderData data;
        BuildRenderData(*inst, preview_.Camera(), data);
        for (const SpriteBatch& b : data.sprites) {
            for (const SpriteInstance& s : b.instances) {
                ImVec2 c, r;
                if (!project(s.center, c) || !project(s.center + s.right, r)) continue;
                const f32 radius = std::max(1.0f, std::hypot(r.x - c.x, r.y - c.y));
                dl->AddCircleFilled(c, radius, Col(s.color), 12);
            }
        }
        for (const MeshBatch& b : data.meshes) {
            for (usize i = 0; i < b.transforms.size(); ++i) {
                ImVec2 c;
                const Vec4 t = b.transforms[i].cols[3];
                if (project({t.x, t.y, t.z}, c)) dl->AddRectFilled(ImVec2(c.x - 3, c.y - 3), ImVec2(c.x + 3, c.y + 3), Col(b.colors[i]));
            }
        }
        for (const RibbonStrip& r : data.ribbons) {
            for (usize i = 2; i + 1 < r.vertices.size(); i += 2) {
                ImVec2 a, b;
                if (project(r.vertices[i - 2].position, a) && project(r.vertices[i].position, b)) dl->AddLine(a, b, Col(r.vertices[i].color), 2.0f);
            }
        }
        for (const ParticleLight& l : data.lights) {
            ImVec2 c;
            if (project(l.position, c)) dl->AddCircle(c, 6.0f, IM_COL32(255, 230, 120, 200));
        }
    }
    dl->PopClipRect();
}

void ParticleEditor::DrawParameters() {
    const auto& params = doc_.Get().parameters;
    for (usize i = 0; i < params.size(); ++i) {
        const ParticleParameter p = params[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::Text("%s", p.name.c_str());
        ImGui::SameLine(160);
        ParameterValue v = p.value;
        bool changed = false;
        switch (v.type) {
        case ParameterType::Float: changed = ImGui::DragFloat("##v", &v.f, 0.05f); break;
        case ParameterType::Vector: {
            f32 f[3] = {v.v.x, v.v.y, v.v.z};
            if (ImGui::DragFloat3("##v", f, 0.05f)) v.v = Vec3(f[0], f[1], f[2]), changed = true;
            break;
        }
        case ParameterType::Color: {
            f32 f[4] = {v.c.r, v.c.g, v.c.b, v.c.a};
            if (ImGui::ColorEdit4("##v", f)) v.c = {f[0], f[1], f[2], f[3]}, changed = true;
            break;
        }
        }
        if (changed) doc_.SetParameterDefault(p.name, v, ImGui::IsMouseDragging(0) ? "param:" + p.name : std::string());
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
            doc_.RemoveParameter(p.name);
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    ImGui::SetNextItemWidth(140);
    ImGui::InputText("##name", &new_parameter_);
    ImGui::SameLine();
    const char* types[] = {"Float", "Vector", "Color"};
    ImGui::SetNextItemWidth(90);
    ImGui::Combo("##type", &new_parameter_type_, types, 3);
    ImGui::SameLine();
    if (ImGui::Button("Add parameter")) {
        ParameterValue v;
        v.type = static_cast<ParameterType>(new_parameter_type_);
        std::string error;
        if (!doc_.AddParameter(new_parameter_, v, &error)) status_ = error;
    }
}

void ParticleEditor::DrawStats() {
    if (ImGui::BeginTable("stats", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        for (const char* h : {"Emitter", "Particles", "Spawned", "CPU ms", "Runs on", "Bounds"}) ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        for (const ParticlePreview::EmitterStats& s : preview_.Stats()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s.name.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%zu", s.count);
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(s.spawned));
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", s.update_ms);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s.target == SimTarget::Gpu ? "GPU" : (s.gpu_supported ? "CPU" : "CPU only"));
            ImGui::TableNextColumn();
            if (s.bounds.valid) {
                ImGui::Text("%.1f x %.1f x %.1f", s.bounds.max.x - s.bounds.min.x, s.bounds.max.y - s.bounds.min.y, s.bounds.max.z - s.bounds.min.z);
            } else {
                ImGui::TextDisabled("-");
            }
        }
        ImGui::EndTable();
    }
}

void ParticleEditor::DrawDiagnostics() {
    const auto& d = doc_.Diagnostics();
    if (d.empty()) ImGui::TextDisabled("No problems");
    for (usize i = 0; i < d.size(); ++i) {
        ImGui::PushStyleColor(ImGuiCol_Text, d[i].error ? IM_COL32(255, 120, 110, 255) : IM_COL32(240, 200, 90, 255));
        ImGui::TextWrapped("%s  %s", d[i].code.c_str(), d[i].message.c_str());
        ImGui::PopStyleColor();
    }
}

void ParticleEditor::HandleKeys() {
    ImGuiIO& io = ImGui::GetIO();
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || io.WantTextInput) return;
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) io.KeyShift ? (void)doc_.Redo() : (void)doc_.Undo();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) doc_.Redo();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) SaveNow();
    if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space, false)) playing_ = !playing_;
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && module_) {
        doc_.RemoveModule(*module_);
        module_.reset();
    }
}

} // namespace aether::editor
