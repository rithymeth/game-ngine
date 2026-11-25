#pragma once

#include "aether/sprite2d/atlas.h"
#include "ui/image_texture.h"

namespace aether::assets { class AssetDatabase; }

namespace aether::editor {

// Read-only view of the project's authored frame rectangles and animation clips.
class SpriteAtlasPanel {
public:
    bool Open(const std::filesystem::path& file, const assets::AssetDatabase* database, std::string* error);
    void Clear();
    void Draw();

private:
    sprite2d::SpriteAtlas atlas_;
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
