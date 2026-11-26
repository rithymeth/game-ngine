#pragma once

#include "aether/sprite2d/atlas.h"
#include "ui/image_texture.h"

namespace aether::assets { class AssetDatabase; }

namespace aether::editor {

class SceneDocument;

// Preview atlas metadata and assign a frame or clip to a matching selected scene Sprite.
class SpriteAtlasPanel {
public:
    bool Open(const std::filesystem::path& file, const assets::AssetDatabase* database, std::string* error);
    void Clear();
    // Re-focus a scene Sprite's saved frame or clip after opening its atlas.
    void FocusPreview(std::string_view frame, std::string_view clip);
    void Draw(SceneDocument* scene = nullptr);
    bool ApplyFrame(SceneDocument& scene, const std::string& name) const;
    bool ApplyClip(SceneDocument& scene, const std::string& name) const;

private:
    const char* SelectionProblem(const SceneDocument& scene) const;
    const char* ClipAssignmentProblem(const SceneDocument& scene, const sprite2d::SpriteClip* clip) const;
    const char* FrameAssignmentProblem(const SceneDocument& scene, const std::string& name) const;
    sprite2d::SpriteAtlas atlas_;
    assets::AssetGuid atlas_guid_;
    assets::ImageData pending_image_;
    ImageTexture image_;
    std::string asset_name_;
    std::string texture_status_;
    bool show_texture_ = false;
    bool playing_ = false;
    int frame_ = 0;
    int clip_ = -1;
    f32 elapsed_ = 0.0f;
};

} // namespace aether::editor
