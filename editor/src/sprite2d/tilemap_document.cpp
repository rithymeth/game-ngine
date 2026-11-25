#include "tilemap_document.h"

#include "aether/assets/asset_database.h"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <unordered_set>

namespace aether::editor {

using namespace sprite2d;

namespace {
namespace stdfs = std::filesystem;

// Place every payload beside its eventual destination, where final rename
// can replace the file without a partial cross-volume copy.
struct StagedTileAsset {
    stdfs::path directory;
    stdfs::path payload;

    ~StagedTileAsset() {
        if (directory.empty()) return;
        std::error_code ignored;
        stdfs::remove(payload, ignored);
        stdfs::remove(directory, ignored);
    }

    bool Prepare(const stdfs::path& target, std::string* error) {
        const auto refuse = [&] {
            if (error) *error = "Couldn't stage " + target.string();
            return false;
        };
        if (target.filename().empty()) return refuse();
        std::error_code status_error;
        const auto status = stdfs::symlink_status(target, status_error);
        if (status.type() != stdfs::file_type::not_found &&
            (status_error || !stdfs::is_regular_file(status))) return refuse();
        const auto parent = target.parent_path().empty() ? stdfs::path(".") : target.parent_path();
        std::error_code ec;
        stdfs::create_directories(parent, ec);
        if (ec) return refuse();
        auto staging = parent / (".aether-save-" + assets::ToString(assets::NewAssetGuid()));
        if (!stdfs::create_directory(staging, ec) || ec) return refuse();
        directory = std::move(staging);
        payload = directory / target.filename();
        return true;
    }
};
} // namespace

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

void TilemapEditDocument::Record(bool track_selection) {
    undo_.push_back({map_, tileset_, track_selection ? std::optional<usize>{active_layer} : std::nullopt});
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
    if (map_.width == 0 || map_.height == 0) return;
    const i32 left = std::max(0, std::min(x0, x1));
    const i32 bottom = std::max(0, std::min(y0, y1));
    const i32 right = std::min(static_cast<i32>(map_.width) - 1, std::max(x0, x1));
    const i32 top = std::min(static_cast<i32>(map_.height) - 1, std::max(y0, y1));
    if (left > right || bottom > top) return;
    BeginStroke();
    const i32 value = tool == TileTool::Erase ? kEmpty : brush;
    for (i32 y = bottom; y <= top; ++y) {
        for (i32 x = left; x <= right; ++x) PaintCell(x, y, value);
    }
    EndStroke();
}

bool TilemapEditDocument::Undo() {
    if (undo_.empty()) return false;
    const auto selection = undo_.back().selection;
    redo_.push_back({map_, tileset_, selection ? std::optional<usize>{active_layer} : std::nullopt});
    map_ = std::move(undo_.back().map);
    tileset_ = std::move(undo_.back().tileset);
    undo_.pop_back();
    if (selection) active_layer = *selection;
    ClampActive();
    Changed();
    return true;
}

bool TilemapEditDocument::Redo() {
    if (redo_.empty()) return false;
    const auto selection = redo_.back().selection;
    undo_.push_back({map_, tileset_, selection ? std::optional<usize>{active_layer} : std::nullopt});
    map_ = std::move(redo_.back().map);
    tileset_ = std::move(redo_.back().tileset);
    redo_.pop_back();
    if (selection) active_layer = *selection;
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

bool TilemapEditDocument::MoveLayer(usize from, usize to) {
    if (in_stroke_ || from == to || from >= map_.layers.size() || to >= map_.layers.size()) return false;
    Record(true);
    const auto begin = map_.layers.begin();
    const auto src = begin + static_cast<std::ptrdiff_t>(from);
    const auto dst = begin + static_cast<std::ptrdiff_t>(to);
    if (from < to) std::rotate(src, src + 1, dst + 1);
    else std::rotate(dst, src, src + 1);
    if (active_layer == from) active_layer = to;
    else if (from < active_layer && active_layer <= to) --active_layer;
    else if (to <= active_layer && active_layer < from) ++active_layer;
    return true;
}

bool TilemapEditDocument::DuplicateLayer(usize layer) {
    if (in_stroke_ || layer >= map_.layers.size()) return false;
    TileLayer copy = map_.layers[layer];
    {
        std::unordered_set<std::string_view> names;
        names.reserve(map_.layers.size());
        for (const auto& existing : map_.layers) names.emplace(existing.name);
        const std::string base = copy.name + " copy";
        copy.name = base;
        for (usize n = 2; names.count(copy.name) != 0; ++n) copy.name = base + " " + std::to_string(n);
    }
    Record(true);
    const usize destination = layer + 1;
    map_.layers.insert(map_.layers.begin() + static_cast<std::ptrdiff_t>(destination), std::move(copy));
    active_layer = destination;
    return true;
}

bool TilemapEditDocument::RenameLayer(usize layer, const std::string& name) {
    if (layer >= map_.layers.size() || map_.layers[layer].name == name ||
        std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isspace(c) != 0; })) return false;
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
                               std::string* error) {
    if (error) error->clear();
    StagedTileAsset map, tiles;
    if (!map.Prepare(tilemap_file, error) || !tiles.Prepare(tileset_file, error) ||
        !SaveTilemap(map.payload, map_, error) || !SaveTileset(tiles.payload, tileset_, error)) return false;
    std::error_code ec;
    stdfs::rename(map.payload, tilemap_file, ec);
    if (ec) {
        if (error) *error = "Couldn't replace " + tilemap_file.string() + ": " + ec.message();
        return false;
    }
    // Both files were staged, but an external change can still block the second rename.
    stdfs::rename(tiles.payload, tileset_file, ec);
    if (ec) {
        if (error) *error = "The map was saved, but couldn't replace " + tileset_file.string() + ": " + ec.message();
        return false;
    }
    tilemap_file_ = tilemap_file;
    tileset_file_ = tileset_file;
    dirty_ = false;
    return true;
}

bool TilemapEditDocument::Load(const std::filesystem::path& tilemap_file, const std::filesystem::path& tileset_file,
                               std::string* error) {
    TilemapData map;
    Tileset tileset;
    if (!LoadTilemap(tilemap_file, map, error) || !LoadTileset(tileset_file, tileset, error)) return false;
    map_ = std::move(map);
    tileset_ = std::move(tileset);
    if (map_.layers.empty()) map_.AddLayer("Layer 1");
    tilemap_file_ = tilemap_file;
    tileset_file_ = tileset_file;
    undo_.clear();
    redo_.clear();
    in_stroke_ = false;
    active_layer = 0;
    tool = TileTool::Paint;
    brush = 0;
    ++revision_;
    dirty_ = false;
    return true;
}

bool TilemapEditDocument::Save(std::string* error) {
    if (tilemap_file_.empty() || tileset_file_.empty()) {
        if (error != nullptr) *error = "This tilemap has never been saved, so there is nowhere to write it.";
        return false;
    }
    return Save(tilemap_file_, tileset_file_, error);
}

} // namespace aether::editor
