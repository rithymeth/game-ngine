#include "world/viewport_tools.h"

#include "aether/scene/hierarchy.h"

#include <algorithm>
#include <cmath>

namespace aether::editor {

namespace {

ImU32 Color(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

Vec3 OriginOf(const SceneDocument& document, Entity entity) {
    const Mat4 world = ComputeWorldTransform(document.GetWorld(), document.Guids(), entity);
    return Vec3(world.cols[3].x, world.cols[3].y, world.cols[3].z);
}

} // namespace

void ViewportTools::DrawToolbar() {
    for (const GizmoMode mode : {GizmoMode::Move, GizmoMode::Rotate, GizmoMode::Scale}) {
        const char* label = mode == GizmoMode::Move ? "Move" : (mode == GizmoMode::Rotate ? "Rotate" : "Scale");
        if (ImGui::RadioButton(label, mode_ == mode)) mode_ = mode;
        ImGui::SetItemTooltip("%s (%c)", label, mode == GizmoMode::Move ? 'W' : (mode == GizmoMode::Rotate ? 'E' : 'R'));
        ImGui::SameLine();
    }
    ImGui::Checkbox("Snap", &snap_);
    ImGui::SetItemTooltip("Round a gizmo drag to the step");
    ImGui::SameLine();
    ImGui::BeginDisabled(!snap_);
    ImGui::SetNextItemWidth(90.0f);
    if (mode_ == GizmoMode::Move) ImGui::DragFloat("##snap_move", &snap_move_, 0.05f, 0.05f, 100.0f, "%.2f m");
    else if (mode_ == GizmoMode::Rotate) ImGui::DragFloat("##snap_rotate", &snap_rotate_, 0.5f, 1.0f, 90.0f, "%.0f deg");
    else ImGui::DragFloat("##snap_scale", &snap_scale_, 0.01f, 0.01f, 10.0f, "%.2f");
    ImGui::EndDisabled();
}

void ViewportTools::HandleInput(SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, bool hovered, bool active) {
    const ImGuiIO& io = ImGui::GetIO();
    if (hovered && !io.WantTextInput && !gizmo_.Dragging()) { // W, E and R choose the gizmo, as in most editors
        if (ImGui::IsKeyPressed(ImGuiKey_W)) mode_ = GizmoMode::Move;
        if (ImGui::IsKeyPressed(ImGuiKey_E)) mode_ = GizmoMode::Rotate;
        if (ImGui::IsKeyPressed(ImGuiKey_R)) mode_ = GizmoMode::Scale;
    }
    const f32 snap_step = SnapStep();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        press_ = io.MousePos;
        dragged_ = false;
        if (!io.KeyShift && gizmo_.Begin(document, camera, origin, size, io.MousePos, mode_)) dragged_ = true; // a gizmo press is not a pick
    }
    if (gizmo_.Dragging()) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) gizmo_.Update(document, camera, origin, size, io.MousePos, snap_step);
        else gizmo_.End(document, camera, origin, size, io.MousePos, snap_step);
    }
    if (active && ImGui::IsMouseDown(ImGuiMouseButton_Left) && (std::fabs(io.MousePos.x - press_.x) + std::fabs(io.MousePos.y - press_.y)) > 4.0f) dragged_ = true;
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !dragged_ && !io.KeyShift) {
        const Entity picked = PickEntity(document, camera, origin, size, io.MousePos, 14.0f, bounds_);
        if (picked == kNullEntity) {
            if (!io.KeyCtrl) document.ClearSelection();
        } else if (io.KeyCtrl) {
            document.ToggleSelection(picked);
        } else {
            document.SetSelection({picked});
        }
    }
}

void ViewportTools::DrawGizmo(ImDrawList* draw, const SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, bool hovered) const {
    const Entity subject = TransformGizmo::Subject(document);
    if (subject == kNullEntity || size.x < 1.0f || size.y < 1.0f) return;
    const Mat4 vp = camera.ViewProjection(size.x / size.y);
    const ImGuiIO& io = ImGui::GetIO();
    const GizmoMode mode = gizmo_.Dragging() ? gizmo_.Mode() : mode_;
    const GizmoAxis hot = gizmo_.Dragging() ? gizmo_.Axis() : (hovered ? gizmo_.HitTest(document, camera, origin, size, io.MousePos, mode) : GizmoAxis::None);
    const Vec3 at = OriginOf(document, subject);
    const f32 length = TransformGizmo::HandleLength(camera);
    struct Handle { GizmoAxis axis; ImU32 normal, bright; };
    const Handle handles[3] = {{GizmoAxis::X, Color(210, 70, 70), Color(255, 130, 120)},
                               {GizmoAxis::Y, Color(80, 190, 80), Color(140, 255, 140)},
                               {GizmoAxis::Z, Color(80, 120, 230), Color(140, 175, 255)}};
    ImVec2 from;
    if (!ProjectToViewport(vp, at, origin, size, from)) return;
    for (const Handle& h : handles) {
        const ImU32 color = h.axis == hot ? h.bright : h.normal;
        const f32 thickness = h.axis == hot ? 4.0f : 3.0f;
        if (mode == GizmoMode::Rotate) {
            constexpr int kSegments = 48;
            Vec3 ring[kSegments];
            const Vec3 axis_direction = TransformGizmo::HandleDirection(document, subject, h.axis, mode);
            TransformGizmo::RingPoints(at, axis_direction, length, kSegments, ring);
            ImVec2 previous;
            bool have_previous = ProjectToViewport(vp, ring[kSegments - 1], origin, size, previous);
            for (int i = 0; i < kSegments; ++i) {
                ImVec2 current;
                const bool ok = ProjectToViewport(vp, ring[i], origin, size, current);
                if (ok && have_previous) draw->AddLine(previous, current, color, thickness);
                previous = current;
                have_previous = ok;
            }
        } else {
            ImVec2 to;
            if (!ProjectToViewport(vp, at + TransformGizmo::HandleDirection(document, subject, h.axis, mode) * length, origin, size, to)) continue;
            draw->AddLine(from, to, color, thickness);
            const f32 r = h.axis == hot ? 7.0f : 5.5f;
            if (mode == GizmoMode::Scale) draw->AddRectFilled(ImVec2(to.x - r, to.y - r), ImVec2(to.x + r, to.y + r), color);
            else draw->AddCircleFilled(to, r, color);
        }
    }
    draw->AddCircleFilled(from, 3.5f, Color(235, 238, 245));
}

void ViewportTools::DrawEntityBox(ImDrawList* draw, const SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size,
                                  Entity entity, ImU32 color, f32 thickness, const ModelBoundsCache* bounds) {
    static const int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    if (size.x < 1.0f || size.y < 1.0f || !document.GetWorld().IsAlive(entity)) return;
    const Mat4 vp = camera.ViewProjection(size.x / size.y);
    const Mat4 m = ComputeWorldTransform(document.GetWorld(), document.Guids(), entity);
    const LocalBounds box = EntityBounds(document, entity, bounds);
    const Vec3 corners[8] = {{box.min.x, box.min.y, box.min.z}, {box.max.x, box.min.y, box.min.z}, {box.max.x, box.max.y, box.min.z}, {box.min.x, box.max.y, box.min.z},
                             {box.min.x, box.min.y, box.max.z}, {box.max.x, box.min.y, box.max.z}, {box.max.x, box.max.y, box.max.z}, {box.min.x, box.max.y, box.max.z}};
    ImVec2 p[8];
    bool ok[8];
    for (int i = 0; i < 8; ++i) {
        const Vec4 w = m * Vec4(corners[i].x, corners[i].y, corners[i].z, 1.0f);
        ok[i] = ProjectToViewport(vp, Vec3(w.x, w.y, w.z), origin, size, p[i]);
    }
    for (const auto& e : edges) {
        if (ok[e[0]] && ok[e[1]]) draw->AddLine(p[e[0]], p[e[1]], color, thickness);
    }
}

void ViewportTools::DrawSelectionOutlines(ImDrawList* draw, const SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, bool hovered) const {
    if (size.x < 1.0f || size.y < 1.0f) return;
    const Mat4 vp = camera.ViewProjection(size.x / size.y);
    const World& world = document.GetWorld();
    auto label = [&](Entity entity, ImU32 color) {
        ImVec2 at;
        if (ProjectToViewport(vp, OriginOf(document, entity), origin, size, at)) draw->AddText(ImVec2(at.x + 8.0f, at.y - 18.0f), color, document.Name(entity).c_str());
    };
    Entity under_mouse = kNullEntity;
    if (hovered && !gizmo_.Dragging()) under_mouse = PickEntity(document, camera, origin, size, ImGui::GetIO().MousePos, 14.0f, bounds_);
    if (under_mouse != kNullEntity && !document.IsSelected(under_mouse)) {
        DrawEntityBox(draw, document, camera, origin, size, under_mouse, Color(255, 255, 255), 1.0f, bounds_);
        label(under_mouse, Color(255, 255, 255));
    }
    for (const Entity entity : document.Selection()) {
        if (!world.IsAlive(entity) || !world.HasComponent<Transform>(entity)) continue;
        DrawEntityBox(draw, document, camera, origin, size, entity, Color(255, 170, 40), 2.0f, bounds_);
        label(entity, Color(255, 170, 40));
    }
}

} // namespace aether::editor
