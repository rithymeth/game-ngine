#include "world/content_drop.h"

#include "content/content_drag.h"
#include "core/snapping.h"

namespace aether::editor {

Vec3 DropPoint(const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, f32 snap_step) {
    Vec3 ground;
    if (size.x < 1.0f || size.y < 1.0f || !GroundPoint(camera.ViewProjection(size.x / size.y), origin, size, mouse, ground)) ground = camera.target;
    if (snap_step > 0.0f) ground = SnapTranslation(ground, snap_step);
    return ground;
}

std::string PlaceContentAsset(SceneDocument& document, const std::filesystem::path& content_root, const std::string& content_path, const Vec3& at, bool* placed) {
    if (placed) *placed = false;
    if (IsPlaceablePrefab(content_path)) {
        if (content_root.empty()) return "Could not place " + content_path;
        std::string problem;
        const Entity entity = PlacePrefabAsset(document, content_root, content_path, at, &problem);
        if (entity == kNullEntity) return problem.empty() ? "Could not place " + content_path : problem;
        if (placed) *placed = true;
        return "Placed " + content_path;
    }
    if (IsPlaceableModel(content_path)) {
        const Entity entity = PlaceModelAsset(document, content_path, at);
        if (entity == kNullEntity) return "Could not place " + content_path;
        if (placed) *placed = true;
        return "Placed " + content_path;
    }
    return "Only models (.gltf, .glb) and prefabs (.aprefab) can be placed in the scene.";
}

void ContentDrop::Accept(SceneDocument& document, const std::filesystem::path& content_root, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, f32 snap_step) {
    preview = false;
    if (!ImGui::BeginDragDropTarget()) return;
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kContentPathPayload, ImGuiDragDropFlags_AcceptBeforeDelivery)) {
        const std::string path(static_cast<const char*>(payload->Data));
        const Vec3 at = DropPoint(camera, origin, size, ImGui::GetIO().MousePos, snap_step);
        if (payload->IsDelivery()) {
            note = PlaceContentAsset(document, content_root, path, at);
        } else if (IsPlaceableModel(path) || IsPlaceablePrefab(path)) {
            preview = true; // still being dragged: a marker where it would land
            point = at;
        }
    }
    ImGui::EndDragDropTarget();
}

void ContentDrop::DrawPreview(ImDrawList* draw, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size) const {
    if (!preview || size.x < 1.0f || size.y < 1.0f) return;
    ImVec2 at;
    if (!ProjectToViewport(camera.ViewProjection(size.x / size.y), point, origin, size, at)) return;
    draw->AddCircle(at, 10.0f, IM_COL32(255, 200, 60, 255), 24, 2.0f);
    draw->AddCircleFilled(at, 3.0f, IM_COL32(255, 200, 60, 255));
}

} // namespace aether::editor
