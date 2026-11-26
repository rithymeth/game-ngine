#pragma once

#include "aether/sprite2d/atlas.h"
#include "ui/image_texture.h"

namespace aether::assets { class AssetDatabase; }

namespace aether::editor {

class SceneDocument;

// Preview atlas metadata without editing the asset; apply a manual frame to the selected static Sprite.
class SpriteAtlasPanel {
public:
    bool Open(const std::filesystem::path& file, const assets::AssetDatabase* database, std::string* error);
    void Clear();
    void Draw(SceneDocument* scene = nullptr);
    bool ApplyFrame(SceneDocument& scene, const std::string& name) const;

private:
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
