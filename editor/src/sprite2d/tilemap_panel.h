#pragma once

#include "sprite2d/tilemap_document.h"

#include "aether/assets/image.h"
#include "ui/image_texture.h"

#include <optional>

namespace aether::assets { class AssetDatabase; }

namespace aether::editor {

// The Tilemap editor (Phase 26 step 5, §26.6): the tileset's tiles to pick
// from (solid ones marked), the tools, the layers, and the map as a grid to
// paint on (drag to paint, right-drag to erase, mouse wheel to zoom, middle
// drag to pan). Art comes from the authored tileset texture when readable,
// with numbered swatches as the fallback.
class TilemapPanel {
public:
    explicit TilemapPanel(TilemapEditDocument& document) : doc_(document) {}
    void Draw();
    bool FindNextInvalidPaint();
    bool FocusCell(usize layer, i32 x, i32 y);
    void LoadTexturePreview(const assets::AssetDatabase& database);
    bool LoadJumpProofOverlay(const std::filesystem::path& path, std::string* error = nullptr);

    f32 zoom = 24.0f; // screen pixels per tile
    bool show_grid = true;
    bool show_solid = true;
    bool solo_layer_art = false; // edit view only; diagnostics still cover all layers

private:
    void DrawPalette();
    bool DrawTileArt(ImDrawList* draw, ImVec2 a, ImVec2 b, i32 tile) const;
    void DrawTools();
    void DrawResizeDialog();
    void DrawLayers();
    void DrawCanvas();
    void DrawJumpProofTools();
    bool JumpProofCellMatchesMap() const;

    TilemapEditDocument& doc_;
    assets::ImageData pending_sheet_;
    ImageTexture sheet_;
    std::string sheet_status_;
    bool show_sheet_ = false;
    int resize_width_ = 0, resize_height_ = 0;
    u64 save_error_revision_ = 0;
    std::string save_error_;
    struct InvalidPaintCell { usize layer, index; i32 tile; };
    std::optional<InvalidPaintCell> invalid_paint_cell_;
    std::optional<u64> invalid_search_revision_;
    struct PendingFocus { i32 x, y; u64 revision; };
    std::optional<PendingFocus> pending_focus_;
    f32 pan_x_ = 8.0f, pan_y_ = 8.0f;
    bool dragging_ = false;
    struct BrushSample {
        i32 x, y;
        TileTool tool;
    };
    std::optional<BrushSample> last_brush_sample_;
    bool rect_active_ = false;
    i32 rect_x_ = 0, rect_y_ = 0;
    i32 selected_autotile_ = 0;
    struct GhostPoint { f32 x, y; };
    std::vector<GhostPoint> ghost_before_, ghost_after_;
    usize ghost_difference_ = 0;
    struct GhostVerdicts { bool before_passed, after_passed; };
    std::optional<GhostVerdicts> ghost_verdicts_;
    std::filesystem::path ghost_map_;
    struct GhostCell {
        std::filesystem::path asset;
        i32 layer, x, y, edited_value;
    };
    std::optional<GhostCell> ghost_cell_;
    std::string ghost_file_, ghost_error_;
    f32 ghost_pixels_per_unit_ = 16.0f;
};

} // namespace aether::editor
