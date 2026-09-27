#include "anim/blend_space_editor.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace aether::editor {

using anim::BlendSpace;
using nlohmann::json;

namespace {
f32 Span(f32 lo, f32 hi) { return hi - lo != 0.0f ? hi - lo : 1.0f; }
f32 Snap(f32 v, f32 lo, f32 hi, u32 divisions) {
    if (divisions == 0) return v;
    const f32 step = (hi - lo) / static_cast<f32>(divisions);
    return step > 0 ? lo + std::round((v - lo) / step) * step : v;
}
} // namespace

// --- Document --------------------------------------------------------------------------------------

BlendSpaceDocument::BlendSpaceDocument(BlendSpace space, std::filesystem::path path) : space_(std::move(space)), path_(std::move(path)) { Changed(); }

void BlendSpaceDocument::Changed() {
    anim::Triangulate(space_);
    diagnostics_ = anim::ValidateBlendSpace(space_);
}

bool BlendSpaceDocument::Save(std::string* error) {
    if (path_.empty()) {
        if (error != nullptr) *error = "the blend space has no file yet; use Save As";
        return false;
    }
    return SaveAs(path_, error);
}

bool BlendSpaceDocument::SaveAs(const std::filesystem::path& path, std::string* error) {
    std::ofstream file(path);
    if (!file) {
        if (error != nullptr) *error = "can't write " + path.string();
        return false;
    }
    file << anim::BlendSpaceToJson(space_).dump(2) << "\n";
    path_ = path;
    dirty_ = false;
    return true;
}

std::string BlendSpaceDocument::Name() const { return path_.empty() ? "Untitled" : path_.stem().string(); }

void BlendSpaceDocument::Edit(const std::string& label, const std::function<void(BlendSpace&)>& change, const std::string& merge_key) {
    history_.Record(label, anim::BlendSpaceToJson(space_), merge_key);
    change(space_);
    dirty_ = true;
    Changed();
}

bool BlendSpaceDocument::Undo() {
    json restore;
    if (!history_.Undo(anim::BlendSpaceToJson(space_), restore)) return false;
    anim::BlendSpaceFromJson(restore, space_);
    dirty_ = true;
    Changed();
    return true;
}

bool BlendSpaceDocument::Redo() {
    json restore;
    if (!history_.Redo(anim::BlendSpaceToJson(space_), restore)) return false;
    anim::BlendSpaceFromJson(restore, space_);
    dirty_ = true;
    Changed();
    return true;
}

usize BlendSpaceDocument::AddSample(const std::string& clip, f32 x, f32 y) {
    Edit("Add sample", [&](BlendSpace& s) { s.samples.push_back({clip, x, s.dimensions == 2 ? y : 0.0f, 1.0f}); });
    return space_.samples.size() - 1;
}

bool BlendSpaceDocument::MoveSample(usize index, f32 x, f32 y, u32 divisions, const std::string& merge_key) {
    if (index >= space_.samples.size()) return false;
    const f32 nx = Snap(std::clamp(x, space_.x.min, space_.x.max), space_.x.min, space_.x.max, divisions);
    const f32 ny = space_.dimensions == 2 ? Snap(std::clamp(y, space_.y.min, space_.y.max), space_.y.min, space_.y.max, divisions) : 0.0f;
    Edit("Move sample", [&](BlendSpace& s) {
        s.samples[index].x = nx;
        s.samples[index].y = ny;
    }, merge_key);
    return true;
}

bool BlendSpaceDocument::RemoveSample(usize index) {
    if (index >= space_.samples.size()) return false;
    Edit("Remove sample", [&](BlendSpace& s) { s.samples.erase(s.samples.begin() + static_cast<std::ptrdiff_t>(index)); });
    return true;
}

// --- Canvas ----------------------------------------------------------------------------------------

void BlendSpaceCanvas::ParamToScreen(const BlendSpace& s, f32 px, f32 py, f32& sx, f32& sy) const {
    sx = x0 + (px - s.x.min) / Span(s.x.min, s.x.max) * width;
    sy = s.dimensions == 2 ? y0 + height - (py - s.y.min) / Span(s.y.min, s.y.max) * height : y0 + height * 0.5f;
}

void BlendSpaceCanvas::ScreenToParam(const BlendSpace& s, f32 sx, f32 sy, f32& px, f32& py) const {
    px = s.x.min + (sx - x0) / (width > 0 ? width : 1.0f) * Span(s.x.min, s.x.max);
    py = s.dimensions == 2 ? s.y.min + (y0 + height - sy) / (height > 0 ? height : 1.0f) * Span(s.y.min, s.y.max) : 0.0f;
}

i32 BlendSpaceCanvas::HitTest(const BlendSpace& s, f32 sx, f32 sy, f32 radius) const {
    i32 best = -1;
    f32 best_d = radius * radius;
    for (usize i = 0; i < s.samples.size(); ++i) {
        f32 x, y;
        ParamToScreen(s, s.samples[i].x, s.samples[i].y, x, y);
        const f32 d = (x - sx) * (x - sx) + (y - sy) * (y - sy);
        if (d <= best_d) best_d = d, best = static_cast<i32>(i);
    }
    return best;
}

// --- Editor ----------------------------------------------------------------------------------------

std::vector<anim::BlendWeight> BlendSpaceEditor::PreviewWeights() const { return anim::ComputeWeights(doc_.Get(), preview_x, preview_y); }

void BlendSpaceEditor::Draw() {
    ImGui::PushID(this);
    if (ImGui::Button("Undo") && doc_.Undo()) selected_ = std::min(selected_, static_cast<i32>(doc_.Get().samples.size()) - 1);
    ImGui::SameLine();
    if (ImGui::Button("Redo")) doc_.Redo();
    ImGui::SameLine();
    int div = static_cast<int>(grid_divisions);
    ImGui::SetNextItemWidth(100);
    if (ImGui::SliderInt("Snap", &div, 0, 10)) grid_divisions = static_cast<u32>(div);
    ImGui::SameLine();
    ImGui::TextDisabled("|  %s%s", doc_.Name().c_str(), doc_.Dirty() ? "*" : "");
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("##grid", ImVec2(std::max(200.0f, avail.x - 280.0f), 0), ImGuiChildFlags_Borders);
    DrawGrid();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##samples", ImVec2(0, 0), ImGuiChildFlags_Borders);
    DrawSampleList();
    ImGui::EndChild();
    ImGui::PopID();
}

void BlendSpaceEditor::DrawGrid() {
    const BlendSpace& s = doc_.Get();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    canvas_ = {origin.x + 30, origin.y + 10, std::max(size.x - 50, 10.0f), std::max((s.dimensions == 2 ? size.y : 60.0f) - 40, 10.0f)};
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const u32 divisions = std::max(grid_divisions, 1u);
    for (u32 i = 0; i <= divisions; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(divisions);
        const f32 x = canvas_.x0 + t * canvas_.width;
        draw->AddLine(ImVec2(x, canvas_.y0), ImVec2(x, canvas_.y0 + canvas_.height), IM_COL32(70, 70, 70, 255));
        if (s.dimensions == 2) {
            const f32 y = canvas_.y0 + t * canvas_.height;
            draw->AddLine(ImVec2(canvas_.x0, y), ImVec2(canvas_.x0 + canvas_.width, y), IM_COL32(70, 70, 70, 255));
        }
    }
    auto to_screen = [&](f32 px, f32 py) {
        f32 x, y;
        canvas_.ParamToScreen(s, px, py, x, y);
        return ImVec2(x, y);
    };
    for (const auto& t : s.triangles) {
        const ImVec2 a = to_screen(s.samples[t[0]].x, s.samples[t[0]].y), b = to_screen(s.samples[t[1]].x, s.samples[t[1]].y),
                     c = to_screen(s.samples[t[2]].x, s.samples[t[2]].y);
        draw->AddTriangle(a, b, c, IM_COL32(90, 110, 140, 255));
    }
    const std::vector<anim::BlendWeight> weights = PreviewWeights();
    for (usize i = 0; i < s.samples.size(); ++i) {
        const ImVec2 p = to_screen(s.samples[i].x, s.samples[i].y);
        f32 w = 0;
        for (const anim::BlendWeight& b : weights) {
            if (b.sample == i) w = b.weight;
        }
        draw->AddCircleFilled(p, 5.0f + 5.0f * w, static_cast<i32>(i) == selected_ ? IM_COL32(255, 200, 60, 255) : IM_COL32(200, 200, 200, 255));
        draw->AddText(ImVec2(p.x + 8, p.y - 7), IM_COL32(220, 220, 220, 255), s.samples[i].clip.c_str());
    }
    const ImVec2 dot = to_screen(preview_x, preview_y);
    draw->AddCircleFilled(dot, 5.0f, IM_COL32(80, 220, 120, 255));
    draw->AddText(ImVec2(canvas_.x0, canvas_.y0 + canvas_.height + 4), IM_COL32(170, 170, 170, 255), s.x.name.c_str());

    // Interaction: drag samples (snapped), drag the preview dot, right-click to add, Delete to remove.
    ImGui::InvisibleButton("##canvas", ImVec2(std::max(size.x, 1.0f), std::max(size.y, 1.0f)),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    f32 px, py;
    canvas_.ScreenToParam(s, mouse.x, mouse.y, px, py);
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        dragging_ = canvas_.HitTest(s, mouse.x, mouse.y);
        dragging_preview_ = dragging_ < 0;
        if (dragging_ >= 0) selected_ = dragging_;
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        if (dragging_ >= 0) {
            doc_.MoveSample(static_cast<usize>(dragging_), px, py, grid_divisions, "drag:" + std::to_string(dragging_));
        } else if (dragging_preview_) {
            preview_x = std::clamp(px, s.x.min, s.x.max);
            preview_y = s.dimensions == 2 ? std::clamp(py, s.y.min, s.y.max) : 0.0f;
        }
    }
    if (!ImGui::IsItemActive()) dragging_ = -1, dragging_preview_ = false;
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        const std::string clip = clip_names_.empty() ? std::string("Clip") : clip_names_.front();
        selected_ = static_cast<i32>(doc_.AddSample(clip, Snap(px, s.x.min, s.x.max, grid_divisions), Snap(py, s.y.min, s.y.max, grid_divisions)));
    }
    if (ImGui::IsItemHovered() && ImGui::IsKeyPressed(ImGuiKey_Delete, false) && selected_ >= 0) {
        doc_.RemoveSample(static_cast<usize>(selected_));
        selected_ = -1;
    }
}

void BlendSpaceEditor::DrawSampleList() {
    const BlendSpace& s = doc_.Get();
    ImGui::SeparatorText("Axes");
    std::string name = s.x.name;
    f32 range[2] = {s.x.min, s.x.max};
    if (ImGui::InputText("X Name", &name)) doc_.Edit("Axis", [&](BlendSpace& b) { b.x.name = name; }, "x_name");
    if (ImGui::DragFloat2("X Range", range, 0.1f)) doc_.Edit("Axis", [&](BlendSpace& b) { b.x.min = range[0], b.x.max = range[1]; }, "x_range");
    if (s.dimensions == 2) {
        name = s.y.name;
        f32 yr[2] = {s.y.min, s.y.max};
        if (ImGui::InputText("Y Name", &name)) doc_.Edit("Axis", [&](BlendSpace& b) { b.y.name = name; }, "y_name");
        if (ImGui::DragFloat2("Y Range", yr, 0.1f)) doc_.Edit("Axis", [&](BlendSpace& b) { b.y.min = yr[0], b.y.max = yr[1]; }, "y_range");
    }
    ImGui::SeparatorText("Samples");
    for (usize i = 0; i < s.samples.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable((s.samples[i].clip + "##sample").c_str(), static_cast<i32>(i) == selected_)) selected_ = static_cast<i32>(i);
        ImGui::PopID();
    }
    if (selected_ >= 0 && static_cast<usize>(selected_) < s.samples.size()) {
        const usize i = static_cast<usize>(selected_);
        const anim::BlendSample sample = s.samples[i];
        ImGui::SeparatorText("Sample");
        std::string clip = sample.clip;
        if (ImGui::BeginCombo("Clip", clip.c_str())) {
            for (const std::string& c : clip_names_) {
                if (ImGui::Selectable(c.c_str(), c == clip)) doc_.Edit("Sample clip", [&](BlendSpace& b) { b.samples[i].clip = c; });
            }
            ImGui::EndCombo();
        }
        f32 pos[2] = {sample.x, sample.y};
        if (ImGui::DragFloat2("Position", pos, 0.01f)) doc_.MoveSample(i, pos[0], pos[1], 0, "pos:" + std::to_string(i));
        f32 rate = sample.rate;
        if (ImGui::DragFloat("Rate", &rate, 0.01f, 0.01f, 10.0f)) doc_.Edit("Rate", [&](BlendSpace& b) { b.samples[i].rate = rate; }, "rate:" + std::to_string(i));
        if (ImGui::Button("Remove")) {
            doc_.RemoveSample(i);
            selected_ = -1;
        }
    }
    ImGui::SeparatorText("Preview");
    for (const anim::BlendWeight& w : PreviewWeights()) ImGui::Text("%s  %.0f%%", s.samples[w.sample].clip.c_str(), w.weight * 100.0f);
    for (const anim::BlendDiagnostic& d : doc_.Diagnostics()) {
        ImGui::TextColored(d.error ? ImVec4(0.9f, 0.35f, 0.35f, 1) : ImVec4(0.9f, 0.75f, 0.2f, 1), "%s %s", d.code.c_str(), d.message.c_str());
    }
}

} // namespace aether::editor
