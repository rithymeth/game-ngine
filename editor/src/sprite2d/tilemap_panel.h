#pragma once

#include "sprite2d/tilemap_document.h"

namespace aether::editor {

// The Tilemap editor (Phase 26 step 5, §26.6): the tileset's tiles to pick
// from (solid ones marked), the tools, the layers, and the map as a grid to
// paint on (drag to paint, right-drag to erase, mouse wheel to zoom, middle
// drag to pan). Tiles show as numbered colour swatches: the panel draws
// without a texture, so it works headless and in every host.
class TilemapPanel {
public:
    explicit TilemapPanel(TilemapEditDocument& document) : doc_(document) {}
    void Draw();

    f32 zoom = 24.0f; // screen pixels per tile
    bool show_grid = true;
    bool show_solid = true;

private:
    void DrawPalette();
    void DrawTools();
    void DrawLayers();
    void DrawCanvas();

    TilemapEditDocument& doc_;
    f32 pan_x_ = 8.0f, pan_y_ = 8.0f;
    bool dragging_ = false;
    bool rect_active_ = false;
    i32 rect_x_ = 0, rect_y_ = 0;
    i32 selected_autotile_ = 0;
};

} // namespace aether::editor
