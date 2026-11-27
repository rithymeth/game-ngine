#include "tilemap_document.h"

#include "aether/assets/asset_database.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <string_view>
#include <unordered_set>

namespace aether::editor {

using namespace sprite2d;

namespace {
namespace stdfs = std::filesystem;

bool SameSaveDestination(const stdfs::path& a, const stdfs::path& b) {
    if (a.empty() || b.empty()) return false;
    std::error_code ec;
    // Existing paths can alias through hard links or filesystem-specific spelling.
    if (stdfs::equivalent(a, b, ec)) return true;
    const auto normalize = [](const stdfs::path& path) {
        std::error_code ec;
        const auto absolute = stdfs::absolute(path, ec);
        if (ec) return path.lexically_normal();
        // Resolve existing parent components, even when the leaf has not been created.
        const auto canonical = stdfs::weakly_canonical(absolute, ec);
        return ec ? absolute.lexically_normal() : canonical;
    };
    return normalize(a) == normalize(b);
}

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
    state_ = ++revision_;
}

void TilemapEditDocument::ClampActive() {
    if (active_layer >= map_.layers.size()) active_layer = map_.layers.empty() ? 0 : map_.layers.size() - 1;
}

void TilemapEditDocument::Record(bool track_selection) {
    EndStroke();
    undo_.push_back({Snapshot{map_, tileset_, track_selection ? std::optional<usize>{active_layer} : std::nullopt}, state_});
    redo_.clear();
    Changed();
}

void TilemapEditDocument::RecordLayerProperties(usize layer) {
    EndStroke();
    const auto& current = map_.layers[layer];
    undo_.push_back({LayerProperties{layer, current.name, current.collides}, state_});
    redo_.clear();
    Changed();
}

void TilemapEditDocument::RecordTileset() {
    EndStroke();
    undo_.push_back({tileset_, state_});
    redo_.clear();
    Changed();
}

void TilemapEditDocument::BeginStroke() {
    if (in_stroke_) return;
    in_stroke_ = true;
}

void TilemapEditDocument::EndStroke() {
    if (!in_stroke_) return;
    in_stroke_ = false;
    if (stroke_changed_cells_ != 0) {
        if (stroke_start_) undo_.push_back({std::move(*stroke_start_), state_});
        else undo_.push_back({std::move(stroke_cells_), state_});
        stroke_changed_cells_ = 0;
        redo_.clear();
        Changed();
    }
    stroke_start_.reset();
    stroke_cells_.clear();
}

bool TilemapEditDocument::PaintCell(i32 x, i32 y, i32 value) {
    if (active_layer >= map_.layers.size() || !map_.InBounds(x, y)) return false;
    const u64 index = static_cast<u64>(y) * map_.width + static_cast<u32>(x);
    if (index >= map_.layers[active_layer].cells.size()) return false;
    return PaintCellAt(static_cast<usize>(index), value);
}

bool TilemapEditDocument::PaintCellAt(usize index, i32 value) {
    auto& cells = map_.layers[active_layer].cells;
    auto& cell = cells[index];
    if (cell == value) return false;
    if (!stroke_start_) {
        const auto address = std::pair{active_layer, index};
        auto it = stroke_cells_.find(address);
        // Bound node overhead for broad fills. On promotion retain the start
        // of the gesture, not the partially painted state.
        const usize sparse_limit = std::clamp<usize>(cells.size() / 16, 32, 4096);
        if (it == stroke_cells_.end() && stroke_cells_.size() >= sparse_limit) {
            stroke_start_ = Snapshot{map_, tileset_};
            for (const auto& [key, original] : stroke_cells_)
                stroke_start_->map.layers[key.first].cells[key.second] = original;
            stroke_cells_.clear();
        } else {
            if (it == stroke_cells_.end()) stroke_cells_.emplace(address, cell);
            else if (it->second == value) stroke_cells_.erase(it);
            cell = value;
            stroke_changed_cells_ = stroke_cells_.size();
            return true;
        }
    }

    const i32 original = stroke_start_->map.layers[active_layer].cells[index];
    if (cell == original) ++stroke_changed_cells_;
    else if (value == original) --stroke_changed_cells_;
    cell = value;
    return true;
}

std::vector<std::pair<i32, i32>> TilemapEditDocument::FloodRegion(i32 x, i32 y) const {
    std::vector<std::pair<i32, i32>> region;
    if (active_layer >= map_.layers.size() || !map_.InBounds(x, y)) return region;
    const i32 target = map_.Get(active_layer, x, y);
    std::vector<bool> seen(static_cast<usize>(map_.width) * map_.height, false);
    // The result doubles as the queue; each cell is examined at most once.
    const auto enqueue = [&](i32 cx, i32 cy) {
        if (!map_.InBounds(cx, cy)) return;
        const usize index = static_cast<usize>(cy) * map_.width + static_cast<usize>(cx);
        if (seen[index]) return;
        seen[index] = true;
        if (map_.Get(active_layer, cx, cy) == target) region.emplace_back(cx, cy);
    };
    enqueue(x, y);
    for (usize i = 0; i < region.size(); ++i) {
        const auto [cx, cy] = region[i]; // Copy before enqueue can grow the vector.
        enqueue(cx + 1, cy);
        enqueue(cx - 1, cy);
        enqueue(cx, cy + 1);
        enqueue(cx, cy - 1);
    }
    return region;
}

void TilemapEditDocument::FloodPaint(i32 x, i32 y) {
    if (active_layer >= map_.layers.size() || !map_.InBounds(x, y)) return;
    const i32 target = map_.Get(active_layer, x, y);
    if (target == brush) return;
    const auto& cells = map_.layers[active_layer].cells;
    const usize width = map_.width;
    const usize height = map_.height;
    if (cells.size() != static_cast<u64>(width) * height) {
        // Retain Get's missing-cells-as-empty behavior for noncanonical data.
        for (const auto& [cx, cy] : FloodRegion(x, y)) PaintCell(cx, cy, brush);
        return;
    }

    struct Run { usize row, first, end; }; // end is exclusive
    std::deque<Run> pending;
    // As each horizontal run is discovered, paint it immediately. The new
    // brush value marks it visited, with no per-cell queue or visited bitmap.
    const auto paint_run = [&](usize row, usize col) {
        const usize base = row * width;
        usize first = col;
        usize end = col + 1;
        while (first > 0 && cells[base + first - 1] == target) --first;
        while (end < width && cells[base + end] == target) ++end;
        pending.push_back({row, first, end});
        for (usize i = base + first; i < base + end; ++i) PaintCellAt(i, brush);
        return end;
    };
    paint_run(static_cast<usize>(y), static_cast<usize>(x));
    while (!pending.empty()) {
        const auto [row, first, end] = pending.front();
        pending.pop_front();
        const auto scan = [&](usize adjacent) {
            const usize base = adjacent * width;
            for (usize col = first; col < end; ) {
                if (cells[base + col] == target) col = paint_run(adjacent, col);
                else ++col;
            }
        };
        if (row > 0) scan(row - 1);
        if (row + 1 < height) scan(row + 1);
    }
}

void TilemapEditDocument::Apply(i32 x, i32 y) {
    if (tool == TileTool::Pick) {
        if (map_.InBounds(x, y)) brush = map_.Get(active_layer, x, y);
        return; // Selecting a brush does not need a map snapshot.
    }
    const bool own_stroke = !in_stroke_;
    if (own_stroke) BeginStroke();
    switch (tool) {
    case TileTool::Paint: PaintCell(x, y, brush); break;
    case TileTool::Erase: PaintCell(x, y, kEmpty); break;
    case TileTool::Fill: FloodPaint(x, y); break;
    case TileTool::Pick: break; // handled before starting a stroke
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

bool TilemapEditDocument::Travel(std::vector<HistoryEntry>& from, std::vector<HistoryEntry>& to) {
    EndStroke();
    if (from.empty()) return false;
    to.push_back(std::move(from.back()));
    from.pop_back();
    auto& step = to.back();
    // Keep the opposite state in the same entry. Swapping avoids copying a
    // full map on either stack, including for structural history.
    if (auto* snapshot = std::get_if<Snapshot>(&step.edit)) {
        std::swap(map_, snapshot->map);
        std::swap(tileset_, snapshot->tileset);
        if (snapshot->selection) std::swap(active_layer, *snapshot->selection);
    } else if (auto* properties = std::get_if<LayerProperties>(&step.edit)) {
        auto& layer = map_.layers[properties->layer];
        std::swap(layer.name, properties->name);
        std::swap(layer.collides, properties->collides);
    } else if (auto* move = std::get_if<LayerMove>(&step.edit)) {
        RotateLayers(move->from, move->to);
        std::swap(move->from, move->to);
        std::swap(active_layer, move->selection);
    } else if (auto* tileset = std::get_if<Tileset>(&step.edit)) {
        std::swap(tileset_, *tileset);
    } else {
        for (auto& [key, value] : std::get<CellEdits>(step.edit))
            std::swap(map_.layers[key.first].cells[key.second], value);
    }
    std::swap(state_, step.state);
    ClampActive();
    ++revision_;
    return true;
}

bool TilemapEditDocument::Undo() { return Travel(undo_, redo_); }
bool TilemapEditDocument::Redo() { return Travel(redo_, undo_); }

usize TilemapEditDocument::AddLayer(const std::string& name) {
    Record(true);
    const usize index = map_.AddLayer(name);
    active_layer = index;
    return index;
}

bool TilemapEditDocument::RemoveLayer(usize layer) {
    if (layer >= map_.layers.size() || map_.layers.size() == 1) return false; // a map keeps one layer
    Record(true);
    map_.layers.erase(map_.layers.begin() + static_cast<std::ptrdiff_t>(layer));
    if (layer < active_layer) --active_layer;
    ClampActive();
    return true;
}

void TilemapEditDocument::RotateLayers(usize from, usize to) {
    const auto begin = map_.layers.begin();
    const auto src = begin + static_cast<std::ptrdiff_t>(from);
    const auto dst = begin + static_cast<std::ptrdiff_t>(to);
    if (from < to) std::rotate(src, src + 1, dst + 1);
    else std::rotate(dst, src, src + 1);
}

bool TilemapEditDocument::MoveLayer(usize from, usize to) {
    if (in_stroke_ || from == to || from >= map_.layers.size() || to >= map_.layers.size()) return false;
    // Moving whole layers is reversible without copying their tile storage.
    undo_.push_back({LayerMove{to, from, active_layer}, state_});
    redo_.clear();
    Changed();
    RotateLayers(from, to);
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
    RecordLayerProperties(layer);
    map_.layers[layer].name = name;
    return true;
}

bool TilemapEditDocument::SetLayerCollides(usize layer, bool collides) {
    if (layer >= map_.layers.size() || map_.layers[layer].collides == collides) return false;
    RecordLayerProperties(layer);
    map_.layers[layer].collides = collides;
    return true;
}

bool TilemapEditDocument::CanResize(u32 width, u32 height) const {
    return !in_stroke_ && width > 0 && height > 0 && static_cast<u64>(width) * height <= 4u * 1024u * 1024u &&
           (width != map_.width || height != map_.height);
}

bool TilemapEditDocument::Resize(u32 width, u32 height) {
    if (!CanResize(width, height)) return false;
    Record();
    map_.Resize(width, height);
    return true;
}

void TilemapEditDocument::SetSolid(i32 tile, bool solid) {
    if (tile < 0 || tileset_.IsSolid(tile) == solid) return;
    RecordTileset();
    tileset_.SetSolid(tile, solid);
}

usize TilemapEditDocument::AddAutotile(const std::string& name) {
    RecordTileset();
    return tileset_.AddAutotile(name, Autotile().tiles);
}

bool TilemapEditDocument::SetAutotileTile(usize autotile, u8 mask, i32 tile) {
    if (autotile >= tileset_.autotiles.size() || mask >= 16 || tileset_.autotiles[autotile].tiles[mask] == tile) return false;
    RecordTileset();
    tileset_.autotiles[autotile].tiles[mask] = tile;
    return true;
}

bool TilemapEditDocument::FillEmptyAutotileMasks(usize autotile, i32 tile) {
    if (in_stroke_ || autotile >= tileset_.autotiles.size() || tile < 0 ||
        static_cast<u32>(tile) >= tileset_.TileCount()) return false;
    auto& variants = tileset_.autotiles[autotile].tiles;
    if (std::find(variants.begin(), variants.end(), kEmpty) == variants.end()) return false;
    RecordTileset();
    std::replace(variants.begin(), variants.end(), kEmpty, tile);
    return true;
}

bool TilemapEditDocument::Save(const std::filesystem::path& tilemap_file, const std::filesystem::path& tileset_file,
                               std::string* error) {
    EndStroke();
    if (error) error->clear();
    if (SameSaveDestination(tilemap_file, tileset_file)) {
        if (error) *error = "The tilemap and tileset must be saved to distinct files.";
        return false;
    }
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
        saved_state_.reset(); // The previous pair on disk may no longer match its checkpoint.
        if (error) *error = "The map was saved, but couldn't replace " + tileset_file.string() + ": " + ec.message();
        return false;
    }
    tilemap_file_ = tilemap_file;
    tileset_file_ = tileset_file;
    saved_state_ = state_;
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
    stroke_start_.reset();
    stroke_cells_.clear();
    stroke_changed_cells_ = 0;
    active_layer = 0;
    tool = TileTool::Paint;
    brush = 0;
    state_ = ++revision_;
    saved_state_ = state_;
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
