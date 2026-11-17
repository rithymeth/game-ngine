#pragma once

#include "world/content_drop.h"
#include "world/scene_view.h"
#include "world/scene_view_shading.h"
#include "world/viewport_tools.h"

#include <string>
#include <vector>

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
    // The bottom-right chip's text as last drawn: "24 entities · 88 draw calls · GPU 2.4 ms · 120 FPS" when the shaded picture reports its
    // draws and GPU time, else just the entities and the frame rate. A host or a test reads it without the pixels.
    const std::string& LastStatsText() const { return stats_text_; }

    // Play in the editor: while the document is playing or paused the view draws the running game behind a 2 px border (green
    // pie.border; amber while paused with a tint over the picture) and a state banner, and leaves the editor's own overlays off.
    // In a Play session the camera is the game's; Stop ejects back to the camera the play started from. In a Simulate session
    // the game keeps its own camera and this view's camera stays under the editor's control, so Stop leaves it where it is. A
    // host and a test read this without the pixels.
    struct PieOverlay {
        bool active = false;
        bool paused = false;
        bool simulating = false; // started by Simulate: the view's camera is not the game's
        f32 border_thickness = 0.0f;
        ImU32 border_color = 0;
        ImVec2 banner_min{}, banner_max{};
        std::string banner_text;
        // The running game's runtime errors, as the shaded picture reports them. Up to three are listed under the banner;
        // a row's rectangle is where the error is drawn, and a click on one selects the entity it came from (when it has one).
        struct Error {
            std::string text;
            std::string entity_guid;
            ImVec2 min{}, max{};
        };
        std::vector<Error> errors;
        ImVec2 errors_min{}, errors_max{};
    };
    const PieOverlay& LastPie() const { return pie_; }

    // The concept's row 3 look controls, drawn after the gizmo icons: "Perspective", "Lit" and "Show" dropdowns. A
    // projection preset (Top, Front, Side) also switches to orthographic. The buttons' rectangles and, while a popup is
    // open, its items' labels and rectangles are reported, so a host aligns its chrome and a test clicks an item.
    struct LookControls {
        ImVec2 projection_min{}, projection_max{};
        ImVec2 mode_min{}, mode_max{};
        ImVec2 show_min{}, show_max{};
        std::vector<std::string> items;        // the open popup's labels, top to bottom
        std::vector<ImVec2> item_min, item_max; // and their rectangles
    };
    void DrawLookToolbar();
    const LookControls& LastLookControls() const { return look_; }

private:
    Chrome chrome_;
    LookControls look_;
    PieOverlay pie_;
    std::string stats_text_; // the stats chip's last text
    SceneViewCamera camera_;
    SceneViewCamera::State camera_before_play_{};
    bool play_active_ = false; // the document was playing or paused at the previous draw
    bool play_possessed_ = false; // this session started as Play: the view's camera goes back on Stop (Simulate leaves it)
    ViewportTools tools_;
    ContentDrop drop_;          // an asset dragged in from the Content Browser: where it would land, and the last thing to say about a drop
    std::string notice_;        // a short message in the middle of the view (a saved or recalled bookmark), for `notice_left_` seconds
    f32 notice_left_ = 0.0f;
    f32 speed_notice_ = 0.0f;   // seconds left to show the fly speed after the wheel changed it
};

} // namespace aether::editor
