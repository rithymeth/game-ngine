#pragma once

#include "aether/renderer/render_scene.h"
#include "aether/scene/gameplay.h"

namespace aether {

// A plane n·p + d = 0, with n pointing into the kept half.
struct Plane {
    Vec3 normal{0, 1, 0};
    f32 d = 0.0f;
    f32 Distance(const Vec3& p) const { return normal.Dot(p) + d; }
};

// The six planes of a view volume (left, right, bottom, top, near, far),
// pointing inward. From a view-projection matrix with [0, 1] depth.
struct Frustum {
    Plane planes[6];
    static Frustum FromViewProjection(const Mat4& view_projection);
    bool Contains(const Vec3& point) const;
    bool Intersects(const Aabb& box) const; // conservative: may keep boxes just outside a corner
    bool IntersectsSphere(const Vec3& center, f32 radius) const;
};

// Everything the renderer needs to know about one viewpoint.
struct View {
    Mat4 view, projection, view_projection;
    Vec3 position{0, 0, 0};
    Vec3 forward{0, 0, -1};
    f32 near_plane = 0.1f, far_plane = 1000.0f;
    f32 fov_degrees = 60.0f; // perspective
    f32 aspect = 16.0f / 9.0f;
    bool perspective = true;
    Frustum frustum;
};

// A view from a camera at `world_transform` (rotation + translation).
View MakeView(const Camera& camera, const Mat4& world_transform, f32 aspect);
// The world's active camera (FindActiveCamera). False if there's none.
bool MakeViewFromActiveCamera(const World& world, const GuidIndex& guids, f32 aspect, View& out);

} // namespace aether
