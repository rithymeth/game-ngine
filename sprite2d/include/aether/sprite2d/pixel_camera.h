#pragma once

#include "aether/core/base.h"
#include "aether/math/math.h"

// The pixel-perfect camera (Phase 26 step 5, §26.6): a reference resolution
// shown at the largest whole-number scale the window allows, centred with
// bars around it, so every art pixel covers the same number of screen
// pixels. Positions snap to the art's pixel grid so nothing shimmers.

namespace aether::sprite2d {

struct PixelPerfectCamera {
    u32 reference_width = 320;
    u32 reference_height = 180;
    f32 pixels_per_unit = 16.0f; // art pixels per world unit
    // Fills the window with the scene past the reference size (a partial pixel
    // row at the edge) instead of bars; the scale is still a whole number.
    bool fill_window = false;
};

struct PixelViewport {
    i32 x = 0, y = 0;    // the viewport's top-left in the window, pixels
    u32 width = 0, height = 0;
    u32 scale = 1;       // screen pixels per art pixel
    f32 ortho_width = 0, ortho_height = 0; // what it shows, in world units
};

PixelViewport ComputePixelViewport(const PixelPerfectCamera& camera, u32 window_width, u32 window_height);

// Rounds x and y to the art pixel grid (z is kept).
Vec3 SnapToPixelGrid(const Vec3& position, f32 pixels_per_unit);

} // namespace aether::sprite2d
