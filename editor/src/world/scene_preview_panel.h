#pragma once

#include "core/scene_preview.h"
#include "ui/image_texture.h"
#include "world/scene_panel.h"

#include <filesystem>
#include <memory>
#include <string>

namespace aether::editor {

// The Render Preview tool: a picture of the open scene from the real renderer (see ScenePreview). "Render" saves the scene into the
// project first, since the player renders what is on disk, then shows the result in the panel when it is ready. Fills the current ImGui window.
class ScenePreviewPanel {
public:
    explicit ScenePreviewPanel(ScenePanel& scene) : scene_(scene) {}
    // The project to render (its Content folder, cooked into Saved/Preview). Empty: none open.
    void SetProject(const std::filesystem::path& project_file);
    void Draw();

    // Saves the scene and starts a render; false (with Note()) if it could not.
    bool Render();
    // Moves a finished render into the texture. Draw() does this every frame; tests call it directly.
    void Poll();
    ScenePreview* Preview() { return preview_.get(); }
    const ImageTexture& Texture() const { return texture_; }
    const std::string& Note() const { return note_; }

    u32 width = 960;
    u32 height = 540;

private:
    ScenePanel& scene_;
    std::unique_ptr<ScenePreview> preview_;
    ImageTexture texture_;
    std::string note_;
};

} // namespace aether::editor
