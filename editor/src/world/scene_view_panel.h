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
    // `with_toolbar` false skips the row of controls above the picture: a host that embeds the view in a narrower pane (the Scene tool's centre
    // column) relies on the picture's own chips and the W/E/R keys instead.
    void Draw(SceneDocument& document, const ModelBoundsCache* bounds = nullptr, const std::filesystem::path& content_root = {}, bool with_toolbar = true);
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
    // What the picture's own controls were last frame, in screen coordinates, as the design puts them: the viewport's
    // name and look along the top left, the gizmo's space, snapping and the grid along the top right, the selection at
    // the bottom left and the scene's counts at the bottom right. A host drawing its own overlays keeps clear of them,
    // and a test clicks them.
    struct Chrome {
        ImVec2 viewport_label_min{}, viewport_label_max{};
        ImVec2 projection_min{}, projection_max{};
        ImVec2 view_mode_min{}, view_mode_max{};
        ImVec2 gizmo_min{}, gizmo_max{};
        ImVec2 show_min{}, show_max{};
        ImVec2 space_min{}, space_max{};
        ImVec2 snap_min{}, snap_max{};
        ImVec2 grid_min{}, grid_max{};
        ImVec2 selection_min{}, selection_max{};
        ImVec2 stats_min{}, stats_max{};
    };
    const Chrome& LastChrome() const { return chrome_; }

private:
    Chrome chrome_;
    SceneViewCamera camera_;
    ViewportTools tools_;
    ContentDrop drop_;          // an asset dragged in from the Content Browser: where it would land, and the last thing to say about a drop
    std::string notice_;        // a short message in the middle of the view (a saved or recalled bookmark), for `notice_left_` seconds
    f32 notice_left_ = 0.0f;
    f32 speed_notice_ = 0.0f;   // seconds left to show the fly speed after the wheel changed it
};

} // namespace aether::editor
