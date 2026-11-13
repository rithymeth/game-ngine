#include "aether/terrain/spline.h"
#include "test_framework.h"

#include <cmath>

using namespace aether;
using namespace aether::terrain;

namespace {
bool NearEqual(f32 a, f32 b, f32 tol) { return std::fabs(a - b) <= tol; }
bool NearEqual(const Vec3& a, const Vec3& b, f32 tol) { return (a - b).Length() <= tol; }
} // namespace

AETHER_TEST(Spline_AddAndEval) {
    Spline sp;
    AETHER_CHECK(sp.PointCount() == 0);
    sp.AddPoint({0, 0, 0});
    sp.AddPoint({1, 0, 0});
    sp.AddPoint({1, 0, 1});
    AETHER_CHECK(sp.PointCount() == 3);

    // t=0 should be near the first control point.
    Vec3 p0 = sp.Evaluate(0.0f);
    AETHER_CHECK(NearEqual(p0, Vec3(0, 0, 0), 0.5f));

    // t=1 should be near the last control point.
    Vec3 p1 = sp.Evaluate(1.0f);
    AETHER_CHECK(NearEqual(p1, Vec3(1, 0, 1), 0.5f));
}

AETHER_TEST(Spline_InsertRemove) {
    Spline sp;
    sp.AddPoint({0, 0, 0});
    sp.AddPoint({10, 0, 0});
    sp.AddPoint({20, 0, 0});
    AETHER_CHECK(sp.PointCount() == 3);

    // Insert at index 1.
    sp.InsertPoint(1, {5, 0, 0});
    AETHER_CHECK(sp.PointCount() == 4);

    // Evaluate t=0.5 should be near middle.
    Vec3 mid = sp.Evaluate(0.5f);
    AETHER_CHECK(mid.x > 0.0f && mid.x < 20.0f);

    // Remove the inserted point.
    sp.RemovePoint(1);
    AETHER_CHECK(sp.PointCount() == 3);
}

AETHER_TEST(Spline_Frame) {
    Spline sp;
    sp.AddPoint({0, 0, 0});
    sp.AddPoint({1, 0, 0});
    sp.AddPoint({1, 0, 1});
    sp.AddPoint({0, 0, 1});

    Vec3 tangent{1,0,0}, up{0,1,0}, right{1,0,0};
    sp.Frame(0.5f, &tangent, &up, &right);

    AETHER_CHECK(NearEqual(tangent.Length(), 1.0f, 1e-4f));
    AETHER_CHECK(NearEqual(up.Length(), 1.0f, 1e-4f));
    AETHER_CHECK(NearEqual(right.Length(), 1.0f, 1e-4f));
    // tangent should be roughly in the XZ plane.
    AETHER_CHECK(std::abs(tangent.y) < 0.5f);
}

AETHER_TEST(Spline_ClosestPoint) {
    Spline sp;
    sp.AddPoint({0, 0, 0});
    sp.AddPoint({10, 0, 0});
    sp.AddPoint({10, 0, 10});
    sp.AddPoint({0, 0, 10});

    // Point near the middle of the spline.
    auto [t, dist2] = sp.ClosestPoint({5, 0, -1});
    AETHER_CHECK(t >= 0.0f && t <= 1.0f);
    AETHER_CHECK(dist2 >= 0.0f);
    // Should be close to the spline at t.
    AETHER_CHECK(dist2 < 10.0f);
}

AETHER_TEST(Spline_UniformSample) {
    Spline sp;
    sp.AddPoint({0, 0, 0});
    sp.AddPoint({5, 0, 0});
    sp.AddPoint({10, 0, 0});

    auto samples = sp.SampleUniform(1.0f);
    AETHER_CHECK(samples.size() > 2); // should have multiple samples
    AETHER_CHECK(samples[0].x < samples.back().x + 0.1f); // roughly increasing x
}

AETHER_TEST(Spline_WidthInterpolation) {
    Spline sp;
    sp.AddPoint({0, 0, 0});    sp.SetWidth(0, 2.0f);
    sp.AddPoint({10, 0, 0});   sp.SetWidth(1, 6.0f);
    AETHER_CHECK(NearEqual(sp.Width(0.0f), 2.0f, 1e-4f));
    AETHER_CHECK(NearEqual(sp.Width(1.0f), 6.0f, 1e-4f));
    // Halfway: average of 2 and 6.
    AETHER_CHECK(NearEqual(sp.Width(0.5f), 4.0f, 1e-3f));
}

AETHER_TEST(Spline_MoveControlPoint) {
    Spline sp;
    sp.AddPoint({0, 0, 0});
    sp.AddPoint({5, 0, 0});
    sp.AddPoint({10, 0, 0});

    Vec3 before = sp.Evaluate(0.5f);
    sp.SetPoint(1, {20, 0, 0}); // move middle point far right
    Vec3 after = sp.Evaluate(0.5f);

    AETHER_CHECK(after.x > before.x); // t=0.5 should now be further right
}

AETHER_TEST(Spline_MeshAndRoadMask) {
    Spline sp;
    sp.AddPoint({0, 0, 0});
    sp.AddPoint({10, 0, 0});
    sp.AddPoint({20, 0, 0});
    // 2 segments x 4 cross-sections each, plus the end: 9, two vertices each.
    const auto mesh = BuildSplineMesh(sp, 1.0f, 4);
    AETHER_CHECK(mesh.size() == 9u * 2u * 8u);
    // Every cross-section is filled in (none left at the origin past the start), ending at the last point.
    for (usize v = 2; v < 18; ++v) AETHER_CHECK(mesh[v * 8 + 0] > 0.5f);
    AETHER_CHECK(NearEqual(mesh[16 * 8 + 0], 20.0f, 0.5f));
    AETHER_CHECK(NearEqual(mesh[17 * 8 + 7], 1.0f, 1e-5f)); // v at the end
    // The mask matches the mesh's vertices; 5 m dashes over 20 m: on, off, on, off...
    const auto mask = BuildRoadMask(sp, 4, 5.0f);
    AETHER_CHECK(mask.size() == 9u * 2u * 2u);
    AETHER_CHECK(mask[0] == 1.0f && mask[2] == 1.0f);       // the start is painted, on both sides
    AETHER_CHECK(NearEqual(mask[mask.size() - 1], 1.0f, 1e-5f)); // distance along reaches 1
    bool gap = false;
    for (usize i = 0; i < mask.size(); i += 4) gap = gap || mask[i] == 0.0f;
    AETHER_CHECK(gap);
}

AETHER_TEST(Spline_TangentFollowsCurve) {
    Spline sp;
    sp.AddPoint({0, 0, 0});
    sp.AddPoint({10, 0, 0});
    sp.AddPoint({10, 0, 10});
    sp.AddPoint({0, 0, 10});

    // The tangent must point where the curve actually goes. The old code
    // returned the segment's chord, constant over each segment and tens of
    // degrees away from the real direction on a bend like this one.
    const f32 h = 1e-3f;
    for (const f32 t : {0.13f, 0.29f, 0.42f, 0.61f, 0.78f, 0.91f}) {
        const Vec3 numeric = (sp.Evaluate(t + h) - sp.Evaluate(t - h)).Normalized();
        const Vec3 tangent = sp.Tangent(t);
        AETHER_CHECK(NearEqual(tangent.Length(), 1.0f, 1e-4f));
        AETHER_CHECK(tangent.Dot(numeric) > 0.999f);
    }
}

AETHER_TEST(Spline_FrameVertical) {
    Spline sp;
    sp.AddPoint({0, 0, 0});
    sp.AddPoint({0, 10, 0});
    sp.AddPoint({0, 20, 0});

    Vec3 tangent{1, 0, 0}, up{0, 1, 0}, right{1, 0, 0};
    sp.Frame(0.5f, &tangent, &up, &right);

    AETHER_CHECK(NearEqual(tangent.Length(), 1.0f, 1e-4f));
    AETHER_CHECK(NearEqual(up.Length(), 1.0f, 1e-4f));
    AETHER_CHECK(NearEqual(right.Length(), 1.0f, 1e-4f));
    // Orthonormal even though the tangent is parallel to world up (the old
    // cross product with +Y was zero and collapsed the frame).
    AETHER_CHECK(NearEqual(tangent.Dot(up), 0.0f, 1e-4f));
    AETHER_CHECK(NearEqual(tangent.Dot(right), 0.0f, 1e-4f));
    AETHER_CHECK(NearEqual(up.Dot(right), 0.0f, 1e-4f));
    AETHER_CHECK(tangent.Dot(Vec3(0, 1, 0)) > 0.999f);
}

AETHER_TEST(Spline_RollIsDegrees) {
    Spline sp;
    sp.AddPoint({0, 0, 0});
    sp.AddPoint({10, 0, 0});
    sp.SetRoll(0, 90.0f);
    sp.SetRoll(1, 90.0f);

    Vec3 tangent{1, 0, 0}, up{0, 1, 0}, right{1, 0, 0};
    sp.Frame(0.5f, &tangent, &up, &right);
    // A quarter turn around +X sends up to +Z and right to -Y. Roll is
    // documented in degrees, and the editor sets it in degrees; treating it
    // as radians turned 90 into 90 rad.
    AETHER_CHECK(NearEqual(up, Vec3(0, 0, 1), 1e-4f));
    AETHER_CHECK(NearEqual(right, Vec3(0, -1, 0), 1e-4f));
}

AETHER_TEST(Spline_SampleDegenerate) {
    // One point: evaluating anywhere gives it back.
    Spline one;
    one.AddPoint({3, 4, 5});
    const auto single = one.SampleUniform(1.0f);
    AETHER_CHECK(single.size() == 1);
    AETHER_CHECK(NearEqual(single[0], Vec3(3, 4, 5), 1e-5f));

    // Two coincident points span no distance: one finite sample, not 0/0.
    Spline still;
    still.AddPoint({1, 2, 3});
    still.AddPoint({1, 2, 3});
    const auto flat = still.SampleUniform(1.0f);
    AETHER_CHECK(flat.size() == 1);
    AETHER_CHECK(std::isfinite(flat[0].x) && std::isfinite(flat[0].y) && std::isfinite(flat[0].z));

    // A zero or negative step has no interval, and a tiny one must not ask
    // for an unbounded allocation.
    Spline line;
    line.AddPoint({0, 0, 0});
    line.AddPoint({10, 0, 0});
    AETHER_CHECK(line.SampleUniform(0.0f).size() == 1);
    AETHER_CHECK(line.SampleUniform(-3.0f).size() == 1);
    AETHER_CHECK(line.SampleUniform(1e-9f).size() <= 1'000'000);

    // A step longer than the spline still gives both ends.
    const auto ends = line.SampleUniform(100.0f);
    AETHER_CHECK(ends.size() == 2);
    AETHER_CHECK(NearEqual(ends[0], Vec3(0, 0, 0), 1e-5f));
    AETHER_CHECK(NearEqual(ends[1], Vec3(10, 0, 0), 1e-5f));
}

AETHER_TEST(Spline_LengthOfWindingCurve) {
    // A zigzag of many short segments. The old length used 16 chords over the
    // whole spline and cut across the turns, reporting far less than the
    // curve is long.
    Spline sp;
    for (u32 i = 0; i <= 32; ++i) {
        sp.AddPoint({static_cast<f32>(i) * 5.0f, 0.0f, (i % 2 == 0) ? 10.0f : -10.0f});
    }
    constexpr u32 kFine = 4096;
    f32 fine = 0.0f;
    Vec3 prev = sp.Evaluate(0.0f);
    for (u32 i = 1; i <= kFine; ++i) {
        const Vec3 cur = sp.Evaluate(static_cast<f32>(i) / static_cast<f32>(kFine));
        fine += (cur - prev).Length();
        prev = cur;
    }
    const f32 length = sp.Length();
    AETHER_CHECK(std::fabs(length - fine) / fine < 0.03f);
}
