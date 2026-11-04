#pragma once

#include "aether/math/math.h"

#include <vector>

namespace aether::mat {

// Decals (Phase 39.4), the CPU side. A decal is a box; the pass draws it as one screen-space rectangle, and for each pixel in it reads the
// scene depth, rebuilds where that pixel's surface is, and paints the decal there when the point is inside the box. So the same pixel is
// never blended twice (front and back faces of a box would), and the camera may be inside the box.
//
// The shader needs, per decal: the rectangle (in normalised device coordinates), and one matrix that takes a pixel's (ndc.x, ndc.y, depth, 1)
// straight to the decal's unit box coordinates (-0.5..0.5 on every axis, +Z the projection axis): clip_to_unit = inverse(box to world) *
// inverse(view projection). `depth` is what the depth buffer holds, clip z / clip w.
struct DecalInput {
    Mat4 world;        // the entity's world transform (its scale stretches the box too: the box is `size` times that)
    Vec3 size{1, 1, 1};
    i32 sort_order = 0;
    u32 id = 0;        // the caller's index, returned in DecalDraw::id
};

struct DecalDraw {
    Mat4 clip_to_unit;
    f32 rect[4] = {-1.0f, -1.0f, 1.0f, 1.0f}; // ndc min x, min y, max x, max y
    Vec3 size{1, 1, 1};
    u32 id = 0;
    f32 distance = 0.0f; // camera to the box centre
};

// The matrix from the unit box (-0.5..0.5) to the world: the transform with `size` applied on top, so a decal's world scale and its size
// both stretch it.
Mat4 DecalLocalToWorld(const Mat4& world, const Vec3& size);

// The ndc rectangle (min x, min y, max x, max y) that covers the box's projection, clamped to the screen. A box partly behind the camera
// covers the whole screen; one entirely behind it, or entirely off to a side, gives false.
bool DecalScreenRect(const Mat4& local_to_world, const Mat4& view_projection, f32 rect[4]);

// The draws for a frame: decals that are visible, ordered by sort_order and then far to near. `out` is cleared first. A decal whose box can't
// be inverted (a zero size) is skipped.
void BuildDecalDraws(const std::vector<DecalInput>& inputs, const Mat4& view_projection, const Vec3& camera_position, std::vector<DecalDraw>& out);

// The unit-box coordinates of the world point a pixel shows: what the shader computes for (ndc, depth). For tests and tools.
Vec3 DecalUnitPosition(const DecalDraw& draw, f32 ndc_x, f32 ndc_y, f32 depth);

} // namespace aether::mat
