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

// The mouse as a ray into the world (origin and unit direction); false if the view-projection can't be inverted.
bool MouseRay(const Mat4& view_projection, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, Vec3& ray_origin, Vec3& ray_direction);

// The distance along the line through `axis_origin` in unit direction `axis_direction` at which it passes closest to a ray;
// false when the two are (nearly) parallel, where there is no such point.
bool ClosestOnAxis(const Vec3& axis_origin, const Vec3& axis_direction, const Vec3& ray_origin, const Vec3& ray_direction, f32& distance);

enum class GizmoAxis { None, X, Y, Z };

// The move gizmo: three axis handles at the first selected entity, dragged along world X, Y or Z. A drag is one undoable
// edit however many frames it takes (the document merges the uncommitted updates), moves the entity in world space even under
// a parent, and snaps the dragged axis to `snap_step` (0 = off). Only the first selected entity moves; a multi-selection move
// needs the document to track more than one pending edit. No ImGui: the panel feeds it the mouse.
class TranslateGizmo {
public:
    // The handle length in world units, so it stays about the same size on screen at any zoom.
    static f32 HandleLength(const SceneViewCamera& camera) { return camera.distance * 0.18f; }
    // The entity the gizmo is on (the first selected one with a Transform), or kNullEntity.
    static Entity Subject(const SceneDocument& document);

    GizmoAxis HitTest(const SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse) const;
    // Starts a drag if `mouse` is on a handle; true if it did.
    bool Begin(const SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse);
    // Moves the subject to follow the mouse, as an uncommitted edit; false if there is no drag (or the mouse ray is parallel to the axis).
    bool Update(SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, f32 snap_step);
    // Ends the drag, committing it as one edit.
    void End(SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, f32 snap_step);
    bool Dragging() const { return axis_ != GizmoAxis::None; }
    GizmoAxis Axis() const { return axis_; }

private:
    bool Apply(SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, f32 snap_step, bool committed);

    GizmoAxis axis_ = GizmoAxis::None;
    Entity entity_ = kNullEntity;
    Vec3 start_world_{0.0f, 0.0f, 0.0f}; // the subject's world position when the drag began
    f32 grab_ = 0.0f;                    // where along the axis the mouse took hold, so the entity does not jump to the cursor
};

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
    TranslateGizmo gizmo_;
    ImVec2 press_{0.0f, 0.0f};
    bool dragged_ = false;
    bool snap_ = false;
    f32 snap_step_ = 0.5f;
};

} // namespace aether::editor
