#include "test_framework.h"

#include "aether/cook/cluster_reference.h"

#include <cfloat>
#include <cmath>
#include <limits>

// The CPU reference for cooked-LOD selection and meshlet frustum culling (cluster_reference.h): the checks its design doc (docs/design/CLUSTER_GEOMETRY_LOD.md)
// asks for before any GPU work builds on it. It picks the coarsest whole-primitive LOD whose error fits the pixel budget and rejects meshlet spheres that
// are outside the frustum.

using namespace aether;
using namespace aether::cook;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {


ClusterReferenceCamera CameraAt(float z, float max_error = 2.0f) {
    ClusterReferenceCamera c;
    c.position = Vec3(0, 0, z);
    c.vertical_fov_radians = kPi / 3.0f;
    c.viewport_height_pixels = 1080.0f;
    c.maximum_error_pixels = max_error;
    return c;
}

MeshletBounds Sphere(float x, float radius) {
    MeshletBounds b;
    b.center = Vec3(x, 0, 0);
    b.radius = radius;
    return b;
}

// A primitive in the cube [-1, 1]^3 (centre 0, bounding radius sqrt(3)) with LODs of the given relative errors; lod 0 has three meshlets at x = -10, 0, 10.
CookedMeshPrimitive Primitive(std::initializer_list<float> errors) {
    CookedMeshPrimitive p;
    p.bounds_min = Vec3(-1, -1, -1);
    p.bounds_max = Vec3(1, 1, 1);
    for (const float e : errors) {
        CookedMeshLod lod;
        lod.error = e;
        if (p.lods.empty()) {
            for (const float x : {-10.0f, 0.0f, 10.0f}) {
                lod.meshlets.meshlets.push_back(Meshlet{});
                lod.meshlets.bounds.push_back(Sphere(x, 1.0f));
            }
        }
        p.lods.push_back(std::move(lod));
    }
    return p;
}

} // namespace

AETHER_TEST(ClusterReference_ProjectedErrorScalesAndRefusesBadInput) {
    const ClusterReferenceCamera cam = CameraAt(0.0f);
    // error * height / (2 * distance * tan(fov / 2)): 1 m at 10 m, 60 degrees, 1080 px is 1080 / (20 * 0.57735) = 93.5 px.
    const float px = ProjectedClusterErrorPixels(1.0f, 10.0f, cam);
    CHECK(std::fabs(px - 93.53f) < 0.05f);
    CHECK(std::fabs(ProjectedClusterErrorPixels(2.0f, 10.0f, cam) - 2.0f * px) < 1e-3f);   // twice the error: twice the pixels
    CHECK(std::fabs(ProjectedClusterErrorPixels(1.0f, 20.0f, cam) - 0.5f * px) < 1e-3f);   // twice as far: half
    CHECK(ProjectedClusterErrorPixels(0.0f, 10.0f, cam) == 0.0f);                            // no error is no pixels, however near
    CHECK(ProjectedClusterErrorPixels(0.0f, 0.0f, cam) == 0.0f);
    // At distance 0 (inside the sphere) it is held to a tiny distance: huge, but finite.
    const float inside = ProjectedClusterErrorPixels(0.01f, 0.0f, cam);
    CHECK(std::isfinite(inside) && inside > 1.0e4f);
    CHECK(ProjectedClusterErrorPixels(0.01f, -5.0f, cam) == inside); // a negative distance is the same
    // Anything that is not a usable error or camera is infinitely bad (never "fits the budget").
    const float inf = std::numeric_limits<float>::infinity();
    CHECK(ProjectedClusterErrorPixels(-1.0f, 10.0f, cam) == inf && ProjectedClusterErrorPixels(std::nanf(""), 10.0f, cam) == inf && ProjectedClusterErrorPixels(inf, 10.0f, cam) == inf);
    ClusterReferenceCamera bad = cam;
    bad.viewport_height_pixels = 0.0f;
    CHECK(ProjectedClusterErrorPixels(1.0f, 10.0f, bad) == inf);
    bad = cam;
    bad.viewport_height_pixels = inf;
    CHECK(ProjectedClusterErrorPixels(1.0f, 10.0f, bad) == inf);
    for (const float fov : {0.0f, -1.0f, kPi, 4.0f, std::nanf("")}) {
        bad = cam;
        bad.vertical_fov_radians = fov;
        CHECK(ProjectedClusterErrorPixels(1.0f, 10.0f, bad) == inf);
    }
}

AETHER_TEST(ClusterReference_TheCoarsestFittingLodIsChosenByDistanceAndBudget) {
    // Extent 2: the LODs' errors are 0, 0.01 (0.02 m) and 0.04 (0.08 m). At a 2 px budget a 60 degree, 1080 px camera needs about 9.4 m of clear distance for the
    // second and 37.4 m for the third; the cube's bounding radius (1.73) comes off the camera distance first.
    const CookedMeshPrimitive p = Primitive({0.0f, 0.01f, 0.04f});
    CHECK(SelectReferenceLod(p, 2.0f, CameraAt(5.0f)) == 0);     // too close for any coarser level
    CHECK(SelectReferenceLod(p, 2.0f, CameraAt(10.0f)) == 0);    // 8.3 m of clear distance: still short of 9.4
    CHECK(SelectReferenceLod(p, 2.0f, CameraAt(12.0f)) == 1);    // 10.3 m: the middle level fits
    CHECK(SelectReferenceLod(p, 2.0f, CameraAt(38.0f)) == 1);    // 36.3 m: not yet the coarsest
    CHECK(SelectReferenceLod(p, 2.0f, CameraAt(40.0f)) == 2);    // 38.3 m
    CHECK(SelectReferenceLod(p, 2.0f, CameraAt(5000.0f)) == 2);
    // Never coarser when closer.
    u32 previous = 2;
    for (const float z : {5000.0f, 500.0f, 100.0f, 40.0f, 38.0f, 12.0f, 10.0f, 5.0f, 2.0f}) {
        const u32 lod = SelectReferenceLod(p, 2.0f, CameraAt(z));
        CHECK(lod <= previous);
        previous = lod;
    }
    // A bigger pixel budget accepts a coarser level at the same distance; a zero budget accepts only a lossless one.
    CHECK(SelectReferenceLod(p, 2.0f, CameraAt(12.0f, 20.0f)) == 2);
    CHECK(SelectReferenceLod(p, 2.0f, CameraAt(5000.0f, 0.0f)) == 0);
    // Inside the bounds there is no clear distance at all: full detail.
    CHECK(SelectReferenceLod(p, 2.0f, CameraAt(0.5f)) == 0);
}

AETHER_TEST(ClusterReference_LodSelectionChecksEachCandidateAndRefusesBadInput) {
    // Errors need not grow level to level: the highest level that fits on its own is the one chosen.
    const CookedMeshPrimitive odd = Primitive({0.0f, 0.04f, 0.01f});
    CHECK(SelectReferenceLod(odd, 2.0f, CameraAt(12.0f)) == 2); // level 1 (0.08 m) is too coarse here, level 2 (0.02 m) fits
    CHECK(SelectReferenceLod(odd, 2.0f, CameraAt(40.0f)) == 2);
    CookedMeshPrimitive none;
    CHECK(SelectReferenceLod(none, 2.0f, CameraAt(100.0f)) == 0);                        // no LODs at all
    CHECK(SelectReferenceLod(Primitive({0.0f}), 2.0f, CameraAt(100.0f)) == 0);           // only the source
    const CookedMeshPrimitive p = Primitive({0.0f, 0.01f, 0.04f});
    CHECK(SelectReferenceLod(p, std::nanf(""), CameraAt(5000.0f)) == 0);
    CHECK(SelectReferenceLod(p, -1.0f, CameraAt(5000.0f)) == 0);
    CHECK(SelectReferenceLod(p, std::numeric_limits<float>::infinity(), CameraAt(5000.0f)) == 0);
    CHECK(SelectReferenceLod(p, 2.0f, CameraAt(5000.0f, std::nanf(""))) == 0);
    CHECK(SelectReferenceLod(p, 2.0f, CameraAt(5000.0f, -1.0f)) == 0);
    ClusterReferenceCamera bad = CameraAt(5000.0f);
    bad.viewport_height_pixels = 0.0f;
    CHECK(SelectReferenceLod(p, 2.0f, bad) == 0); // an unusable camera never claims a coarse level fits
    bad = CameraAt(5000.0f);
    bad.vertical_fov_radians = 0.0f;
    CHECK(SelectReferenceLod(p, 2.0f, bad) == 0);
}

AETHER_TEST(ClusterReference_SphereFrustumTestIsConservativeAndHandlesAwkwardPlanes) {
    const MeshletBounds unit = Sphere(0.0f, 1.0f);
    const auto plane = [](float nx, float ny, float nz, float d) { return ClusterReferencePlane{Vec3(nx, ny, nz), d}; };
    CHECK(ClusterSphereInFrustum(unit, {}));                                   // no planes: nothing rejects it
    CHECK(ClusterSphereInFrustum(unit, {plane(1, 0, 0, 0.0f)}));               // centre on the plane
    CHECK(ClusterSphereInFrustum(unit, {plane(1, 0, 0, -1.0f)}));              // tangent from outside: kept (conservative)
    CHECK(!ClusterSphereInFrustum(unit, {plane(1, 0, 0, -1.01f)}));            // just wholly outside
    CHECK(ClusterSphereInFrustum(unit, {plane(3, 0, 0, -3.0f)}));              // a plane's normal need not be unit length
    CHECK(!ClusterSphereInFrustum(unit, {plane(3, 0, 0, -3.1f)}));
    CHECK(!ClusterSphereInFrustum(unit, {plane(0, 0, 0.5f, -0.51f)}));         // ... however short
    CHECK(!ClusterSphereInFrustum(unit, {plane(1, 0, 0, 5.0f), plane(-1, 0, 0, -3.0f)})); // inside the first plane, outside the second
    CHECK(ClusterSphereInFrustum(unit, {plane(1, 0, 0, 5.0f), plane(-1, 0, 0, 5.0f), plane(0, 1, 0, 5.0f)}));
}

AETHER_TEST(ClusterReference_CullKeepsTheMeshletsInViewOfTheChosenLod) {
    const CookedMeshPrimitive p = Primitive({0.0f, 0.01f, 0.04f});
    // The meshlets belong to LOD 0 (the others have none): near the object, LOD 0 is chosen and the frustum x >= -5 drops the one at x = -10.
    const std::vector<ClusterReferencePlane> right_of_minus5 = {ClusterReferencePlane{Vec3(1, 0, 0), 5.0f}};
    ClusterReferenceSelection near_sel = CullReferenceMeshlets(p, 2.0f, CameraAt(5.0f), right_of_minus5);
    CHECK(near_sel.lod_index == 0 && near_sel.visible_meshlets.size() == 2 && near_sel.visible_meshlets[0] == 1 && near_sel.visible_meshlets[1] == 2);
    // No planes: all three. A frustum that excludes everything: none.
    CHECK(CullReferenceMeshlets(p, 2.0f, CameraAt(5.0f), {}).visible_meshlets.size() == 3);
    const std::vector<ClusterReferencePlane> nothing = {ClusterReferencePlane{Vec3(1, 0, 0), -100.0f}};
    CHECK(CullReferenceMeshlets(p, 2.0f, CameraAt(5.0f), nothing).visible_meshlets.empty());
    // Far away a coarser LOD is chosen, and it has no meshlets in this fixture: the answer is its (empty) list, not LOD 0's.
    ClusterReferenceSelection far_sel = CullReferenceMeshlets(p, 2.0f, CameraAt(5000.0f), {});
    CHECK(far_sel.lod_index == 2 && far_sel.visible_meshlets.empty());
    // No LODs: nothing, and no crash.
    CookedMeshPrimitive none;
    ClusterReferenceSelection empty = CullReferenceMeshlets(none, 2.0f, CameraAt(5.0f), {});
    CHECK(empty.visible_meshlets.empty() && empty.lod_index == 0);
    // Fewer bounds than meshlets (malformed): only the ones that have bounds are considered.
    CookedMeshPrimitive ragged = Primitive({0.0f});
    ragged.lods[0].meshlets.bounds.pop_back();
    CHECK(CullReferenceMeshlets(ragged, 2.0f, CameraAt(5.0f), {}).visible_meshlets.size() == 2);
}
