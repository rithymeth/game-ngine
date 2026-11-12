#pragma once

#include "aether/ecs/entity.h"
#include "aether/math/mat4.h"
#include "core/scene_document.h"

#include <imgui.h>

namespace aether::editor {

// An orbit camera around a target point, Y up: right-drag turns it, middle-drag (or shift + left-drag) pans, the wheel
// zooms. Plain data and plain functions, so the maths is tested without a window.
struct SceneViewCamera {
    f32 yaw = 0.7f;      // radians around Y
    f32 pitch = 0.5f;    // radians above the horizon, kept clear of straight up and down
    f32 distance = 14.0f;
    Vec3 target{0.0f, 0.0f, 0.0f};
    f32 fov_degrees = 60.0f;

    Vec3 Eye() const;
    Mat4 View() const;
    Mat4 ViewProjection(f32 aspect) const;
    void Orbit(f32 delta_yaw, f32 delta_pitch);
    void Zoom(f32 wheel_steps);          // each step scales the distance by 0.9 (in) or 1/0.9 (out)
    void Pan(f32 right_units, f32 up_units); // in the view plane, in world units
};

// Where a world point lands in a viewport rectangle (top-left `origin`, `size` pixels); false when it is behind the camera.
bool ProjectToViewport(const Mat4& view_projection, const Vec3& world, const ImVec2& origin, const ImVec2& size, ImVec2& out, f32* depth = nullptr);

// The entity whose origin is nearest `mouse` within `radius` pixels (the nearer to the camera on a tie within 2 pixels);
// kNullEntity if none. Only entities with a Transform can be picked.
Entity PickEntity(const SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, f32 radius = 14.0f);

// A view of the open scene as boxes and axes over a ground grid, from an orbit camera, with click-to-select. It draws
// with ImGui's own draw list (no renderer), so it works in the portable shell and headless; it shows where things are, not how they look.
class SceneViewPanel {
public:
    // Fills the current ImGui window, editing the selection of `document`.
    void Draw(SceneDocument& document);
    // Moves the camera's target to the selection (or the whole scene if nothing is selected) and backs off to fit.
    void FocusSelection(const SceneDocument& document);
    SceneViewCamera& Camera() { return camera_; }
    const SceneViewCamera& Camera() const { return camera_; }

private:
    SceneViewCamera camera_;
    ImVec2 press_{0.0f, 0.0f};
    bool dragged_ = false;
};

} // namespace aether::editor
