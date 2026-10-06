#pragma once

#include "aether/sprite2d/tilemap.h"

#include <filesystem>
#include <string>
#include <vector>

// A tilemap and its tileset being edited (Phase 26 step 5, §26.6): paint,
// erase, rectangle and flood fill with a brush that is a tile or an
// autotile, layers, solid flags, autotile rules, and undo per stroke.

namespace aether::editor {

enum class TileTool : u8 { Paint, Erase, Rectangle, Fill, Pick };

class TilemapEditDocument {
public:
    TilemapEditDocument(sprite2d::TilemapData map, sprite2d::Tileset tileset);

    const sprite2d::TilemapData& Map() const { return map_; }
    const sprite2d::Tileset& GetTileset() const { return tileset_; }
    u64 Revision() const { return revision_; }

    // What the tools paint: a tile number, or sprite2d::AutotileCell(i).
    i32 brush = 0;
    usize active_layer = 0;
    TileTool tool = TileTool::Paint;

    // A stroke is one undo step: press, any number of Apply calls, release.
    void BeginStroke();
    // Paint, Erase and Fill act at the cell; Pick takes the cell's value as
    // the brush. (Rectangle uses Rectangle().)
    void Apply(i32 x, i32 y);
    void EndStroke();
    // Fills the rectangle (inclusive corners) with the brush, or clears it
    // with Erase; one undo step.
    void Rectangle(i32 x0, i32 y0, i32 x1, i32 y1);
    // The cells a flood fill from (x, y) would change (4-connected, same value).
    std::vector<std::pair<i32, i32>> FloodRegion(i32 x, i32 y) const;

    bool Undo();
    bool Redo();
    bool CanUndo() const { return !undo_.empty(); }
    bool CanRedo() const { return !redo_.empty(); }

    // Layers and size (undoable). The active layer stays valid.
    usize AddLayer(const std::string& name);
    bool RemoveLayer(usize layer);
    bool RenameLayer(usize layer, const std::string& name);
    bool SetLayerCollides(usize layer, bool collides);
    bool Resize(u32 width, u32 height);

    // The tileset (undoable).
    void SetSolid(i32 tile, bool solid);
    usize AddAutotile(const std::string& name);
    // Which tile an autotile shows for a neighbour mask.
    bool SetAutotileTile(usize autotile, u8 mask, i32 tile);

    bool Save(const std::filesystem::path& tilemap_file, const std::filesystem::path& tileset_file,
              std::string* error = nullptr) const;
    bool dirty() const { return dirty_; }

private:
    struct Snapshot {
        sprite2d::TilemapData map;
        sprite2d::Tileset tileset;
    };
    void Record();
    void Changed();
    void ClampActive();
    bool PaintCell(i32 x, i32 y, i32 value);

    sprite2d::TilemapData map_;
    sprite2d::Tileset tileset_;
    bool in_stroke_ = false;
    Snapshot stroke_start_;
    std::vector<Snapshot> undo_, redo_;
    u64 revision_ = 1;
    bool dirty_ = false;
};

} // namespace aether::editor
