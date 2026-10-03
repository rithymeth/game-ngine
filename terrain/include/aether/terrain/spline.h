#pragma once

#include "aether/core/base.h"
#include "aether/math/math.h"

#include <span>
#include <vector>

namespace aether {
namespace terrain {

using std::span;

// Phase 21 step 5: spline tools for roads, rivers and fences placed
// along curves. Splines are Catmull-Rom curves with control points.

struct SplinePoint {
    Vec3 position{0, 0, 0};
    Vec3 tangent{1, 0, 0};   // pre-computed tangent (for Catmull-Rom)
    f32 roll = 0.0f;         // Z rotation around the spline's forward axis (degrees)
    f32 width = 2.0f;        // road width at this point (world units)
};

// A Catmull-Rom spline through control points.
// Supports road/river placement and mesh generation along the curve.
class Spline {
public:
    usize SegmentCount() const { return points_.size() >= 2 ? points_.size() - 1 : 0; }
    Spline() = default;
    explicit Spline(span<const Vec3> control_points);

    u32 PointCount() const { return static_cast<u32>(points_.size()); }
    span<const SplinePoint> Points() const { return points_; }

    // Add a control point (appended to the end).
    void AddPoint(Vec3 position);

    // Insert a point between index i and i+1.
    void InsertPoint(u32 after_index, Vec3 position);

    // Remove the point at index.
    void RemovePoint(u32 index);

    // Move a control point.
    void SetPoint(u32 index, Vec3 position);
    void SetWidth(u32 index, f32 width); // ignored for an index past the end
    void SetRoll(u32 index, f32 roll);

    // Recompute tangents (called after any point change).
    void RecomputeTangents();

    // Evaluate position along the spline at parameter t [0..1].
    // Uses Catmull-Rom interpolation between control points.
    Vec3 Evaluate(f32 t) const;

    // Evaluate position and tangent at parameter t [0..1].
    void Evaluate(f32 t, Vec3* out_pos, Vec3* out_tangent) const;

    // Get the total world-space length of the spline.
    f32 Length() const;

    // Sample the spline at regular intervals.
    // Returns positions every step units of arc length.
    std::vector<Vec3> SampleUniform(f32 step) const;

    // Sample at a fixed number of points per segment.
    std::vector<Vec3> SamplePerSegment(u32 points_per_segment) const;

    // Find the closest point on the spline to a world position.
    // Returns (t, distance).
    std::pair<f32, f32> ClosestPoint(Vec3 pos) const;

    // Get the tangent at parameter t.
    Vec3 Tangent(f32 t) const;

    // Get the roll at parameter t (linear interpolation between points).
    f32 Roll(f32 t) const;

    // Get the width at parameter t.
    f32 Width(f32 t) const;

    // Get the forward frame (tangent, up, right) at t.
    void Frame(f32 t, Vec3* out_tangent, Vec3* out_up, Vec3* out_right) const;

private:
    std::vector<SplinePoint> points_;

    f32 SegmentT(f32 t) const;
};

// Build a strip mesh along the spline.
// Returns interleaved vertices: pos(3) + normal(3) + uv(2), 8 floats/vertex.
// Width is the half-width on each side. UV v coordinate goes 0..1 along length.
std::vector<f32> BuildSplineMesh(const Spline& spline, f32 half_width, u32 verts_per_segment);

// Build a road mesh: two-sided strip with the center line at UV.y=0.5.
std::vector<f32> BuildRoadMesh(const Spline& spline, u32 verts_per_segment);

// Get road markings: dashed center line texture UV (alternating on/off).
// UV: x = along length, y = across width (0=center, 1=edge).
// A value of 1.0 = painted, 0.0 = no paint.
std::vector<f32> BuildRoadMask(const Spline& spline, u32 verts_per_segment, f32 dash_length);

// Closest point on a single Catmull-Rom segment.
std::pair<f32, f32> ClosestPointOnSegment(Vec3 pos, const SplinePoint& p0,
                                           const SplinePoint& p1, const SplinePoint& p2,
                                           const SplinePoint& p3);

// De Boor evaluation of a Catmull-Rom curve.
Vec3 CatmullRom(f32 t, const SplinePoint& p0, const SplinePoint& p1,
                const SplinePoint& p2, const SplinePoint& p3);

} // namespace terrain
} // namespace aether
