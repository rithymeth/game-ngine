#pragma once

#include "aether/assets/asset_guid.h"
#include "aether/core/base.h"

#include <nlohmann/json.hpp>

#include <array>
#include <filesystem>
#include <string>
#include <vector>

// Tilesets and tilemaps (Phase 26 step 5, docs/design/PHASE_SPECS.md §26.6).
//
// A Tileset (`.atileset`) cuts a texture into equal tiles, numbered in
// reading order from 0 at the top left, with a collision flag per tile and
// autotile rules. A TilemapData (`.atilemap`) is a grid of cells in
// layers, over a tileset. Cells grow right (x) and up (y) from the entity's
// origin, so row 0 is the bottom row.
//
// A cell holds a tile number (>= 0), kEmpty, or an autotile reference
// (kAutotileBase - index): that cell then shows whichever tile of the
// autotile matches which of its four neighbours hold the same reference.

namespace aether::sprite2d {

constexpr i32 kEmpty = -1;
constexpr i32 kAutotileBase = -2;
inline constexpr i32 AutotileCell(usize autotile) { return kAutotileBase - static_cast<i32>(autotile); }
inline constexpr bool IsAutotileCell(i32 cell) { return cell <= kAutotileBase; }
inline constexpr usize AutotileIndex(i32 cell) { return static_cast<usize>(kAutotileBase - cell); }

// Neighbour bits of an autotile mask.
enum NeighbourBit : u8 { kNorth = 1, kEast = 2, kSouth = 4, kWest = 8 };

struct Autotile {
    std::string name;
    // The tile for each mask of same-reference neighbours (north 1, east 2,
    // south 4, west 8); -1 draws nothing. Mask 0 is an island, 15 the middle.
    std::array<i32, 16> tiles;
    Autotile() { tiles.fill(kEmpty); }
};

struct Tileset {
    assets::AssetGuid texture;
    u32 texture_width = 0, texture_height = 0;
    u32 tile_width = 16, tile_height = 16;
    u32 margin = 0;  // pixels around the whole sheet
    u32 spacing = 0; // pixels between tiles
    std::vector<u8> solid; // by tile number: 1 blocks (missing = 0)
    std::vector<Autotile> autotiles;

    u32 Columns() const;
    u32 Rows() const;
    u32 TileCount() const { return Columns() * Rows(); }
    // {u0, v0, u1, v1} of a tile, v growing downward; false if out of range.
    bool Uv(i32 tile, f32 out[4]) const;
    bool IsSolid(i32 tile) const { return tile >= 0 && static_cast<usize>(tile) < solid.size() && solid[static_cast<usize>(tile)] != 0; }
    void SetSolid(i32 tile, bool value);
    // Adds an autotile from its 16 tile numbers (by neighbour mask); returns its index.
    usize AddAutotile(const std::string& name, const std::array<i32, 16>& tiles);
};

nlohmann::json TilesetToJson(const Tileset& tileset);
bool TilesetFromJson(const nlohmann::json& data, Tileset& out, std::string* error = nullptr);
bool LoadTileset(const std::filesystem::path& file, Tileset& out, std::string* error = nullptr);
bool SaveTileset(const std::filesystem::path& file, const Tileset& tileset, std::string* error = nullptr);

struct TileLayer {
    std::string name;
    std::vector<i32> cells; // width * height, row-major from the bottom row
    bool collides = true;   // its solid tiles block, in IsSolidAt
};

struct TilemapData {
    assets::AssetGuid tileset;
    u32 width = 0, height = 0;
    std::vector<TileLayer> layers;

    // Sets the size, keeping the cells that still fit; each layer gets
    // width * height cells (new ones empty).
    void Resize(u32 new_width, u32 new_height);
    usize AddLayer(const std::string& name);
    bool InBounds(i32 x, i32 y) const { return x >= 0 && y >= 0 && static_cast<u32>(x) < width && static_cast<u32>(y) < height; }
    // kEmpty outside the map.
    i32 Get(usize layer, i32 x, i32 y) const;
    bool Set(usize layer, i32 x, i32 y, i32 cell);
    // Fills a rectangle (inclusive corners, clamped); returns the cells set.
    usize Fill(usize layer, i32 x0, i32 y0, i32 x1, i32 y1, i32 cell);
};

nlohmann::json TilemapToJson(const TilemapData& map);
bool TilemapFromJson(const nlohmann::json& data, TilemapData& out, std::string* error = nullptr);
bool LoadTilemap(const std::filesystem::path& file, TilemapData& out, std::string* error = nullptr);
bool SaveTilemap(const std::filesystem::path& file, const TilemapData& map, std::string* error = nullptr);

// Which of the cell's four neighbours hold the same cell value.
u8 NeighbourMask(const TilemapData& map, usize layer, i32 x, i32 y);
// The tile number to draw for the cell: its own, an autotile's by mask, or -1.
i32 ResolveTile(const TilemapData& map, const Tileset& tileset, usize layer, i32 x, i32 y);
// Whether any colliding layer has a solid tile at the cell (outside: false).
bool IsSolidAt(const TilemapData& map, const Tileset& tileset, i32 x, i32 y);

// Tile <-> local position (units; the cell size is `tile_units` square).
inline void CellToLocal(i32 x, i32 y, f32 tile_units, f32& out_x, f32& out_y) {
    out_x = static_cast<f32>(x) * tile_units;
    out_y = static_cast<f32>(y) * tile_units;
}
void LocalToCell(f32 local_x, f32 local_y, f32 tile_units, i32& out_x, i32& out_y);

} // namespace aether::sprite2d
