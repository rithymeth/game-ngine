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
