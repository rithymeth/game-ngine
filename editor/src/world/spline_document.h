#pragma once

#include "aether/terrain/spline.h"

#include <optional>
#include <vector>

namespace aether::editor {

// A spline being edited (Phase 21 step 7): its control points (add,
// insert, remove, move, width and roll) with undo, the selected point, and
// what the viewport draws: the curve and the road's edges.
class SplineEditDocument {
public:
    explicit SplineEditDocument(terrain::Spline spline = {});

    const terrain::Spline& Get() const { return spline_; }
    u64 Revision() const { return revision_; }

    u32 AddPoint(const Vec3& position);                  // at the end; its index
    std::optional<u32> InsertAfter(u32 index);            // halfway to the next point
    bool RemovePoint(u32 index);
    bool MovePoint(u32 index, const Vec3& position, bool merge = false); // merge: part of a drag (one undo step)
    bool SetWidth(u32 index, f32 width);
    bool SetRoll(u32 index, f32 roll_degrees);

    std::optional<u32> selected;
    // The point nearest `p` on the ground plane within `radius`.
    std::optional<u32> PickPoint(const Vec3& p, f32 radius) const;

    bool Undo();
    bool Redo();
    bool CanUndo() const { return !undo_.empty(); }
    bool CanRedo() const { return !redo_.empty(); }

    // The curve, `per_segment` points per segment, and the road's left and right edges (BuildRoadMesh's vertices).
    std::vector<Vec3> CurvePoints(u32 per_segment = 8) const;
    void RoadEdges(u32 per_segment, std::vector<Vec3>& left, std::vector<Vec3>& right) const;

private:
    void Record(bool merge = false);

    terrain::Spline spline_;
    std::vector<std::vector<terrain::SplinePoint>> undo_, redo_;
    bool merging_ = false;
    u64 revision_ = 1;
};

} // namespace aether::editor
