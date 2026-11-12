#pragma once

#include "host/live_viewport.h"
#include "ui/image_texture.h"
#include "world/scene_panel.h"
#include "world/scene_view.h"
#include "world/content_drop.h"
#include "world/viewport_tools.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace aether::editor {

// The Live Viewport tool: the open scene running under the real renderer (see LiveViewport), with the orbit camera of the Scene View (right-drag
// looks, middle-drag or shift+drag pans, the wheel zooms). **Start** cooks the models the scene uses and runs the scene *as it is open*, unsaved
// edits included; after that every change to the scene (an edit, a gizmo drag, an undo) reloads it a moment later, so what you do shows without saving.
// **Reload** cooks again, which picks up a model added since. **Run game** lets physics and scripts run; off (the default) freezes the world so the
// view is the scene as authored. The editing tools of the Scene View work on the rendered picture: click an object to select it, drag a gizmo handle to
// move, rotate or scale it (W, E, R), and the change shows in the picture as it is made. Draws at up to 30 frames a second while the tool is on
// screen. Fills the current window.
class LiveViewportPanel {
public:
    // `project_file` is asked each time (the open project can change).
    LiveViewportPanel(rhi::IDevice& device, ScenePanel& scene, std::function<std::filesystem::path()> project_file)
        : viewport_(device), scene_(scene), project_file_(std::move(project_file)) {}

    // Cooks the models the open scene names and loads the scene as it is open (nothing is saved); false (with Note()) if it could not.
    bool Start();
    void Stop();
    // Reloads the scene if it changed since the last look (at most every `reload_interval`), then runs one frame if due: `dt` is the time since the
    // last call. Draw() does this; tests call it directly. True if a new picture was shown.
    bool Advance(f32 dt);
    void Draw(f32 dt);

    LiveViewport& Viewport() { return viewport_; }
    SceneViewCamera& Camera() { return camera_; }
    ViewportTools& Tools() { return tools_; }
    const ImageTexture& Texture() const { return texture_; }
    const std::string& Note() const { return note_; }
    f32 FramesPerSecond() const { return fps_; }
    // Models the open scene names that the last cook did not include: they are not drawn until Reload.
    usize UncookedModels() const;

    u32 width = 960;
    u32 height = 540;
    f32 max_fps = 30.0f;
    f32 reload_interval = 0.1f; // least time between two reloads of the scene while it is being edited
    bool run_game = false;      // let the game's physics and scripts run (otherwise the world is frozen)

private:
    LiveViewport viewport_;
    ScenePanel& scene_;
    std::function<std::filesystem::path()> project_file_;
    SceneViewCamera camera_;
    ViewportTools tools_;
    ContentDrop drop_; // a model or prefab dragged in from the Content Browser: where it would land, and what happened to the last one
    ImageTexture texture_;
    std::string note_;
    f32 since_frame_ = 0.0f;
    f32 since_reload_ = 0.0f;
    f32 fps_ = 0.0f;
    u64 last_revision_ = 0;               // the scene's revision when it was last handed to the viewport
    u64 last_structure_ = 0;              // and its structure revision: while that holds, a change was only to Transforms
    std::vector<std::string> cooked_models_; // the models the last cook was asked for
};

} // namespace aether::editor
