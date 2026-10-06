#include "tilemap_document.h"

#include <algorithm>

namespace aether::editor {

using namespace sprite2d;

TilemapEditDocument::TilemapEditDocument(TilemapData map, Tileset tileset)
    : map_(std::move(map)), tileset_(std::move(tileset)) {
    if (map_.layers.empty()) map_.AddLayer("Layer 1");
}

void TilemapEditDocument::Changed() {
    ++revision_;
    dirty_ = true;
}

void TilemapEditDocument::ClampActive() {
    if (active_layer >= map_.layers.size()) active_layer = map_.layers.empty() ? 0 : map_.layers.size() - 1;
}

void TilemapEditDocument::Record() {
    undo_.push_back({map_, tileset_});
    redo_.clear();
    Changed();
}

void TilemapEditDocument::BeginStroke() {
    if (in_stroke_) return;
    in_stroke_ = true;
    stroke_start_ = {map_, tileset_};
}

void TilemapEditDocument::EndStroke() {
    if (!in_stroke_) return;
    in_stroke_ = false;
    if (TilemapToJson(map_) == TilemapToJson(stroke_start_.map)) return; // nothing changed
    undo_.push_back(std::move(stroke_start_));
    redo_.clear();
    Changed();
}

bool TilemapEditDocument::PaintCell(i32 x, i32 y, i32 value) {
    if (active_layer >= map_.layers.size() || map_.Get(active_layer, x, y) == value) return false;
    return map_.Set(active_layer, x, y, value);
}

std::vector<std::pair<i32, i32>> TilemapEditDocument::FloodRegion(i32 x, i32 y) const {
    std::vector<std::pair<i32, i32>> region;
    if (active_layer >= map_.layers.size() || !map_.InBounds(x, y)) return region;
    const i32 target = map_.Get(active_layer, x, y);
    std::vector<bool> seen(static_cast<usize>(map_.width) * map_.height, false);
    std::vector<std::pair<i32, i32>> stack = {{x, y}};
    while (!stack.empty()) {
        const auto [cx, cy] = stack.back();
        stack.pop_back();
        if (!map_.InBounds(cx, cy)) continue;
        const usize index = static_cast<usize>(cy) * map_.width + static_cast<usize>(cx);
        if (seen[index] || map_.Get(active_layer, cx, cy) != target) continue;
        seen[index] = true;
        region.emplace_back(cx, cy);
        stack.insert(stack.end(), {{cx + 1, cy}, {cx - 1, cy}, {cx, cy + 1}, {cx, cy - 1}});
    }
    return region;
}

void TilemapEditDocument::Apply(i32 x, i32 y) {
    const bool own_stroke = !in_stroke_;
    if (own_stroke) BeginStroke();
    switch (tool) {
    case TileTool::Paint: PaintCell(x, y, brush); break;
    case TileTool::Erase: PaintCell(x, y, kEmpty); break;
    case TileTool::Fill:
        if (map_.Get(active_layer, x, y) != brush) {
            for (const auto& [cx, cy] : FloodRegion(x, y)) PaintCell(cx, cy, brush);
        }
        break;
    case TileTool::Pick:
        if (map_.InBounds(x, y)) brush = map_.Get(active_layer, x, y);
        break;
    case TileTool::Rectangle: break; // see Rectangle()
    }
    if (own_stroke) EndStroke();
}

void TilemapEditDocument::Rectangle(i32 x0, i32 y0, i32 x1, i32 y1) {
    BeginStroke();
    const i32 value = tool == TileTool::Erase ? kEmpty : brush;
    for (i32 y = std::min(y0, y1); y <= std::max(y0, y1); ++y) {
        for (i32 x = std::min(x0, x1); x <= std::max(x0, x1); ++x) PaintCell(x, y, value);
    }
    EndStroke();
}

bool TilemapEditDocument::Undo() {
    if (undo_.empty()) return false;
    redo_.push_back({map_, tileset_});
    map_ = std::move(undo_.back().map);
    tileset_ = std::move(undo_.back().tileset);
    undo_.pop_back();
    ClampActive();
    Changed();
    return true;
}

bool TilemapEditDocument::Redo() {
    if (redo_.empty()) return false;
    undo_.push_back({map_, tileset_});
    map_ = std::move(redo_.back().map);
    tileset_ = std::move(redo_.back().tileset);
    redo_.pop_back();
    ClampActive();
    Changed();
    return true;
}

usize TilemapEditDocument::AddLayer(const std::string& name) {
    Record();
    const usize index = map_.AddLayer(name);
    active_layer = index;
    return index;
}

bool TilemapEditDocument::RemoveLayer(usize layer) {
    if (layer >= map_.layers.size() || map_.layers.size() == 1) return false; // a map keeps one layer
    Record();
    map_.layers.erase(map_.layers.begin() + static_cast<std::ptrdiff_t>(layer));
    ClampActive();
    return true;
}

bool TilemapEditDocument::RenameLayer(usize layer, const std::string& name) {
    if (layer >= map_.layers.size() || map_.layers[layer].name == name) return false;
    Record();
    map_.layers[layer].name = name;
    return true;
}

bool TilemapEditDocument::SetLayerCollides(usize layer, bool collides) {
    if (layer >= map_.layers.size() || map_.layers[layer].collides == collides) return false;
    Record();
    map_.layers[layer].collides = collides;
    return true;
}

bool TilemapEditDocument::Resize(u32 width, u32 height) {
    if (width == 0 || height == 0 || static_cast<u64>(width) * height > 4u * 1024u * 1024u) return false;
    if (width == map_.width && height == map_.height) return false;
    Record();
    map_.Resize(width, height);
    return true;
}

void TilemapEditDocument::SetSolid(i32 tile, bool solid) {
    if (tile < 0 || tileset_.IsSolid(tile) == solid) return;
    Record();
    tileset_.SetSolid(tile, solid);
}

usize TilemapEditDocument::AddAutotile(const std::string& name) {
    Record();
    return tileset_.AddAutotile(name, Autotile().tiles);
}

bool TilemapEditDocument::SetAutotileTile(usize autotile, u8 mask, i32 tile) {
    if (autotile >= tileset_.autotiles.size() || mask >= 16 || tileset_.autotiles[autotile].tiles[mask] == tile) return false;
    Record();
    tileset_.autotiles[autotile].tiles[mask] = tile;
    return true;
}

bool TilemapEditDocument::Save(const std::filesystem::path& tilemap_file, const std::filesystem::path& tileset_file,
                               std::string* error) const {
    if (!SaveTilemap(tilemap_file, map_, error) || !SaveTileset(tileset_file, tileset_, error)) return false;
    const_cast<TilemapEditDocument*>(this)->dirty_ = false;
    return true;
}

} // namespace aether::editor
