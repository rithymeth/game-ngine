#include "world/scene_view_panel.h"

#include "aether/scene/components.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/hierarchy.h"
#include "content/content_drag.h"
#include "core/snapping.h"

#include <algorithm>
#include <cmath>

namespace aether::editor {

namespace {

ImU32 Color(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }

Vec3 WorldOrigin(const SceneDocument& document, Entity entity) {
    const Mat4 world = ComputeWorldTransform(document.GetWorld(), document.Guids(), entity);
    return Vec3(world.cols[3].x, world.cols[3].y, world.cols[3].z);
}

} // namespace

void SceneViewPanel::FocusSelection(const SceneDocument& document) {
    std::vector<Entity> subjects;
    for (const Entity entity : document.Selection()) {
        if (document.GetWorld().IsAlive(entity) && document.GetWorld().HasComponent<Transform>(entity)) subjects.push_back(entity);
    }
    if (subjects.empty()) {
        for (const Entity entity : document.Entities()) {
            if (document.GetWorld().HasComponent<Transform>(entity)) subjects.push_back(entity);
        }
    }
    if (subjects.empty()) return;
    Vec3 low(1e30f, 1e30f, 1e30f), high(-1e30f, -1e30f, -1e30f);
    for (const Entity entity : subjects) {
        const Vec3 p = WorldOrigin(document, entity);
        low = Vec3(std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z));
        high = Vec3(std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z));
    }
    camera_.target = (low + high) * 0.5f;
    const Vec3 extent = high - low;
    camera_.distance = std::clamp(std::max({extent.x, extent.y, extent.z}) * 1.6f + 5.0f, 3.0f, 1000.0f);
}

void SceneViewPanel::Draw(SceneDocument& document, const ModelBoundsCache* bounds, const std::filesystem::path& content_root) {
    tools_.SetBounds(bounds);
    tools_.DrawToolbar();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", note_.empty() ? "drop a model or a prefab from the Content Browser to place it" : note_.c_str());
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 size(std::max(avail.x, 64.0f), std::max(avail.y, 64.0f));
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##scene_view", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    drop_preview_ = false;
    // An asset dragged in from the Content Browser: a model is placed on the ground under the mouse (rounded to the move step if Snap is on).
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kContentPathPayload, ImGuiDragDropFlags_AcceptBeforeDelivery)) {
            const std::string path(static_cast<const char*>(payload->Data));
            Vec3 ground;
            const bool on_ground = GroundPoint(camera_.ViewProjection(size.x / size.y), origin, size, ImGui::GetIO().MousePos, ground);
            if (!on_ground) ground = camera_.target;
            if (tools_.SnapEnabled()) ground = SnapTranslation(ground, tools_.MoveSnapStep());
            const bool is_prefab = IsPlaceablePrefab(path);
            if (!IsPlaceableModel(path) && !is_prefab) {
                if (payload->IsDelivery()) note_ = "Only models (.gltf, .glb) and prefabs (.aprefab) can be placed in the scene.";
            } else if (payload->IsDelivery()) {
                if (is_prefab) {
                    std::string problem;
                    const Entity placed = content_root.empty() ? kNullEntity : PlacePrefabAsset(document, content_root, path, ground, &problem);
                    note_ = placed != kNullEntity ? "Placed " + path : (problem.empty() ? "Could not place " + path : problem);
                } else {
                    const Entity placed = PlaceModelAsset(document, path, ground);
                    note_ = placed == kNullEntity ? "Could not place " + path : "Placed " + path;
                }
            } else {
                drop_preview_ = true; // still being dragged: a marker where it would land, drawn with the rest below
                drop_point_ = ground;
            }
        }
        ImGui::EndDragDropTarget();
    }
    const ImGuiIO& io = ImGui::GetIO();

    // Input: right-drag orbits, middle-drag or shift + left-drag pans, the wheel zooms, F focuses; the tools take the gizmo and the clicks.
    HandleCameraInput(camera_, hovered, active, size);
    if (hovered && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F)) FocusSelection(document);
    tools_.HandleInput(document, camera_, origin, size, hovered, active);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(origin.x + size.x, origin.y + size.y);
    draw->PushClipRect(origin, end, true);
    draw->AddRectFilled(origin, end, Color(22, 24, 29));
    const Mat4 vp = camera_.ViewProjection(size.x / size.y);
    auto line = [&](const Vec3& a, const Vec3& b, ImU32 color, f32 thickness = 1.0f) {
        ImVec2 pa, pb;
        if (ProjectToViewport(vp, a, origin, size, pa) && ProjectToViewport(vp, b, origin, size, pb)) draw->AddLine(pa, pb, color, thickness);
    };

    // The ground: a grid on y = 0, every fifth line stronger, with the world axes through the origin.
    constexpr int kHalf = 20;
    for (int i = -kHalf; i <= kHalf; ++i) {
        const ImU32 c = i % 5 == 0 ? Color(70, 76, 90) : Color(42, 46, 56);
        line(Vec3(static_cast<f32>(i), 0, static_cast<f32>(-kHalf)), Vec3(static_cast<f32>(i), 0, static_cast<f32>(kHalf)), c);
        line(Vec3(static_cast<f32>(-kHalf), 0, static_cast<f32>(i)), Vec3(static_cast<f32>(kHalf), 0, static_cast<f32>(i)), c);
    }
    line(Vec3(-kHalf, 0, 0), Vec3(kHalf, 0, 0), Color(150, 60, 60, 200), 1.5f);
    line(Vec3(0, 0, -kHalf), Vec3(0, 0, kHalf), Color(60, 90, 160, 200), 1.5f);
    line(Vec3(0, 0, 0), Vec3(0, 3, 0), Color(70, 150, 80, 220), 1.5f);

    // Each entity with a Transform: its box (the Transform's scale is the box's size), orange when selected, cyan for a camera.
    const World& world = document.GetWorld();
    Entity hovered_entity = kNullEntity;
    if (hovered) hovered_entity = PickEntity(document, camera_, origin, size, io.MousePos, 14.0f, bounds);
    for (const Entity entity : document.Entities()) {
        if (!world.HasComponent<Transform>(entity)) continue;
        const Mat4 m = ComputeWorldTransform(world, document.Guids(), entity);
        const bool selected = document.IsSelected(entity);
        const bool is_camera = world.HasComponent<::aether::Camera>(entity);
        const ImU32 color = selected ? Color(255, 170, 40) : (entity == hovered_entity ? Color(255, 255, 255) : (is_camera ? Color(80, 200, 220) : Color(150, 158, 175)));
        ViewportTools::DrawEntityBox(draw, document, camera_, origin, size, entity, color, selected ? 2.0f : 1.0f, bounds);
        const Vec3 origin_w(m.cols[3].x, m.cols[3].y, m.cols[3].z);
        if (selected) { // the axes of what is selected, in its own orientation
            const Vec3 x(m.cols[0].x, m.cols[0].y, m.cols[0].z), y(m.cols[1].x, m.cols[1].y, m.cols[1].z), z(m.cols[2].x, m.cols[2].y, m.cols[2].z);
            const f32 lx = std::max(x.Length(), 1e-4f), ly = std::max(y.Length(), 1e-4f), lz = std::max(z.Length(), 1e-4f);
            line(origin_w, origin_w + x * (1.2f / lx), Color(230, 70, 70), 2.0f);
            line(origin_w, origin_w + y * (1.2f / ly), Color(90, 210, 90), 2.0f);
            line(origin_w, origin_w + z * (1.2f / lz), Color(80, 120, 240), 2.0f);
        }
        if (selected || entity == hovered_entity) {
            ImVec2 at;
            if (ProjectToViewport(vp, origin_w, origin, size, at)) draw->AddText(ImVec2(at.x + 8.0f, at.y - 18.0f), color, document.Name(entity).c_str());
        }
    }
    if (drop_preview_) {
        ImVec2 at;
        if (ProjectToViewport(vp, drop_point_, origin, size, at)) {
            draw->AddCircle(at, 10.0f, Color(255, 200, 60), 24, 2.0f);
            draw->AddCircleFilled(at, 3.0f, Color(255, 200, 60));
        }
    }
    tools_.DrawGizmo(draw, document, camera_, origin, size, hovered);
    draw->AddText(ImVec2(origin.x + 8.0f, end.y - 22.0f), Color(128, 135, 148),
                  "right-drag: look   middle-drag / shift+drag: pan   wheel: zoom   click: select   F: focus   W/E/R: move/rotate/scale");
    draw->PopClipRect();
}

} // namespace aether::editor
