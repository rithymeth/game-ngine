#include "anim/blend_space_document.h"

#include <algorithm>
#include <cmath>
#include <fstream>

namespace aether::editor {
namespace {
using anim::BlendSpace;
using nlohmann::json;
f32 Span(f32 lo, f32 hi) { return hi - lo != 0.0f ? hi - lo : 1.0f; }
f32 Snap(f32 v, f32 lo, f32 hi, u32 divisions) {
    if (divisions == 0) return v;
    const f32 step = (hi - lo) / static_cast<f32>(divisions);
    return step > 0 ? lo + std::round((v - lo) / step) * step : v;
}
} // namespace

BlendSpaceDocument::BlendSpaceDocument(BlendSpace space, std::filesystem::path path)
    : space_(std::move(space)), path_(std::move(path)) { Changed(); }
void BlendSpaceDocument::Changed() {
    anim::Triangulate(space_);
    diagnostics_ = anim::ValidateBlendSpace(space_);
}
bool BlendSpaceDocument::Save(std::string* error) {
    if (path_.empty()) {
        if (error) *error = "the blend space has no file yet; use Save As";
        return false;
    }
    return SaveAs(path_, error);
}
bool BlendSpaceDocument::SaveAs(const std::filesystem::path& path, std::string* error) {
    std::ofstream file(path);
    if (!file) {
        if (error) *error = "can't write " + path.string();
        return false;
    }
    file << anim::BlendSpaceToJson(space_).dump(2) << "\n";
    path_ = path;
    dirty_ = false;
    return true;
}
std::string BlendSpaceDocument::Name() const { return path_.empty() ? "Untitled" : path_.stem().string(); }
void BlendSpaceDocument::Edit(const std::string& label, const std::function<void(BlendSpace&)>& change,
                              const std::string& merge_key) {
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
    Edit("Move sample", [&](BlendSpace& s) { s.samples[index].x = nx; s.samples[index].y = ny; }, merge_key);
    return true;
}
bool BlendSpaceDocument::RemoveSample(usize index) {
    if (index >= space_.samples.size()) return false;
    Edit("Remove sample", [&](BlendSpace& s) { s.samples.erase(s.samples.begin() + static_cast<std::ptrdiff_t>(index)); });
    return true;
}
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

} // namespace aether::editor
