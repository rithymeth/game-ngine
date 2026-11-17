#pragma once

#include "host/live_viewport.h"
#include "ui/image_texture.h"
#include "world/game_view_panel.h"
#include "world/scene_panel.h"

#include <filesystem>
#include <functional>
#include <string>

namespace aether::editor {

// The Game View's picture: the open scene run by the player's Game in this process (LiveViewport) and drawn from its own camera every
// frame -- the Camera entity the game uses, at the pose, field of view and projection the game has left it in. It is the one view that
// never borrows the editor's camera, so what it shows is what a player would see. Needs a Vulkan device, so the shell makes it; the
// portable panel only sees GameViewSurface.
class LiveGameView final : public GameViewSurface {
public:
    LiveGameView(rhi::IDevice& device, ScenePanel& scene, std::function<std::filesystem::path()> project_file);
    ~LiveGameView() override;

    bool Start(const SceneDocument& document, u32 width, u32 height) override;
    bool Frame(f32 play_dt) override;
    const ImageTexture& Picture() const override { return texture_; }
    const std::string& Note() const override { return note_; }
    void Stop() override;

    // The running scene, for a host or a test that wants the pixels or the counters.
    LiveViewport& Viewport() { return viewport_; }
    const LiveViewport& Viewport() const { return viewport_; }

private:
    ScenePanel& scene_;
    std::function<std::filesystem::path()> project_file_;
    LiveViewport viewport_;
    ImageTexture texture_;
    std::string note_;
};

} // namespace aether::editor
