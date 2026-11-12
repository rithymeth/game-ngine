#pragma once

#include "world/scene_view.h"

#include <imgui.h>

#include <functional>

namespace aether::editor {

// The editing tools of a 3D view, shared by the Scene View (drawn over a wireframe) and the Live Viewport (drawn over the rendered picture):
// the Move / Rotate / Scale choice and snap steps, the gizmo drag, click-to-select, and drawing the gizmo and the selection's outline. It holds no
// camera and no picture; the view passes in its camera and the rectangle the picture covers, so the same maths serves both.
class ViewportTools {
public:
    GizmoMode Mode() const { return mode_; }
    GizmoSpace Space() const { return space_; }
    void SetSpace(GizmoSpace space) { space_ = space; }
    bool SnapEnabled() const { return snap_; }
    // The step of the current mode, or 0 while snapping is off (metres for Move, degrees for Rotate, a scale step for Scale).
    f32 SnapStep() const { return !snap_ ? 0.0f : (mode_ == GizmoMode::Move ? snap_move_ : (mode_ == GizmoMode::Rotate ? snap_rotate_ : snap_scale_)); }
    // The Move step (what a dropped model snaps to), or 0 while snapping is off.
    f32 MoveSnapStep() const { return snap_ ? snap_move_ : 0.0f; }
    bool Dragging() const { return gizmo_.Dragging(); }
    void SetMode(GizmoMode mode) { mode_ = mode; }
    void SetSnap(bool enabled) { snap_ = enabled; }
    // The models' real extents, for picking and outlines (null: every object is a unit box and is picked by its origin). Not owned.
    void SetBounds(const ModelBoundsCache* bounds) { bounds_ = bounds; }
    const ModelBoundsCache* Bounds() const { return bounds_; }

    // The mode buttons and the snap controls on the current line (the caller may SameLine and add to it).
    void DrawToolbar();
    // For the ImGui item just submitted over the view (an invisible button covering `origin`, `size`): W, E and R choose the mode, a press on a
    // gizmo handle starts a drag that follows the mouse and records one undo step on release, and a click elsewhere selects (ctrl toggles, empty space clears).
    void HandleInput(SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, bool hovered, bool active);
    // The gizmo on the first selected entity, in the chosen mode, the handle under the mouse (or being dragged) brighter.
    void DrawGizmo(ImDrawList* draw, const SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, bool hovered) const;
    // An entity's outline: its box (the model's real extent with `bounds`, else a unit box) placed by its world transform. `thickness` > 1 for emphasis.
    static void DrawEntityBox(ImDrawList* draw, const SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size,
                              Entity entity, ImU32 color, f32 thickness, const ModelBoundsCache* bounds = nullptr, const Vec3& shift = Vec3(0.0f, 0.0f, 0.0f));
    // The outline and the name of every selected entity (orange), and of the one under the mouse (white), for a view with no wireframe of its own.
    // `live_position`, if given, says where an entity really is in a running game (false: no such entity); an outline is drawn there, so a body the physics
    // is moving is outlined where it is drawn and not where the document left it.
    using LivePosition = std::function<bool(Entity, Vec3&)>;
    void DrawSelectionOutlines(ImDrawList* draw, const SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, bool hovered,
                               const LivePosition& live_position = nullptr) const;

private:
    TransformGizmo gizmo_;
    const ModelBoundsCache* bounds_ = nullptr;
    GizmoMode mode_ = GizmoMode::Move;
    GizmoSpace space_ = GizmoSpace::World;
    bool snap_ = false;
    f32 snap_move_ = 0.5f;
    f32 snap_rotate_ = 15.0f;
    f32 snap_scale_ = 0.1f;
    ImVec2 press_{0.0f, 0.0f};
    bool dragged_ = false;
};

} // namespace aether::editor
