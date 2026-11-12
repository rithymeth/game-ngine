#pragma once

#include "world/scene_view.h"
#include "world/viewport_tools.h"

#include <string>

namespace aether::editor {

// A view of the open scene as boxes and axes over a ground grid, from an orbit camera, with the editing tools (click-to-select, the move, rotate and
// scale gizmos, dropping a model from the Content Browser). It draws with ImGui's own draw list (no renderer), so it works in the portable shell and
// headless; it shows where things are, not how they look (the Live Viewport shows that, with the same tools).
class SceneViewPanel {
public:
    // Fills the current ImGui window, editing the selection of `document`.
    void Draw(SceneDocument& document);
    // Moves the camera's target to the selection (or the whole scene if nothing is selected) and backs off to fit.
    void FocusSelection(const SceneDocument& document);
    SceneViewCamera& Camera() { return camera_; }
    const SceneViewCamera& Camera() const { return camera_; }
    ViewportTools& Tools() { return tools_; }

private:
    SceneViewCamera camera_;
    ViewportTools tools_;
    std::string note_;          // the last thing the panel has to say (a drop it could not place)
    bool drop_preview_ = false; // a model is being dragged over the view; where it would land
    Vec3 drop_point_{0.0f, 0.0f, 0.0f};
};

} // namespace aether::editor
