#include "aether/terrain/spline.h"
#include "terrain_math.h"

#include <algorithm>
#include <cmath>

namespace aether {
namespace terrain {

using namespace aether;

// --- Catmull-Rom helpers ---

Vec3 CatmullRom(f32 t, const SplinePoint& p0, const SplinePoint& p1,
                const SplinePoint& p2, const SplinePoint& p3) {
    const f32 t2 = t * t;
    const f32 t3 = t2 * t;
    const f32 b0 = -0.5f * t3 +       t2 - 0.5f * t;
    const f32 b1 =  1.5f * t3 - 2.5f * t2 + 1.0f;
    const f32 b2 = -1.5f * t3 + 2.0f * t2 + 0.5f * t;
    const f32 b3 =  0.5f * t3 - 0.5f * t2;
    return b0 * p0.position + b1 * p1.position
         + b2 * p2.position + b3 * p3.position;
}

Vec3 CatmullRomTangent(const SplinePoint& p0, const SplinePoint& p1,
                       const SplinePoint& p2, const SplinePoint& p3) {
    // Derivative of Catmull-Rom.
    // d/dt[CatmullRom] = ...
    // Use finite differences on the position curve.
    const f32 eps = 1e-4f;
    Vec3 a = CatmullRom(0.0f - eps, p0, p1, p2, p3);
    Vec3 b = CatmullRom(0.0f + eps, p1, p2, p3, p0);
    return Normalize(b - a);
}

// --- Spline construction ---

Spline::Spline(span<const Vec3> control_points) {
    for (const Vec3& p : control_points) {
        points_.push_back(SplinePoint{.position = p});
    }
    RecomputeTangents();
}

void Spline::AddPoint(Vec3 position) {
    points_.push_back(SplinePoint{.position = position});
    // Recompute last 2 tangents.
    if (points_.size() >= 2) {
        const usize n = points_.size() - 1;
        Vec3 t = Normalize(points_[n].position - points_[n-1].position);
        points_[n-1].tangent = t;
        points_[n].tangent = t;
    }
}

void Spline::InsertPoint(u32 after_index, Vec3 position) {
    if (after_index >= points_.size()) return;
    SplinePoint p;
    p.position = position;
    points_.insert(points_.begin() + after_index + 1, p);
    RecomputeTangents();
}

void Spline::RemovePoint(u32 index) {
    if (index >= points_.size()) return;
    points_.erase(points_.begin() + index);
    RecomputeTangents();
}

void Spline::SetWidth(u32 index, f32 width) {
    if (index < points_.size()) points_[index].width = width;
}

void Spline::SetRoll(u32 index, f32 roll) {
    if (index < points_.size()) points_[index].roll = roll;
}

void Spline::SetPoint(u32 index, Vec3 position) {
    if (index >= points_.size()) return;
    points_[index].position = position;
    RecomputeTangents();
}

void Spline::RecomputeTangents() {
    const usize n = points_.size();
    if (n < 2) return;

    for (usize i = 0; i < n; ++i) {
        const usize prev = (i == 0) ? n - 1 : i - 1;
        const usize next = (i + 1) % n;
        // Non-periodic: use forward/backward difference at endpoints.
        Vec3 t;
        if (i == 0) {
            t = Normalize(points_[1].position - points_[0].position);
        } else if (i == n - 1) {
            t = Normalize(points_[n-1].position - points_[n-2].position);
        } else {
            t = Normalize(points_[next].position - points_[prev].position);
        }
        points_[i].tangent = t;
    }
}

// --- Evaluation ---

f32 Spline::Length() const {
    f32 len = 0.0f;
    constexpr u32 kSamples = 16;
    Vec3 prev = Evaluate(0.0f);
    for (u32 i = 1; i <= kSamples; ++i) {
        const f32 t = static_cast<f32>(i) / kSamples;
        Vec3 cur = Evaluate(t);
        len += (cur - prev).Length();
        prev = cur;
    }
    return len;
}

f32 Spline::SegmentT(f32 t) const {
    const usize segs = SegmentCount();
    if (segs == 0) return 0.0f;
    const f32 st = t * static_cast<f32>(segs);
    return st - std::floor(st);
}

void Spline::Evaluate(f32 t, Vec3* out_pos, Vec3* out_tangent) const {
    const usize segs = SegmentCount();
    if (segs == 0) { *out_pos = Vec3(0,0,0); *out_tangent = Vec3(1,0,0); return; }

    t = std::clamp(t, 0.0f, 1.0f);
    const f32 st = t * static_cast<f32>(segs);
    usize seg = static_cast<usize>(st);
    if (seg >= segs) seg = segs - 1;
    const f32 local_t = st - static_cast<f32>(seg);

    const usize i0 = seg == 0 ? 0 : seg - 1;
    const usize i1 = seg;
    const usize i2 = std::min(seg + 1, points_.size() - 1);
    const usize i3 = std::min(seg + 2, points_.size() - 1);

    *out_pos = CatmullRom(local_t, points_[i0], points_[i1],
                          points_[i2], points_[i3]);
    *out_tangent = CatmullRomTangent(points_[i0], points_[i1],
                                     points_[i2], points_[i3]);
}

Vec3 Spline::Evaluate(f32 t) const {
    Vec3 pos{0,0,0}, tang{1,0,0};
    Evaluate(t, &pos, &tang);
    return pos;
}

Vec3 Spline::Tangent(f32 t) const {
    Vec3 pos{0,0,0}, tang{1,0,0};
    Evaluate(t, &pos, &tang);
    return tang;
}

f32 Spline::Roll(f32 t) const {
    const usize segs = SegmentCount();
    if (segs == 0) return 0.0f;
    const f32 st = t * static_cast<f32>(segs);
    usize seg = static_cast<usize>(st);
    if (seg >= segs) seg = segs - 1;
    const f32 local_t = st - static_cast<f32>(seg);
    const f32 r0 = points_[seg].roll;
    const f32 r1 = (seg + 1 < points_.size()) ? points_[seg + 1].roll : points_[seg].roll;
    return r0 + (r1 - r0) * local_t;
}

f32 Spline::Width(f32 t) const {
    const usize segs = SegmentCount();
    if (segs == 0) return 2.0f;
    const f32 st = t * static_cast<f32>(segs);
    usize seg = static_cast<usize>(st);
    if (seg >= segs) seg = segs - 1;
    const f32 local_t = st - static_cast<f32>(seg);
    const f32 w0 = points_[seg].width;
    const f32 w1 = (seg + 1 < points_.size()) ? points_[seg + 1].width : points_[seg].width;
    return w0 + (w1 - w0) * local_t;
}

void Spline::Frame(f32 t, Vec3* out_tangent, Vec3* out_up, Vec3* out_right) const {
    Vec3 tangent = Tangent(t);
    const f32 roll = Roll(t);
    // Apply roll around the tangent axis.
    Vec3 up{0, 1, 0};
    Vec3 right = Normalize(Cross(tangent, up));
    up = Normalize(Cross(right, tangent));
    // Simple roll: rotate up/right around tangent.
    const f32 cr = std::cos(roll);
    const f32 sr = std::sin(roll);
    const Vec3 new_up = up * cr + right * sr;
    const Vec3 new_right = right * cr - up * sr;
    *out_tangent = tangent;
    *out_up = new_up;
    *out_right = new_right;
}

// --- Sampling ---

std::vector<Vec3> Spline::SampleUniform(f32 step) const {
    std::vector<Vec3> samples;
    const f32 len = Length();
    const u32 count = static_cast<u32>(std::ceil(len / step)) + 1;
    samples.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        const f32 t = static_cast<f32>(i) / static_cast<f32>(count - 1);
        samples.push_back(Evaluate(t));
    }
    return samples;
}

std::vector<Vec3> Spline::SamplePerSegment(u32 pts_per_seg) const {
    std::vector<Vec3> samples;
    const usize segs = SegmentCount();
    if (segs == 0) return samples;

    for (usize s = 0; s < segs; ++s) {
        for (u32 p = 0; p < pts_per_seg; ++p) {
            const f32 t = (static_cast<f32>(p) / pts_per_seg);
            Vec3 pos{0,0,0}, tang{1,0,0};
            Evaluate(static_cast<f32>(s + t) / static_cast<f32>(segs), &pos, &tang);
            samples.push_back(pos);
        }
    }
    // Last point.
    Vec3 last{0,0,0}, last_tang{1,0,0};
    Evaluate(1.0f, &last, &last_tang);
    samples.push_back(last);
    return samples;
}

// --- Closest point ---

std::pair<f32, f32> Spline::ClosestPoint(Vec3 pos) const {
    const usize segs = SegmentCount();
    if (segs == 0) return {0.0f, 1e9f};

    usize best_seg = 0;
    f32 best_local_t = 0.0f;
    f32 best_dist2 = 1e9f;

    for (usize s = 0; s < segs; ++s) {
        const usize i0 = s == 0 ? 0 : s - 1;
        const usize i1 = s;
        const usize i2 = std::min(s + 1, points_.size() - 1);
        const usize i3 = std::min(s + 2, points_.size() - 1);
        auto [lt, d2] = ClosestPointOnSegment(pos,
            points_[i0], points_[i1], points_[i2], points_[i3]);
        if (d2 < best_dist2) {
            best_dist2 = d2;
            best_local_t = lt;
            best_seg = s;
        }
    }

    const f32 t = (static_cast<f32>(best_seg) + best_local_t) / static_cast<f32>(segs);
    return {std::clamp(t, 0.0f, 1.0f), best_dist2};
}

std::pair<f32, f32> ClosestPointOnSegment(Vec3 pos, const SplinePoint& p0,
                                           const SplinePoint& p1,
                                           const SplinePoint& p2,
                                           const SplinePoint& p3) {
    // Sample the segment and find the closest.
    constexpr u32 kSamples = 32;
    f32 best_t = 0.0f;
    f32 best_dist2 = 1e9f;

    for (u32 i = 0; i <= kSamples; ++i) {
        const f32 t = static_cast<f32>(i) / kSamples;
        const Vec3 sp = CatmullRom(t, p0, p1, p2, p3);
        const Vec3 diff = sp - pos;
        const f32 d2 = Dot(diff, diff);
        if (d2 < best_dist2) {
            best_dist2 = d2;
            best_t = t;
        }
    }
    return {best_t, best_dist2};
}

// --- Mesh generation ---

std::vector<f32> BuildSplineMesh(const Spline& spline, f32 half_width, u32 verts_per_segment) {
    const usize segs = spline.SegmentCount();
    if (segs == 0) return {};

    // verts: (segs * verts_per_segment + 1) * 2 (left + right strip)
    const usize verts = (segs * verts_per_segment + 1) * 2;
    std::vector<f32> mesh(verts * 8, 0.0f); // pos(3)+normal(3)+uv(2)

    usize vidx = 0;
    for (usize s = 0; s <= segs; ++s) {
        const f32 t = static_cast<f32>(s) / static_cast<f32>(segs);
        Vec3 pos = spline.Evaluate(t);
        Vec3 tangent{1,0,0}, up{0,1,0}, right{1,0,0};
        spline.Frame(t, &tangent, &up, &right);

        const f32 w = spline.Width(t) * half_width;
        const Vec3 left = pos - right * w;
        const Vec3 right_v = pos + right * w;

        // Two vertices per cross-section: left and right.
        // Normal is up (for a road it's (0, 1, 0) world-normalized).
        const Vec3 normal{0, 1, 0};
        const f32 v_coord = static_cast<f32>(s) / static_cast<f32>(segs);

        // Left vertex
        mesh[vidx++] = left.x;  mesh[vidx++] = left.y;  mesh[vidx++] = left.z;
        mesh[vidx++] = normal.x; mesh[vidx++] = normal.y; mesh[vidx++] = normal.z;
        mesh[vidx++] = 0.0f;    mesh[vidx++] = v_coord;

        // Right vertex
        mesh[vidx++] = right_v.x; mesh[vidx++] = right_v.y; mesh[vidx++] = right_v.z;
        mesh[vidx++] = normal.x; mesh[vidx++] = normal.y; mesh[vidx++] = normal.z;
        mesh[vidx++] = 1.0f;    mesh[vidx++] = v_coord;
    }

    return mesh;
}

std::vector<f32> BuildRoadMesh(const Spline& spline, u32 verts_per_segment) {
    return BuildSplineMesh(spline, 1.0f, verts_per_segment);
}

std::vector<f32> BuildRoadMask(const Spline& spline, u32 verts_per_segment, f32 dash_length) {
    // Mask: UV x = along road (0=left, 1=right), y = along length
    // Dashed center line: at UV x=0.5, dash pattern along y.
    const usize segs = spline.SegmentCount();
    if (segs == 0) return {};
    const usize verts = (segs * verts_per_segment + 1) * 2;
    std::vector<f32> mask(verts * 2, 0.0f); // x = mask(0/1), y = along length

    usize vidx = 0;
    f32 total_len = 0.0f;
    std::vector<f32> arc_lengths(segs + 1, 0.0f);

    for (usize s = 0; s <= segs; ++s) {
        if (s > 0) {
            Vec3 a = spline.Evaluate(static_cast<f32>(s-1) / static_cast<f32>(segs));
            Vec3 b = spline.Evaluate(static_cast<f32>(s) / static_cast<f32>(segs));
            total_len += Length(b - a);
            arc_lengths[s] = total_len;
        }
    }

    for (usize s = 0; s <= segs; ++s) {
        const f32 y_along = arc_lengths[s] / total_len;
        const f32 dash_t = Fract(arc_lengths[s] / dash_length);
        const f32 center_mask = (dash_t < 0.5f) ? 1.0f : 0.0f;

        // Left and right vertices at x=0 (left edge) and x=1 (right edge)
        // The dashed center line at x=0.5 gets mask 0 or 1.
        // We output mask at 3 points: left(0), center(0.5), right(1)
        // But for simplicity: left vertex = 0, right vertex = 0, and we output
        // a separate "center line" vertex pair.
        mask[vidx++] = 0.0f;    mask[vidx++] = y_along;
        mask[vidx++] = 0.0f;    mask[vidx++] = y_along; // center as separate...
    }
    return mask;
}

} // namespace terrain
} // namespace aether
