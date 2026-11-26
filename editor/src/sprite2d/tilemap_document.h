#pragma once

#include "aether/sprite2d/tilemap.h"

#include <filesystem>
#include <optional>
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
    // Higher indices draw in front of lower indices. Preserve the selected layer when it moves.
    bool MoveLayer(usize from, usize to);
    // Insert an independent copy just above the source and select it.
    bool DuplicateLayer(usize layer);
    bool RenameLayer(usize layer, const std::string& name);
    bool SetLayerCollides(usize layer, bool collides);
    bool CanResize(u32 width, u32 height) const;
    bool Resize(u32 width, u32 height);

    // The tileset (undoable).
    void SetSolid(i32 tile, bool solid);
    usize AddAutotile(const std::string& name);
    // Which tile an autotile shows for a neighbour mask.
    bool SetAutotileTile(usize autotile, u8 mask, i32 tile);
    bool FillEmptyAutotileMasks(usize autotile, i32 tile);

    bool Save(const std::filesystem::path& tilemap_file, const std::filesystem::path& tileset_file,
              std::string* error = nullptr);
    // Replaces what is open with the two files' (not undoable; clears the history); on failure the document is
    // unchanged and `error` says why. A tileset is edited with the map that uses it, so both are loaded together.
    bool Load(const std::filesystem::path& tilemap_file, const std::filesystem::path& tileset_file,
              std::string* error = nullptr);
    // Writes back to the files it was opened from or last saved to; false (with the reason) for a sample that has
    // never been saved.
    bool Save(std::string* error = nullptr);
    // Those files, empty for a sample.
    const std::filesystem::path& TilemapFile() const { return tilemap_file_; }
    const std::filesystem::path& TilesetFile() const { return tileset_file_; }
    bool dirty() const { return saved_state_ != state_ || stroke_changed_cells_ != 0; }

private:
    struct Snapshot {
        sprite2d::TilemapData map;
        sprite2d::Tileset tileset;
        u64 state;
        std::optional<usize> selection = std::nullopt;
    };
    void Record(bool track_selection = false);
    void Changed();
    void ClampActive();
    bool PaintCell(i32 x, i32 y, i32 value);

    sprite2d::TilemapData map_;
    sprite2d::Tileset tileset_;
    std::filesystem::path tilemap_file_;
    std::filesystem::path tileset_file_;
    bool in_stroke_ = false;
    Snapshot stroke_start_{};
    usize stroke_changed_cells_ = 0;
    std::vector<Snapshot> undo_, redo_;
    u64 revision_ = 1;
    // The view revision always moves forward; undo can return to an old edit state.
    u64 state_ = revision_;
    std::optional<u64> saved_state_ = state_;
};

} // namespace aether::editor
