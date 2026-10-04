#include "aether/sprite2d/pixel_camera.h"

#include <algorithm>
#include <cmath>

namespace aether::sprite2d {

PixelViewport ComputePixelViewport(const PixelPerfectCamera& camera, u32 window_width, u32 window_height) {
    PixelViewport v;
    const u32 rw = std::max(camera.reference_width, 1u);
    const u32 rh = std::max(camera.reference_height, 1u);
    const f32 ppu = camera.pixels_per_unit > 0.0f ? camera.pixels_per_unit : 1.0f;
    v.scale = std::max(1u, std::min(window_width / rw, window_height / rh));
    if (camera.fill_window) {
        v.width = window_width;
        v.height = window_height;
        // Shows however many whole art pixels fit, rounding up at the edge.
        v.ortho_width = static_cast<f32>(window_width) / static_cast<f32>(v.scale) / ppu;
        v.ortho_height = static_cast<f32>(window_height) / static_cast<f32>(v.scale) / ppu;
        return v;
    }
    v.width = rw * v.scale;
    v.height = rh * v.scale;
    v.x = (static_cast<i32>(window_width) - static_cast<i32>(v.width)) / 2;
    v.y = (static_cast<i32>(window_height) - static_cast<i32>(v.height)) / 2;
    v.ortho_width = static_cast<f32>(rw) / ppu;
    v.ortho_height = static_cast<f32>(rh) / ppu;
    return v;
}

Vec3 SnapToPixelGrid(const Vec3& position, f32 pixels_per_unit) {
    if (pixels_per_unit <= 0.0f) return position;
    return Vec3(std::round(position.x * pixels_per_unit) / pixels_per_unit,
                std::round(position.y * pixels_per_unit) / pixels_per_unit, position.z);
}

} // namespace aether::sprite2d
