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

void ApplyMarquee(SceneDocument& document, const std::vector<Entity>& hits, bool add, bool subtract) {
    if (!add && !subtract) {
        document.SetSelection(hits);
        return;
    }
    std::vector<Entity> selection = document.Selection();
    for (const Entity entity : hits) {
        const auto it = std::find(selection.begin(), selection.end(), entity);
        if (add && it == selection.end()) selection.push_back(entity);
        if (subtract && it != selection.end()) selection.erase(it);
    }
    document.SetSelection(std::move(selection));
}

} // namespace

void ViewportTools::DrawToolbar() {
    for (const GizmoMode mode : {GizmoMode::Move, GizmoMode::Rotate, GizmoMode::Scale}) {
        const char* label = mode == GizmoMode::Move ? "Move" : (mode == GizmoMode::Rotate ? "Rotate" : "Scale");
        if (ImGui::RadioButton(label, mode_ == mode)) mode_ = mode;
        ImGui::SetItemTooltip("%s (%c)", label, mode == GizmoMode::Move ? 'W' : (mode == GizmoMode::Rotate ? 'E' : 'R'));
        ImGui::SameLine();
    }
    const bool local = space_ == GizmoSpace::Local;
    if (ImGui::Button(local ? "Local" : "World")) space_ = local ? GizmoSpace::World : GizmoSpace::Local;
    ImGui::SetItemTooltip("Move and Rotate handles follow the %s axes (Q)", local ? "object's own" : "world's");
    ImGui::SameLine();
    const bool centre = pivot_ == GizmoPivot::Centre;
    if (ImGui::Button(centre ? "Centre" : "Pivot")) pivot_ = centre ? GizmoPivot::Origin : GizmoPivot::Centre;
    ImGui::SetItemTooltip("Handles sit at the %s (P)", centre ? "middle of the selection" : "first selected entity's origin");
    ImGui::SameLine();
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
    if (hovered && !io.WantTextInput && !gizmo_.Dragging() && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) { // W, E and R choose the gizmo, as in most editors; while looking, W/A/S/D fly
        if (ImGui::IsKeyPressed(ImGuiKey_W)) mode_ = GizmoMode::Move;
        if (ImGui::IsKeyPressed(ImGuiKey_E)) mode_ = GizmoMode::Rotate;
        if (ImGui::IsKeyPressed(ImGuiKey_R)) mode_ = GizmoMode::Scale;
        if (ImGui::IsKeyPressed(ImGuiKey_Q)) space_ = space_ == GizmoSpace::World ? GizmoSpace::Local : GizmoSpace::World;
        if (!io.KeyCtrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_P)) // not Ctrl+Shift+P, the command palette
            pivot_ = pivot_ == GizmoPivot::Origin ? GizmoPivot::Centre : GizmoPivot::Origin;
        if (io.KeyCtrl) { // not while looking, where A is the fly key
            if (ImGui::IsKeyPressed(ImGuiKey_A)) SelectAll(document);
            if (ImGui::IsKeyPressed(ImGuiKey_I)) InvertSelection(document);
        }
    }
    const f32 snap_step = SnapStep();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        press_ = io.MousePos;
        dragged_ = false;
        gizmo_press_ = false;
        marquee_ = false;
        marquee_op_ = io.KeyAlt ? MarqueeOperation::Subtract : (io.KeyCtrl ? MarqueeOperation::Add : MarqueeOperation::Replace);
        if (!io.KeyShift && !io.KeyAlt && gizmo_.Begin(document, camera, origin, size, io.MousePos, mode_, space_, pivot_)) { // Alt selects through, so it never grabs a handle
            dragged_ = true;
            gizmo_press_ = true;
        }
    }
    if (gizmo_.Dragging()) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) gizmo_.Update(document, camera, origin, size, io.MousePos, snap_step);
        else gizmo_.End(document, camera, origin, size, io.MousePos, snap_step);
    }
    if (active && ImGui::IsMouseDown(ImGuiMouseButton_Left) && (std::fabs(io.MousePos.x - press_.x) + std::fabs(io.MousePos.y - press_.y)) > 4.0f) {
        dragged_ = true;
        if (!gizmo_press_ && !io.KeyShift) marquee_ = true; // shift + drag pans the view instead
    }
    if ((hovered || active) && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const bool was_marquee = marquee_, was_gizmo = gizmo_press_;
        marquee_ = false;
        gizmo_press_ = false;
        if (was_gizmo) {
            // the gizmo's End above recorded the drag as one edit
        } else if (was_marquee) {
            ApplyMarquee(document, EntitiesInRectangle(document, camera, origin, size, press_, io.MousePos, bounds_), marquee_op_ == MarqueeOperation::Add,
                         marquee_op_ == MarqueeOperation::Subtract);
        } else if (hovered && !dragged_ && !io.KeyShift) {
            Entity picked = PickEntity(document, camera, origin, size, io.MousePos, 14.0f, bounds_);
            if (io.KeyAlt) { // step through what is stacked under the mouse, so a model in front does not hide the one behind
                const std::vector<Entity> stack = PickEntities(document, camera, origin, size, io.MousePos, 14.0f, bounds_);
                if (!stack.empty()) {
                    std::size_t index = 0;
                    bool found = false;
                    for (std::size_t i = 0; i < stack.size(); ++i) {
                        if (!document.IsSelected(stack[i])) continue;
                        index = i;
                        found = true;
                    }
                    index = !found ? (stack.size() > 1 ? 1 : 0) : (index + 1) % stack.size(); // the first Alt-click takes the one behind the front
                    picked = stack[index];
                }
            }
            if (picked == kNullEntity) {
                if (!io.KeyCtrl) document.ClearSelection();
            } else if (io.KeyCtrl) {
                document.ToggleSelection(picked);
            } else {
                document.SetSelection({picked});
            }
        }
        dragged_ = false;
    }
}

void ViewportTools::SelectAll(SceneDocument& document) {
    std::vector<Entity> all;
    for (const Entity entity : document.Entities()) {
        if (document.GetWorld().HasComponent<Transform>(entity)) all.push_back(entity);
    }
    document.SetSelection(std::move(all));
}

void ViewportTools::InvertSelection(SceneDocument& document) {
    std::vector<Entity> inverted;
    for (const Entity entity : document.Entities()) {
        if (document.GetWorld().HasComponent<Transform>(entity) && !document.IsSelected(entity)) inverted.push_back(entity);
    }
    document.SetSelection(std::move(inverted));
}

void ViewportTools::DrawMarquee(ImDrawList* draw, const ImVec2& origin, const ImVec2& size) const {
    if (!marquee_ || size.x < 1.0f || size.y < 1.0f) return;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const ImVec2 low(std::clamp(std::min(press_.x, mouse.x), origin.x, origin.x + size.x), std::clamp(std::min(press_.y, mouse.y), origin.y, origin.y + size.y));
    const ImVec2 high(std::clamp(std::max(press_.x, mouse.x), origin.x, origin.x + size.x), std::clamp(std::max(press_.y, mouse.y), origin.y, origin.y + size.y));
    const ImU32 color = marquee_op_ == MarqueeOperation::Add     ? Color(90, 200, 120, 220)
                        : marquee_op_ == MarqueeOperation::Subtract ? Color(220, 100, 90, 220)
                                                                    : Color(120, 170, 255, 220);
    draw->AddRectFilled(low, high, (color & 0x00FFFFFFu) | (45u << 24));
    draw->AddRect(low, high, color);
}

void ViewportTools::DrawGizmo(ImDrawList* draw, const SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, bool hovered) const {
    const Entity subject = TransformGizmo::Subject(document);
    if (subject == kNullEntity || size.x < 1.0f || size.y < 1.0f) return;
    const Mat4 vp = camera.ViewProjection(size.x / size.y);
    const ImGuiIO& io = ImGui::GetIO();
    const GizmoMode mode = gizmo_.Dragging() ? gizmo_.Mode() : mode_;
    const GizmoSpace space = gizmo_.Dragging() ? gizmo_.Space() : space_;
    const GizmoAxis hot = gizmo_.Dragging() ? gizmo_.Axis() : (hovered ? gizmo_.HitTest(document, camera, origin, size, io.MousePos, mode, space, pivot_) : GizmoAxis::None);
    const Vec3 at = TransformGizmo::Pivot(document, pivot_);
    const f32 length = TransformGizmo::HandleLength(camera, at, size);
    struct Handle { GizmoAxis axis; ImU32 normal, bright; };
    const Handle handles[3] = {{GizmoAxis::X, Color(210, 70, 70), Color(255, 130, 120)},
                               {GizmoAxis::Y, Color(80, 190, 80), Color(140, 255, 140)},
                               {GizmoAxis::Z, Color(80, 120, 230), Color(140, 175, 255)}};
    ImVec2 from;
    if (!ProjectToViewport(vp, at, origin, size, from)) return;
    if (mode != GizmoMode::Rotate) { // the plane squares, each in the colour of the axis it is flat against: slide (Move) or grow two axes (Scale)
        const struct { GizmoAxis plane; int r, g, b; } planes[3] = {{GizmoAxis::PlaneYZ, 210, 70, 70}, {GizmoAxis::PlaneXZ, 80, 190, 80}, {GizmoAxis::PlaneXY, 80, 120, 230}};
        for (const auto& pl : planes) {
            Vec3 corners[4];
            TransformGizmo::PlaneCorners(document, subject, pl.plane, mode, space, at, length, corners);
            ImVec2 p[4];
            bool visible = true;
            for (int i = 0; i < 4 && visible; ++i) visible = ProjectToViewport(vp, corners[i], origin, size, p[i]);
            if (!visible) continue;
            const bool lit = pl.plane == hot;
            draw->AddQuadFilled(p[0], p[1], p[2], p[3], Color(pl.r, pl.g, pl.b, lit ? 190 : 90));
            draw->AddQuad(p[0], p[1], p[2], p[3], Color(pl.r, pl.g, pl.b, 255), lit ? 2.5f : 1.5f);
        }
    }
    for (const Handle& h : handles) {
        const ImU32 color = h.axis == hot ? h.bright : h.normal;
        const f32 thickness = h.axis == hot ? 4.0f : 3.0f;
        if (mode == GizmoMode::Rotate) {
            constexpr int kSegments = 48;
            Vec3 ring[kSegments];
            const Vec3 axis_direction = TransformGizmo::HandleDirection(document, subject, h.axis, mode, space);
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
            if (!ProjectToViewport(vp, at + TransformGizmo::HandleDirection(document, subject, h.axis, mode, space) * length, origin, size, to)) continue;
            draw->AddLine(from, to, color, thickness);
            const f32 r = h.axis == hot ? 7.0f : 5.5f;
            if (mode == GizmoMode::Scale) draw->AddRectFilled(ImVec2(to.x - r, to.y - r), ImVec2(to.x + r, to.y + r), color);
            else draw->AddCircleFilled(to, r, color);
        }
    }
    if (mode == GizmoMode::Scale) { // the middle box: every axis at once
        const bool lit = hot == GizmoAxis::Uniform;
        const ImU32 fill = lit ? Color(255, 235, 140) : Color(205, 207, 215);
        const f32 r = lit ? 7.0f : 5.5f;
        draw->AddRectFilled(ImVec2(from.x - r, from.y - r), ImVec2(from.x + r, from.y + r), fill);
        draw->AddRect(ImVec2(from.x - r, from.y - r), ImVec2(from.x + r, from.y + r), Color(80, 82, 90), 0.0f, 0, 1.5f);
    }
    draw->AddCircleFilled(from, 3.5f, Color(235, 238, 245));
}

void ViewportTools::DrawEntityBox(ImDrawList* draw, const SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size,
                                  Entity entity, ImU32 color, f32 thickness, const ModelBoundsCache* bounds, const Vec3& shift) {
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
        ok[i] = ProjectToViewport(vp, Vec3(w.x + shift.x, w.y + shift.y, w.z + shift.z), origin, size, p[i]);
    }
    for (const auto& e : edges) {
        if (ok[e[0]] && ok[e[1]]) draw->AddLine(p[e[0]], p[e[1]], color, thickness);
    }
}

void ViewportTools::DrawSelectionOutlines(ImDrawList* draw, const SceneDocument& document, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, bool hovered,
                                          const LivePosition& live_position) const {
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
        Vec3 shift(0.0f, 0.0f, 0.0f);
        Vec3 live;
        if (live_position && live_position(entity, live)) shift = live - OriginOf(document, entity);
        DrawEntityBox(draw, document, camera, origin, size, entity, Color(255, 170, 40), 2.0f, bounds_, shift);
        ImVec2 at;
        if (ProjectToViewport(vp, OriginOf(document, entity) + shift, origin, size, at)) draw->AddText(ImVec2(at.x + 8.0f, at.y - 18.0f), Color(255, 170, 40), document.Name(entity).c_str());
    }
}

} // namespace aether::editor
