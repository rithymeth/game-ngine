#include "../player/src/view_cull.h"
#include "test_framework.h"

using namespace aether;
using namespace aether::player;

// Leaving out the draws the camera cannot see: a camera at the origin looking down -z, 90 degrees tall, 1:1, near 0.1, far 100.

static ViewFrustum Camera(f32 viewport_height = 0.0f) {
    const Mat4 projection = Mat4::PerspectiveRH(1.5707964f, 1.0f, 0.1f, 100.0f);
    const Mat4 view = Mat4::LookAtRH(Vec3(0, 0, 0), Vec3(0, 0, -1), Vec3(0, 1, 0));
    return MakeViewFrustum(projection * view, projection.cols[1].y, viewport_height);
}

AETHER_TEST(ViewCull_AnObjectInFrontIsVisibleAndOneBehindIsNot) {
    const ViewFrustum f = Camera();
    AETHER_CHECK(SphereVisible(f, Vec3(0, 0, -10), 1.0f));
    AETHER_CHECK(!SphereVisible(f, Vec3(0, 0, 10), 1.0f));   // behind the camera
    AETHER_CHECK(!SphereVisible(f, Vec3(0, 0, -200), 1.0f)); // past the far plane
}

AETHER_TEST(ViewCull_TheSidesAreTheFieldOfView) {
    const ViewFrustum f = Camera();
    // At 10 m away the view is 20 m wide (90 degrees): +-10 m.
    AETHER_CHECK(SphereVisible(f, Vec3(9, 0, -10), 0.5f));
    AETHER_CHECK(!SphereVisible(f, Vec3(14, 0, -10), 0.5f));
    AETHER_CHECK(!SphereVisible(f, Vec3(-14, 0, -10), 0.5f));
    AETHER_CHECK(!SphereVisible(f, Vec3(0, 14, -10), 0.5f));
    AETHER_CHECK(!SphereVisible(f, Vec3(0, -14, -10), 0.5f));
    // A big sphere centred outside still reaches in.
    AETHER_CHECK(SphereVisible(f, Vec3(14, 0, -10), 6.0f));
}

AETHER_TEST(ViewCull_ASphereAcrossAnEdgeIsVisible) {
    const ViewFrustum f = Camera();
    // Centred exactly on the right edge at 10 m: half inside.
    AETHER_CHECK(SphereVisible(f, Vec3(10, 0, -10), 0.1f));
    // Straddling the near plane.
    AETHER_CHECK(SphereVisible(f, Vec3(0, 0, 0.0f), 0.5f));
}

AETHER_TEST(ViewCull_ASphereTooSmallToCoverAPixelIsLeftOut) {
    const ViewFrustum f = Camera(1080.0f);
    // A 10 cm sphere at 5 m is 1080 / 2 * 0.1 / 5 = 10.8 px in radius; at 500 m (past the far plane, so use a longer view) it would be a tenth of a pixel.
    AETHER_CHECK(SphereVisible(f, Vec3(0, 0, -5), 0.1f, 0.25f));
    const Mat4 projection = Mat4::PerspectiveRH(1.5707964f, 1.0f, 0.1f, 5000.0f);
    const Mat4 view = Mat4::LookAtRH(Vec3(0, 0, 0), Vec3(0, 0, -1), Vec3(0, 1, 0));
    const ViewFrustum far_view = MakeViewFrustum(projection * view, projection.cols[1].y, 1080.0f);
    AETHER_CHECK(!SphereVisible(far_view, Vec3(0, 0, -2000), 0.1f, 0.25f)); // 1080 / 2 * 0.1 / 2000 = 0.027 px
    AETHER_CHECK(SphereVisible(far_view, Vec3(0, 0, -2000), 0.1f, 0.0f));   // the test off
    AETHER_CHECK(SphereVisible(far_view, Vec3(0, 0, -2000), 100.0f, 0.25f)); // a mountain is not too small
}

AETHER_TEST(ViewCull_AMovedCameraCullsWhatItNowFaces) {
    const Mat4 projection = Mat4::PerspectiveRH(1.5707964f, 1.0f, 0.1f, 100.0f);
    // Turned round to look down +z: what was behind is now ahead.
    const Mat4 view = Mat4::LookAtRH(Vec3(0, 0, 0), Vec3(0, 0, 1), Vec3(0, 1, 0));
    const ViewFrustum f = MakeViewFrustum(projection * view);
    AETHER_CHECK(SphereVisible(f, Vec3(0, 0, 10), 1.0f));
    AETHER_CHECK(!SphereVisible(f, Vec3(0, 0, -10), 1.0f));
}
