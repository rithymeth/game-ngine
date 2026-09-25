#include "aether/math/math.h"
#include "test_framework.h"

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
