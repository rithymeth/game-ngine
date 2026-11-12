#pragma once

#include "world/scene_view.h"

#include <imgui.h>

#include <filesystem>
#include <string>

namespace aether::editor {

// Where a dragged asset would land in a 3D view: the ground (y = 0) under the mouse, rounded to `snap_step` when that is above zero, or the camera's target
// when the mouse looks along or away from the ground. The view is the rectangle at `origin` of `size`.
Vec3 DropPoint(const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, const ImVec2& mouse, f32 snap_step);

// Puts a model (.gltf, .glb) or a prefab (.aprefab) from the content folder into the scene at `at` (PlaceModelAsset, PlacePrefabAsset: one undo step, selected),
// and says what happened in a line a view can show ("Placed ...", or why not). `placed` is set to whether anything was added. A prefab needs `content_root`.
std::string PlaceContentAsset(SceneDocument& document, const std::filesystem::path& content_root, const std::string& content_path, const Vec3& at, bool* placed = nullptr);

// The drop target both 3D views share (the Scene View and the Live Viewport): an asset dragged in from the Content Browser. While it is over the view a
// marker shows where it would land, and letting go places it.
struct ContentDrop {
    std::string note;      // the last thing there is to say (what was placed, or why not); empty before any drop
    bool preview = false;  // an asset is being dragged over the view
    Vec3 point{0.0f, 0.0f, 0.0f};

    // For the ImGui item just submitted over the view (an invisible button covering `origin`, `size`): accepts what the Content Browser drags onto it.
    void Accept(SceneDocument& document, const std::filesystem::path& content_root, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size, f32 snap_step);
    // The landing marker, when an asset is being dragged over the view.
    void DrawPreview(ImDrawList* draw, const SceneViewCamera& camera, const ImVec2& origin, const ImVec2& size) const;
};

} // namespace aether::editor
