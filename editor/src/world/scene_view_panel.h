#pragma once

#include "world/content_drop.h"
#include "world/scene_view.h"
#include "world/scene_view_shading.h"
#include "world/viewport_tools.h"

#include <string>

namespace aether::editor {

// A view of the open scene: boxes and axes over a ground grid, or (with `shading`) the scene drawn by the real renderer in the shell. From an orbit
// camera, with the editing tools (click-to-select, the move, rotate and scale gizmos, dropping a model from the Content Browser). Without `shading` it
// draws with ImGui's own draw list only (no renderer), so it works in the portable shell and headless; the shading is a Vulkan build's business.
class SceneViewPanel {
public:
    // Fills the current ImGui window, editing the selection of `document`. With `bounds`, objects are picked and outlined by their models' real extents.
    // `content_root` is the project's content folder, where a prefab dropped from the Content Browser is read from (empty: prefabs cannot be placed).
    void Draw(SceneDocument& document, const ModelBoundsCache* bounds = nullptr, const std::filesystem::path& content_root = {});
    // Moves the camera's target to the middle of the selection's boxes (or the whole scene if nothing is selected) and backs off to fit. `bounds` frames
    // an entity by its model's real extent; without it every entity is a unit box.
    void FocusSelection(const SceneDocument& document, const ModelBoundsCache* bounds = nullptr);
    SceneViewCamera& Camera() { return camera_; }
    const SceneViewCamera& Camera() const { return camera_; }
    ViewportTools& Tools() { return tools_; }

    // Wireframe (boxes, above) or a rendered picture; Lit, Unlit and Normals need `shading`. Not owned: null leaves the view wireframe-only.
    SceneViewMode mode = SceneViewMode::Wireframe;
    SceneViewShading* shading = nullptr;
    // The overlays drawn on top, in either mode.
    SceneViewShowFlags show;

private:
    SceneViewCamera camera_;
    ViewportTools tools_;
    ContentDrop drop_;          // an asset dragged in from the Content Browser: where it would land, and the last thing to say about a drop
    std::string notice_;        // a short message in the middle of the view (a saved or recalled bookmark), for `notice_left_` seconds
    f32 notice_left_ = 0.0f;
    f32 speed_notice_ = 0.0f;   // seconds left to show the fly speed after the wheel changed it
};

} // namespace aether::editor
