#include "aether/sprite2d/pixel_camera.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace aether::sprite2d {

namespace {

f32 FiniteFloat(f64 value) {
    const f64 limit = std::numeric_limits<f32>::max();
    return static_cast<f32>(std::clamp(value, -limit, limit));
}

f32 WorldSize(u32 pixels, u32 scale, f32 pixels_per_unit) {
    return FiniteFloat(static_cast<f64>(pixels) / scale / pixels_per_unit);
}

} // namespace

PixelViewport ComputePixelViewport(const PixelPerfectCamera& camera, u32 window_width, u32 window_height) {
    PixelViewport v;
    const u32 rw = std::max(camera.reference_width, 1u);
    const u32 rh = std::max(camera.reference_height, 1u);
    const f32 ppu = std::isfinite(camera.pixels_per_unit) && camera.pixels_per_unit > 0.0f
                        ? camera.pixels_per_unit
                        : 1.0f;
    v.scale = std::max(1u, std::min(window_width / rw, window_height / rh));
    if (camera.fill_window) {
        v.width = window_width;
        v.height = window_height;
        // Shows however many whole art pixels fit, rounding up at the edge.
        v.ortho_width = WorldSize(window_width, v.scale, ppu);
        v.ortho_height = WorldSize(window_height, v.scale, ppu);
        return v;
    }
    v.width = rw * v.scale;
    v.height = rh * v.scale;
    v.x = static_cast<i32>((static_cast<i64>(window_width) - v.width) / 2);
    v.y = static_cast<i32>((static_cast<i64>(window_height) - v.height) / 2);
    v.ortho_width = WorldSize(rw, 1, ppu);
    v.ortho_height = WorldSize(rh, 1, ppu);
    return v;
}

Vec3 SnapToPixelGrid(const Vec3& position, f32 pixels_per_unit) {
    if (!std::isfinite(pixels_per_unit) || pixels_per_unit <= 0.0f || !std::isfinite(position.x) ||
        !std::isfinite(position.y)) return position;
    const f64 ppu = pixels_per_unit;
    const f32 x = FiniteFloat(std::round(static_cast<f64>(position.x) * ppu) / ppu);
    const f32 y = FiniteFloat(std::round(static_cast<f64>(position.y) * ppu) / ppu);
    return Vec3(x, y, position.z);
}

} // namespace aether::sprite2d
