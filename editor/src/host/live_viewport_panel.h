#pragma once

#include "host/live_viewport.h"
#include "ui/image_texture.h"
#include "world/scene_panel.h"
#include "world/scene_view.h"

#include <filesystem>
#include <functional>
#include <string>

namespace aether::editor {

// The Live Viewport tool: the open project's scene running under the real renderer (see LiveViewport), with the orbit camera of the Scene View
// (right-drag looks, middle-drag or shift+drag pans, the wheel zooms). **Start** saves the scene, cooks the project and loads it; **Reload**
// does the same again to pick up edits; **Stop** frees it. Draws at up to 30 frames a second while the tool is on screen. Fills the current window.
class LiveViewportPanel {
public:
    // `project_file` is asked each time (the open project can change).
    LiveViewportPanel(rhi::IDevice& device, ScenePanel& scene, std::function<std::filesystem::path()> project_file)
        : viewport_(device), scene_(scene), project_file_(std::move(project_file)) {}

    // Saves, cooks and loads; false (with Note()) if it could not.
    bool Start();
    void Stop();
    // Runs one frame if due: `dt` is the time since the last call. Draw() does this; tests call it directly. True if a new picture was shown.
    bool Advance(f32 dt);
    void Draw(f32 dt);

    LiveViewport& Viewport() { return viewport_; }
    SceneViewCamera& Camera() { return camera_; }
    const ImageTexture& Texture() const { return texture_; }
    const std::string& Note() const { return note_; }
    f32 FramesPerSecond() const { return fps_; }

    u32 width = 960;
    u32 height = 540;
    f32 max_fps = 30.0f;

private:
    LiveViewport viewport_;
    ScenePanel& scene_;
    std::function<std::filesystem::path()> project_file_;
    SceneViewCamera camera_;
    ImageTexture texture_;
    std::string note_;
    f32 since_frame_ = 0.0f;
    f32 fps_ = 0.0f;
};

} // namespace aether::editor
