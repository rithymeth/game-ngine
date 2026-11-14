#pragma once

#include "host/live_viewport.h"
#include "world/scene_panel.h"
#include "world/scene_view_shading.h"

#include <filesystem>
#include <functional>
#include <string>

namespace aether::editor {

// The Scene View's rendered picture: the open scene run by the player's renderer in this process (LiveViewport), driven by the Scene View's camera.
// The project is cooked once, the first time a shaded look is asked for; after that the document's edits are patched into the running scene as in the
// Live Viewport, and only a change that cannot be patched (or another scene) loads it again. Needs a Vulkan device, so the shell makes it; the portable
// panel only sees the SceneViewShading interface.
class LiveSceneShading final : public SceneViewShading {
public:
    LiveSceneShading(rhi::IDevice& device, ScenePanel& scene, std::function<std::filesystem::path()> project_file);
    ~LiveSceneShading() override;

    bool Ready() const override { return running_ && texture_.Valid(); }
    const std::string& Note() const override { return note_; }
    bool Advance(SceneDocument& document, const SceneViewCamera& camera, u32 width, u32 height, SceneViewMode mode, f32 dt) override;
    const ImageTexture& Picture() const override { return texture_; }
    void Stop() override;

    // The running scene, for a host or a test that wants the pixels or the counters.
    LiveViewport& Viewport() { return viewport_; }
    const LiveViewport& Viewport() const { return viewport_; }

    // Frames drawn per second at most, and the least time between two patches of the running scene.
    f32 max_fps = 30.0f;
    f32 reload_interval = 0.1f;
    // How long the view must keep a new size before the picture is rebuilt at it (a dock splitter drag changes it every frame).
    f32 resize_delay = 0.3f;

private:
    // Cooks the project's models and starts the open scene (unsaved edits included) at width x height.
    bool Start(const SceneDocument& document, u32 width, u32 height);
    // Starts the scene again from the cooked folder (a size change: the swap chain is made with the picture).
    bool LoadAgain(const SceneDocument& document, u32 width, u32 height);

    ScenePanel& scene_;
    std::function<std::filesystem::path()> project_file_;
    LiveViewport viewport_;
    ImageTexture texture_;
    std::string note_;
    std::filesystem::path cooked_;
    std::string scene_path_;
    u64 last_revision_ = 0;  // the document's revision last patched into (or loaded from) the running scene
    u64 last_structure_ = 0; // and its structure revision: while that holds, a change was only to values
    u32 width_ = 0;          // the size the picture is drawn at
    u32 height_ = 0;
    u32 wanted_width_ = 0; // the size the view has been asking for, for `resize_delay`
    u32 wanted_height_ = 0;
    f32 size_still_ = 0.0f;
    f32 since_frame_ = 0.0f;
    f32 since_reload_ = 0.0f;
    bool running_ = false;
};

} // namespace aether::editor
