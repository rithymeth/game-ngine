#include "world/spline_document.h"

#include <algorithm>
#include <cmath>

namespace aether::editor {

using namespace aether::terrain;

namespace {
constexpr usize kMaxUndo = 128;

// Rebuilds a spline from saved points.
Spline FromPoints(const std::vector<SplinePoint>& points) {
    Spline s;
    for (const SplinePoint& p : points) s.AddPoint(p.position);
    for (u32 i = 0; i < points.size(); ++i) {
        s.SetWidth(i, points[i].width);
        s.SetRoll(i, points[i].roll);
    }
    return s;
}

std::vector<SplinePoint> PointsOf(const Spline& s) { return std::vector<SplinePoint>(s.Points().begin(), s.Points().end()); }
} // namespace

SplineEditDocument::SplineEditDocument(Spline spline) : spline_(std::move(spline)) {}

void SplineEditDocument::Record(bool merge) {
    if (!(merge && merging_)) {
        undo_.push_back(PointsOf(spline_));
        if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    }
    merging_ = merge;
    redo_.clear();
    ++revision_;
}

u32 SplineEditDocument::AddPoint(const Vec3& p) {
    Record();
    spline_.AddPoint(p);
    selected = spline_.PointCount() - 1;
    return *selected;
}

std::optional<u32> SplineEditDocument::InsertAfter(u32 index) {
    if (index + 1 >= spline_.PointCount()) return std::nullopt;
    const Vec3 a = spline_.Points()[index].position, b = spline_.Points()[index + 1].position;
    Record();
    spline_.InsertPoint(index, (a + b) * 0.5f);
    selected = index + 1;
    return selected;
}

bool SplineEditDocument::RemovePoint(u32 index) {
    if (index >= spline_.PointCount()) return false;
    Record();
    spline_.RemovePoint(index);
    if (selected && *selected >= spline_.PointCount()) selected = spline_.PointCount() == 0 ? std::nullopt : std::optional<u32>(spline_.PointCount() - 1);
    return true;
}

bool SplineEditDocument::MovePoint(u32 index, const Vec3& p, bool merge) {
    if (index >= spline_.PointCount()) return false;
    Record(merge);
    spline_.SetPoint(index, p);
    return true;
}

bool SplineEditDocument::SetWidth(u32 index, f32 width) {
    if (index >= spline_.PointCount() || width < 0.0f) return false;
    Record();
    spline_.SetWidth(index, width);
    return true;
}

bool SplineEditDocument::SetRoll(u32 index, f32 roll) {
    if (index >= spline_.PointCount()) return false;
    Record();
    spline_.SetRoll(index, roll);
    return true;
}

std::optional<u32> SplineEditDocument::PickPoint(const Vec3& p, f32 radius) const {
    std::optional<u32> best;
    f32 best_d = radius;
    for (u32 i = 0; i < spline_.PointCount(); ++i) {
        const Vec3& q = spline_.Points()[i].position;
        const f32 d = std::hypot(q.x - p.x, q.z - p.z);
        if (d <= best_d) best = i, best_d = d;
    }
    return best;
}

bool SplineEditDocument::Undo() {
    if (undo_.empty()) return false;
    redo_.push_back(PointsOf(spline_));
    spline_ = FromPoints(undo_.back());
    undo_.pop_back();
    merging_ = false;
    if (selected && *selected >= spline_.PointCount()) selected.reset();
    ++revision_;
    return true;
}

bool SplineEditDocument::Redo() {
    if (redo_.empty()) return false;
    undo_.push_back(PointsOf(spline_));
    spline_ = FromPoints(redo_.back());
    redo_.pop_back();
    merging_ = false;
    ++revision_;
    return true;
}

std::vector<Vec3> SplineEditDocument::CurvePoints(u32 per_segment) const {
    std::vector<Vec3> out;
    const usize segs = spline_.SegmentCount();
    if (segs == 0) return out;
    const usize n = segs * std::max(per_segment, 1u) + 1;
    for (usize i = 0; i < n; ++i) out.push_back(spline_.Evaluate(static_cast<f32>(i) / static_cast<f32>(n - 1)));
    return out;
}

void SplineEditDocument::RoadEdges(u32 per_segment, std::vector<Vec3>& left, std::vector<Vec3>& right) const {
    left.clear(), right.clear();
    const std::vector<f32> mesh = BuildRoadMesh(spline_, per_segment);
    for (usize v = 0; v < mesh.size() / 8; ++v) {
        const Vec3 p(mesh[v * 8], mesh[v * 8 + 1], mesh[v * 8 + 2]);
        (v % 2 == 0 ? left : right).push_back(p);
    }
}

} // namespace aether::editor
