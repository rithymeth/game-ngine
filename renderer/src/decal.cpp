#include "aether/renderer/decal.h"

#include <algorithm>
#include <cmath>

namespace aether::mat {

Mat4 DecalLocalToWorld(const Mat4& world, const Vec3& size) { return world * Mat4::Scale(size); }

bool DecalScreenRect(const Mat4& local_to_world, const Mat4& view_projection, f32 rect[4]) {
    constexpr f32 kBehind = 1e-4f; // clip w at or below this is on or behind the camera plane
    const Mat4 to_clip = view_projection * local_to_world;
    f32 min_x = 1e30f, min_y = 1e30f, max_x = -1e30f, max_y = -1e30f;
    u32 behind = 0;
    bool left = true, right = true, below = true, above = true; // every projected corner is off that side
    for (u32 corner = 0; corner < 8; ++corner) {
        const Vec4 clip = to_clip * Vec4((corner & 1) ? 0.5f : -0.5f, (corner & 2) ? 0.5f : -0.5f, (corner & 4) ? 0.5f : -0.5f, 1.0f);
        if (clip.w <= kBehind) {
            ++behind;
            continue;
        }
        const f32 x = clip.x / clip.w, y = clip.y / clip.w;
        min_x = std::min(min_x, x), max_x = std::max(max_x, x);
        min_y = std::min(min_y, y), max_y = std::max(max_y, y);
        left = left && x < -1.0f;
        right = right && x > 1.0f;
        below = below && y < -1.0f;
        above = above && y > 1.0f;
    }
    if (behind == 8) return false; // all behind the camera
    if (behind > 0) { // straddles the camera plane: the projection of the front part reaches the edges
        rect[0] = -1.0f, rect[1] = -1.0f, rect[2] = 1.0f, rect[3] = 1.0f;
        return true;
    }
    if (left || right || below || above) return false;
    rect[0] = std::max(min_x, -1.0f);
    rect[1] = std::max(min_y, -1.0f);
    rect[2] = std::min(max_x, 1.0f);
    rect[3] = std::min(max_y, 1.0f);
    return rect[0] < rect[2] && rect[1] < rect[3];
}

void BuildDecalDraws(const std::vector<DecalInput>& inputs, const Mat4& view_projection, const Vec3& camera_position, std::vector<DecalDraw>& out) {
    out.clear();
    Mat4 inverse_view_projection;
    if (!view_projection.TryInverse(inverse_view_projection)) return;
    struct Keyed {
        DecalDraw draw;
        i32 order;
    };
    std::vector<Keyed> keyed;
    for (const DecalInput& input : inputs) {
        if (!(input.size.x > 0.0f && input.size.y > 0.0f && input.size.z > 0.0f)) continue;
        const Mat4 local_to_world = DecalLocalToWorld(input.world, input.size);
        Mat4 world_to_unit;
        if (!local_to_world.TryInverse(world_to_unit)) continue;
        DecalDraw draw;
        if (!DecalScreenRect(local_to_world, view_projection, draw.rect)) continue;
        draw.clip_to_unit = world_to_unit * inverse_view_projection;
        draw.size = input.size;
        draw.id = input.id;
        const Vec3 centre{local_to_world.cols[3].x, local_to_world.cols[3].y, local_to_world.cols[3].z};
        const f32 dx = centre.x - camera_position.x, dy = centre.y - camera_position.y, dz = centre.z - camera_position.z;
        draw.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        keyed.push_back({draw, input.sort_order});
    }
    std::stable_sort(keyed.begin(), keyed.end(), [](const Keyed& a, const Keyed& b) {
        if (a.order != b.order) return a.order < b.order;
        return a.draw.distance > b.draw.distance; // far to near
    });
    for (const Keyed& k : keyed) out.push_back(k.draw);
}

Vec3 DecalUnitPosition(const DecalDraw& draw, f32 ndc_x, f32 ndc_y, f32 depth) {
    const Vec4 p = draw.clip_to_unit * Vec4(ndc_x, ndc_y, depth, 1.0f);
    return Vec3(p.x / p.w, p.y / p.w, p.z / p.w);
}

} // namespace aether::mat
