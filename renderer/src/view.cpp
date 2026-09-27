#include "aether/renderer/view.h"

#include "aether/scene/hierarchy.h"

#include <cmath>

namespace aether {

namespace {

// Row `i` of a column-major matrix.
Vec4 Row(const Mat4& m, int i) {
    auto at = [&](int c) { const Vec4& col = m.cols[c]; return i == 0 ? col.x : i == 1 ? col.y : i == 2 ? col.z : col.w; };
    return Vec4(at(0), at(1), at(2), at(3));
}

Plane MakePlane(const Vec4& v) {
    const Vec3 n(v.x, v.y, v.z);
    const f32 len = n.Length();
    return len > 0.0f ? Plane{n * (1.0f / len), v.w / len} : Plane{};
}

} // namespace

Frustum Frustum::FromViewProjection(const Mat4& m) {
    // Gribb & Hartmann, for clip space x, y in [-w, w] and z in [0, w].
    const Vec4 r0 = Row(m, 0), r1 = Row(m, 1), r2 = Row(m, 2), r3 = Row(m, 3);
    Frustum f;
    f.planes[0] = MakePlane(r3 + r0); // left
    f.planes[1] = MakePlane(r3 - r0); // right
    f.planes[2] = MakePlane(r3 + r1); // bottom
    f.planes[3] = MakePlane(r3 - r1); // top
    f.planes[4] = MakePlane(r2);      // near
    f.planes[5] = MakePlane(r3 - r2); // far
    return f;
}

bool Frustum::Contains(const Vec3& p) const {
    for (const Plane& plane : planes) {
        if (plane.Distance(p) < 0.0f) return false;
    }
    return true;
}

bool Frustum::Intersects(const Aabb& box) const {
    // The box corner furthest along each plane's normal must be inside it.
    for (const Plane& plane : planes) {
        const Vec3 p(plane.normal.x >= 0 ? box.max.x : box.min.x, plane.normal.y >= 0 ? box.max.y : box.min.y,
                     plane.normal.z >= 0 ? box.max.z : box.min.z);
        if (plane.Distance(p) < 0.0f) return false;
    }
    return true;
}

bool Frustum::IntersectsSphere(const Vec3& center, f32 radius) const {
    for (const Plane& plane : planes) {
        if (plane.Distance(center) < -radius) return false;
    }
    return true;
}

View MakeView(const Camera& camera, const Mat4& world_transform, f32 aspect) {
    View v;
    v.aspect = aspect > 0.0f ? aspect : 1.0f;
    v.perspective = camera.projection == Projection::Perspective;
    v.fov_degrees = camera.fov_degrees;
    v.ortho_height = camera.ortho_height;
    v.near_plane = camera.near_plane;
    v.far_plane = camera.far_plane;
    v.projection = CameraProjection(camera, v.aspect);
    // The view matrix inverts a rotation + translation: transpose the rotation.
    const Vec4& c0 = world_transform.cols[0];
    const Vec4& c1 = world_transform.cols[1];
    const Vec4& c2 = world_transform.cols[2];
    const Vec4& t = world_transform.cols[3];
    v.position = Vec3(t.x, t.y, t.z);
    Mat4 view;
    view.cols[0] = Vec4(c0.x, c1.x, c2.x, 0);
    view.cols[1] = Vec4(c0.y, c1.y, c2.y, 0);
    view.cols[2] = Vec4(c0.z, c1.z, c2.z, 0);
    const Vec3 r0(c0.x, c0.y, c0.z), r1(c1.x, c1.y, c1.z), r2(c2.x, c2.y, c2.z);
    view.cols[3] = Vec4(-r0.Dot(v.position), -r1.Dot(v.position), -r2.Dot(v.position), 1);
    v.view = view;
    v.forward = Vec3(-c2.x, -c2.y, -c2.z).Normalized();
    v.view_projection = v.projection * v.view;
    v.frustum = Frustum::FromViewProjection(v.view_projection);
    return v;
}

bool MakeViewFromActiveCamera(const World& world, const GuidIndex& guids, f32 aspect, View& out) {
    const Entity camera = FindActiveCamera(world, guids);
    if (camera.IsNull()) return false;
    out = MakeView(*world.GetComponent<Camera>(camera), ComputeWorldTransform(world, guids, camera), aspect);
    return true;
}

} // namespace aether
