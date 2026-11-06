#include "aether/math/math.h"
#include "aether/math/world_origin.h"
#include "test_framework.h"

#include "aether/math/frustum.h"

using namespace aether;

AETHER_TEST(Vec3_DotAndCross) {
    Vec3 a(1, 0, 0);
    Vec3 b(0, 1, 0);

    AETHER_CHECK_NEAR(a.Dot(b), 0.0f, 1e-6);
    Vec3 c = a.Cross(b);
    AETHER_CHECK_NEAR(c.x, 0.0f, 1e-6);
    AETHER_CHECK_NEAR(c.y, 0.0f, 1e-6);
    AETHER_CHECK_NEAR(c.z, 1.0f, 1e-6);
}

AETHER_TEST(Vec3_Normalize) {
    Vec3 v(3, 4, 0);
    Vec3 n = v.Normalized();
    AETHER_CHECK_NEAR(n.Length(), 1.0f, 1e-5);
}

AETHER_TEST(Vec4_SimdDot) {
    Vec4 a(1, 2, 3, 4);
    Vec4 b(4, 3, 2, 1);
    AETHER_CHECK_NEAR(a.Dot(b), 1*4 + 2*3 + 3*2 + 4*1, 1e-5);
}

AETHER_TEST(Mat4_IdentityIsNeutral) {
    Mat4 id = Mat4::Identity();
    Vec4 v(1, 2, 3, 1);
    Vec4 r = id * v;
    AETHER_CHECK_NEAR(r.x, 1.0f, 1e-6);
    AETHER_CHECK_NEAR(r.y, 2.0f, 1e-6);
    AETHER_CHECK_NEAR(r.z, 3.0f, 1e-6);
    AETHER_CHECK_NEAR(r.w, 1.0f, 1e-6);
}

AETHER_TEST(Mat4_TranslationMovesPoint) {
    Mat4 t = Mat4::Translation(Vec3(10, 20, 30));
    Vec4 p(1, 1, 1, 1);
    Vec4 r = t * p;
    AETHER_CHECK_NEAR(r.x, 11.0f, 1e-5);
    AETHER_CHECK_NEAR(r.y, 21.0f, 1e-5);
    AETHER_CHECK_NEAR(r.z, 31.0f, 1e-5);
}

AETHER_TEST(Mat4_MultiplyComposesTransforms) {
    Mat4 t = Mat4::Translation(Vec3(1, 0, 0));
    Mat4 s = Mat4::Scale(Vec3(2, 2, 2));
    Mat4 combined = t * s; // scale first, then translate

    Vec4 p(1, 1, 1, 1);
    Vec4 r = combined * p;
    AETHER_CHECK_NEAR(r.x, 3.0f, 1e-5); // (1*2) + 1
    AETHER_CHECK_NEAR(r.y, 2.0f, 1e-5);
    AETHER_CHECK_NEAR(r.z, 2.0f, 1e-5);
}

AETHER_TEST(Quaternion_IdentityRotationIsNoOp) {
    Quaternion q = Quaternion::Identity();
    Mat4 m = q.ToMat4();
    Vec4 p(1, 2, 3, 1);
    Vec4 r = m * p;
    AETHER_CHECK_NEAR(r.x, 1.0f, 1e-5);
    AETHER_CHECK_NEAR(r.y, 2.0f, 1e-5);
    AETHER_CHECK_NEAR(r.z, 3.0f, 1e-5);
}

AETHER_TEST(Quaternion_90DegreeYawRotatesXToZ) {
    Quaternion q = Quaternion::FromAxisAngle(Vec3(0, 1, 0), Radians(90.0f));
    Mat4 m = q.ToMat4();
    Vec4 p(1, 0, 0, 1);
    Vec4 r = m * p;
    // Right-handed rotation about +Y by 90deg sends +X toward -Z.
    AETHER_CHECK_NEAR(r.x, 0.0f, 1e-4);
    AETHER_CHECK_NEAR(r.y, 0.0f, 1e-4);
    AETHER_CHECK_NEAR(r.z, -1.0f, 1e-4);
}

AETHER_TEST(Quaternion_ComposedRotationIsNormalized) {
    Quaternion a = Quaternion::FromAxisAngle(Vec3(1, 0, 0), Radians(45.0f));
    Quaternion b = Quaternion::FromAxisAngle(Vec3(0, 1, 0), Radians(30.0f));
    Quaternion c = (a * b).Normalized();
    AETHER_CHECK_NEAR(c.Length(), 1.0f, 1e-5);
}

// Phase 42.1: a double-precision position keeps separation that f32 loses.
AETHER_TEST(Vec3d_PrecisionBeyondFloat) {
    const Vec3d a(100000.0, 0.0, 0.0);      // 100 km out
    const Vec3d b(100000.001, 0.0, 0.0);    // 1 mm further
    AETHER_CHECK((b - a).Length() > 0.0009);
    // f32 cannot tell them apart, which is why a world origin is needed.
    AETHER_CHECK(a.ToVec3().x == b.ToVec3().x);
}

// Phase 42.1: the origin turns absolute positions into small f32 ones and back.
AETHER_TEST(WorldOrigin_RebasesIntoRelativeSpace) {
    WorldOrigin origin;
    origin.absolute = Vec3d(100000.0, 0.0, 200000.0);
    const Vec3d world(100003.0, 1.0, 199998.0);

    const Vec3 relative = origin.ToRelative(world);
    AETHER_CHECK_NEAR(relative.x, 3.0f, 1e-4);
    AETHER_CHECK_NEAR(relative.y, 1.0f, 1e-4);
    AETHER_CHECK_NEAR(relative.z, -2.0f, 1e-4);

    const Vec3d back = origin.ToAbsolute(relative);
    AETHER_CHECK_NEAR(back.x, world.x, 1e-6);
    AETHER_CHECK_NEAR(back.y, world.y, 1e-6);
    AETHER_CHECK_NEAR(back.z, world.z, 1e-6);

    // It only moves when the point strays past the threshold.
    AETHER_CHECK(!RebaseOrigin(origin, Vec3d(100010.0, 0.0, 200000.0), 25.0));
    AETHER_CHECK(RebaseOrigin(origin, world, 1.0));
    AETHER_CHECK_NEAR(origin.absolute.x, world.x, 1e-9);
    AETHER_CHECK_NEAR(origin.ToRelative(world).Length(), 0.0f, 1e-6);
}

// Phase 38 step 8: the culling test. A perspective camera at the origin looking down -Z, so nothing
// needs a view matrix to be interesting.
AETHER_TEST(Math_FrustumCullsWhatIsBehindAndOutside) {
    const Mat4 view_projection = Mat4::PerspectiveRH(Radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    const Frustum frustum = Frustum::FromViewProjection(view_projection);

    // In front, and inside the near and far planes.
    AETHER_CHECK(frustum.ContainsSphere(Vec3(0.0f, 0.0f, -5.0f), 0.5f));
    AETHER_CHECK(frustum.ContainsAabb(Vec3(-1.0f, -1.0f, -6.0f), Vec3(1.0f, 1.0f, -4.0f)));

    // Behind the camera, and past the far plane.
    AETHER_CHECK(!frustum.ContainsSphere(Vec3(0.0f, 0.0f, 5.0f), 0.5f));
    AETHER_CHECK(!frustum.ContainsSphere(Vec3(0.0f, 0.0f, -500.0f), 0.5f));

    // Far off to the side.
    AETHER_CHECK(!frustum.ContainsSphere(Vec3(100.0f, 0.0f, -5.0f), 0.5f));
    AETHER_CHECK(!frustum.ContainsAabb(Vec3(10.0f, 10.0f, -6.0f), Vec3(11.0f, 11.0f, -4.0f)));

    // A sphere straddling the near plane is still reported visible: the test is conservative on
    // purpose, and the normals point outwards so dot + distance >= 0 means inside.
    AETHER_CHECK(frustum.ContainsSphere(Vec3(0.0f, 0.0f, 0.0f), 1.0f));
    for (usize i = 0; i < 6; ++i) {
        const Plane& plane = frustum[i];
        AETHER_CHECK_NEAR(plane.normal.Length(), 1.0f, 1e-3f);
    }
}