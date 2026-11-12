#pragma once

#include "aether/ecs/entity.h"
#include "aether/math/mat4.h"
#include "aether/math/quaternion.h"
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
enum class GizmoMode { Move, Rotate, Scale };

// The transform gizmo on the first selected entity, in three modes. Move: arrows along world X, Y and Z. Rotate: a ring around
// each world axis. Scale: handles along the entity's own (local) axes. A drag is one undoable edit however many frames it takes
// (the document merges the uncommitted updates), works in world space even under a parent, and snaps to `snap_step`, which means
// metres for Move, degrees for Rotate and a scale step for Scale (0 = off). Only the first selected entity changes; a multi-selection
// edit needs the document to track more than one pending edit. No ImGui: the panel feeds it the mouse.
class TransformGizmo {
public:
    // The handle length in world units, so it stays about the same size on screen at any zoom.
    static f32 HandleLength(const SceneViewCamera& camera) { return camera.distance * 0.18f; }
    // The entity the gizmo is on (the first selected one with a Transform), or kNullEntity.
    static Entity Subject(const SceneDocument& document);
    // The world-space direction of a handle: the world axis (Move, Rotate) or the entity's local axis (Scale).
    static Vec3 HandleDirection(const SceneDocument& document, Entity subject, GizmoAxis axis, GizmoMode mode);
    // Points of a rotate ring (a circle around `axis_direction`, radius `radius`, centred on `centre`), for drawing and hit-testing.
    static void RingPoints(const Vec3& centre, const Vec3& axis_direction, f32 radius, int count, Vec3* out);

    GizmoAxis HitTest(const SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, GizmoMode mode) const;
    // Starts a drag if `mouse` is on a handle; true if it did.
    bool Begin(const SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, GizmoMode mode);
    // Edits the subject to follow the mouse, as an uncommitted edit; false if there is no drag (or the mouse ray is parallel to what it needs).
    bool Update(SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, f32 snap_step);
    // Ends the drag, committing it as one edit.
    void End(SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, f32 snap_step);
    bool Dragging() const { return axis_ != GizmoAxis::None; }
    GizmoAxis Axis() const { return axis_; }
    GizmoMode Mode() const { return mode_; }

private:
    bool Apply(SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, f32 snap_step, bool committed);
    // Move and Scale: where along the handle's line the mouse ray passes closest. Rotate: the angle of the mouse around the axis.
    bool Measure(const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, f32& value) const;

    GizmoAxis axis_ = GizmoAxis::None;
    GizmoMode mode_ = GizmoMode::Move;
    Entity entity_ = kNullEntity;
    Vec3 start_world_{0.0f, 0.0f, 0.0f};   // the subject's world position when the drag began
    Vec3 direction_{1.0f, 0.0f, 0.0f};     // the handle's world direction (the axis a ring turns about)
    Vec3 start_scale_{1.0f, 1.0f, 1.0f};
    Quaternion start_rotation_;            // the subject's local rotation when the drag began
    f32 grab_ = 0.0f;                      // Move/Scale: where along the handle the mouse took hold; Rotate: the angle it took hold at
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
    TransformGizmo gizmo_;
    GizmoMode mode_ = GizmoMode::Move;
    ImVec2 press_{0.0f, 0.0f};
    bool dragged_ = false;
    bool snap_ = false;
    f32 snap_move_ = 0.5f;
    f32 snap_rotate_ = 15.0f;
    f32 snap_scale_ = 0.1f;
};

} // namespace aether::editor
