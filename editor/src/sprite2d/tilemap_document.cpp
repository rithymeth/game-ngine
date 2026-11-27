#include "tilemap_document.h"

#include "aether/assets/asset_database.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
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

TilemapEditDocument::LayerPresence& TilemapEditDocument::RecordLayerPresence(usize index) {
    EndStroke();
    undo_.push_back({LayerPresence{index, active_layer, std::nullopt}, state_});
    redo_.clear();
    Changed();
    return std::get<LayerPresence>(undo_.back().edit);
}

void TilemapEditDocument::BeginStroke() {
    if (in_stroke_) return;
    in_stroke_ = true;
}

void TilemapEditDocument::EndStroke() {
    if (!in_stroke_) return;
    in_stroke_ = false;
    if (stroke_changed_cells_ != 0) {
        std::erase_if(stroke_, [](const auto& entry) { return entry.second.changed == 0; });
        undo_.push_back({std::move(stroke_), state_});
        stroke_changed_cells_ = 0;
        redo_.clear();
        Changed();
    }
    stroke_.clear();
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
    auto& layer = stroke_[active_layer];
    i32 original = cell;
    if (auto* sparse = std::get_if<SparseCells>(&layer.cells)) {
        const auto it = sparse->find(index);
        const usize sparse_limit = std::clamp<usize>(cells.size() / 16, 32, 4096);
        if (it == sparse->end() && sparse->size() >= sparse_limit) {
            // Promote only this layer and reconstruct its pre-stroke values.
            auto snapshot = cells;
            for (const auto& [offset, old] : *sparse) snapshot[offset] = old;
            layer.cells = std::move(snapshot);
        } else if (it == sparse->end()) {
            sparse->emplace(index, cell);
        } else {
            original = it->second;
            if (original == value) sparse->erase(it);
        }
    } else {
        original = std::get<std::vector<i32>>(layer.cells)[index];
    }
    if (cell == original) {
        ++layer.changed;
        ++stroke_changed_cells_;
    } else if (value == original) {
        --layer.changed;
        --stroke_changed_cells_;
    }
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
    auto& cells = map_.layers[active_layer].cells;
    const usize width = map_.width;
    const usize height = map_.height;
    if (cells.size() != static_cast<u64>(width) * height) {
        // Retain Get's missing-cells-as-empty behavior for noncanonical data.
        for (const auto& [cx, cy] : FloodRegion(x, y)) PaintCell(cx, cy, brush);
        return;
    }

    auto& edit = stroke_[active_layer];
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
        usize i = base + first;
        const usize stop = base + end;
        while (i < stop && std::holds_alternative<SparseCells>(edit.cells)) PaintCellAt(i++, brush);
        if (i < stop) {
            // Every cell in the remaining run has the same live value. Count
            // new and reverted changes against the original row, then write once.
            const auto& original = std::get<std::vector<i32>>(edit.cells);
            usize changed = 0, restored = 0;
            for (usize offset = i; offset < stop; ++offset) {
                changed += original[offset] == target;
                restored += original[offset] == brush;
            }
            edit.changed = edit.changed + changed - restored;
            stroke_changed_cells_ = stroke_changed_cells_ + changed - restored;
            std::fill_n(cells.data() + i, stop - i, brush);
        }
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

bool TilemapEditDocument::HasMissingAutotileBrush() const {
    return IsAutotileCell(brush) && AutotileIndex(brush) >= tileset_.autotiles.size();
}

bool TilemapEditDocument::HasOutOfGridBrush() const {
    if (brush < 0 || tileset_.texture.IsNull()) return false;
    const u32 count = tileset_.TileCount();
    return count != 0 && static_cast<u32>(brush) >= count;
}

void TilemapEditDocument::Apply(i32 x, i32 y) {
    if (tool == TileTool::Pick) {
        if (map_.InBounds(x, y)) brush = map_.Get(active_layer, x, y);
        return; // Selecting a brush does not need a map snapshot.
    }
    if ((tool == TileTool::Paint || tool == TileTool::Fill) && (HasMissingAutotileBrush() || HasOutOfGridBrush())) return;
    const bool own_stroke = !in_stroke_;
    if (own_stroke) BeginStroke();
    switch (tool) {
    case TileTool::Paint: PaintCell(x, y, brush); break;
    case TileTool::Erase: PaintCell(x, y, kEmpty); break;
    case TileTool::Fill: FloodPaint(x, y); break;
    case TileTool::Pick: break; // handled before starting a stroke
    case TileTool::Rectangle:
    case TileTool::Line: break; // use the two-endpoint operations
    }
    if (own_stroke) EndStroke();
}

void TilemapEditDocument::Rectangle(i32 x0, i32 y0, i32 x1, i32 y1) {
    if (map_.width == 0 || map_.height == 0 ||
        (tool != TileTool::Erase && (HasMissingAutotileBrush() || HasOutOfGridBrush()))) return;
    const i32 left = std::max(0, std::min(x0, x1));
    const i32 bottom = std::max(0, std::min(y0, y1));
    const i32 right = std::min(static_cast<i32>(map_.width) - 1, std::max(x0, x1));
    const i32 top = std::min(static_cast<i32>(map_.height) - 1, std::max(y0, y1));
    if (left > right || bottom > top) return;
    BeginStroke();
    const i32 value = tool == TileTool::Erase ? kEmpty : brush;
    if (active_layer < map_.layers.size()) {
        auto& cells = map_.layers[active_layer].cells;
        auto& edit = stroke_[active_layer];
        for (i32 y = bottom; y <= top; ++y) {
            const u64 base = static_cast<u64>(y) * map_.width;
            const u64 first = base + static_cast<u32>(left);
            if (first >= cells.size()) break;
            // Public layers can have a truncated backing array.
            const usize stop = static_cast<usize>(std::min<u64>(cells.size(), base + static_cast<u32>(right) + 1));
            usize i = static_cast<usize>(first);
            while (i < stop && std::holds_alternative<SparseCells>(edit.cells)) PaintCellAt(i++, value);
            if (i == stop) continue;

            // Count the live and replacement differences against the original
            // before writing this dense span, including already-matching cells.
            const auto& original = std::get<std::vector<i32>>(edit.cells);
            usize before = 0, after = 0;
            for (usize offset = i; offset < stop; ++offset) {
                before += cells[offset] != original[offset];
                after += value != original[offset];
            }
            edit.changed = edit.changed - before + after;
            stroke_changed_cells_ = stroke_changed_cells_ - before + after;
            std::fill_n(cells.data() + i, stop - i, value);
        }
    }
    EndStroke();
}

void TilemapEditDocument::Line(i32 x, i32 y, i32 end_x, i32 end_y) {
    if (!map_.InBounds(x, y) || !map_.InBounds(end_x, end_y) ||
        (tool != TileTool::Erase && (HasMissingAutotileBrush() || HasOutOfGridBrush()))) return;
    const bool own_stroke = !in_stroke_;
    if (own_stroke) BeginStroke();
    const i32 value = tool == TileTool::Erase ? kEmpty : brush;
    const i64 dx = std::abs(static_cast<i64>(end_x) - x);
    const i64 dy = -std::abs(static_cast<i64>(end_y) - y);
    const i32 step_x = x < end_x ? 1 : -1, step_y = y < end_y ? 1 : -1;
    i64 error = dx + dy;
    for (;;) {
        PaintCell(x, y, value);
        if (x == end_x && y == end_y) break;
        const i64 twice = 2 * error;
        if (twice >= dy) { error += dy; x += step_x; }
        if (twice <= dx) { error += dx; y += step_y; }
    }
    if (own_stroke) EndStroke();
}

bool TilemapEditDocument::Travel(std::vector<HistoryEntry>& from, std::vector<HistoryEntry>& to) {
    EndStroke();
    if (from.empty()) return false;
    to.push_back(std::move(from.back()));
    from.pop_back();
    auto& step = to.back();
    // Keep the opposite state in the same entry. Swapping avoids copying a
    // full map on either stack, including for structural history.
    if (auto* opposite_map = std::get_if<TilemapData>(&step.edit)) {
        std::swap(map_, *opposite_map);
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
    } else if (auto* presence = std::get_if<LayerPresence>(&step.edit)) {
        const auto where = map_.layers.begin() + static_cast<std::ptrdiff_t>(presence->index);
        if (presence->removed) {
            map_.layers.insert(where, std::move(*presence->removed));
            presence->removed.reset();
        } else {
            presence->removed.emplace(std::move(*where));
            map_.layers.erase(where);
        }
        std::swap(active_layer, presence->selection);
    } else if (auto* height = std::get_if<HeightEdit>(&step.edit)) {
        SwapHeight(*height);
    } else {
        for (auto& [layer, edit] : std::get<BrushEdit>(step.edit)) {
            auto& cells = map_.layers[layer].cells;
            if (auto* dense = std::get_if<std::vector<i32>>(&edit.cells)) {
                cells.swap(*dense);
            } else {
                for (auto& [offset, value] : std::get<SparseCells>(edit.cells))
                    std::swap(cells[offset], value);
            }
        }
    }
    std::swap(state_, step.state);
    ClampActive();
    ++revision_;
    return true;
}

bool TilemapEditDocument::Undo() { return Travel(undo_, redo_); }
bool TilemapEditDocument::Redo() { return Travel(redo_, undo_); }

usize TilemapEditDocument::AddLayer(const std::string& name) {
    RecordLayerPresence(map_.layers.size());
    const usize index = map_.AddLayer(name);
    active_layer = index;
    return index;
}

bool TilemapEditDocument::RemoveLayer(usize layer) {
    if (layer >= map_.layers.size() || map_.layers.size() == 1) return false; // a map keeps one layer
    auto& edit = RecordLayerPresence(layer);
    edit.removed.emplace(std::move(map_.layers[layer]));
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
    const usize destination = layer + 1;
    RecordLayerPresence(destination);
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
    if (in_stroke_ || width == 0 || height == 0 || (width == map_.width && height == map_.height))
        return false;
    // Loaded assets can exceed the editor's growth budget. Let authors crop
    // those maps without having to discard enough tiles to reach the cap at once.
    return static_cast<u64>(width) * height <= 4u * 1024u * 1024u ||
           (width <= map_.width && height <= map_.height);
}

void TilemapEditDocument::SwapHeight(HeightEdit& edit) {
    const usize cell_count = static_cast<usize>(map_.width) * edit.height;
    if (edit.height < map_.height) {
        // A height-only crop removes a contiguous tail from each layer.
        edit.cropped_rows.resize(map_.layers.size());
        for (usize i = 0; i < map_.layers.size(); ++i) {
            auto& cells = map_.layers[i].cells;
            edit.cropped_rows[i].assign(cells.begin() + static_cast<std::ptrdiff_t>(cell_count), cells.end());
        }
        for (auto& layer : map_.layers) layer.cells.resize(cell_count);
    } else {
        for (usize i = 0; i < map_.layers.size(); ++i) {
            auto& cells = map_.layers[i].cells;
            if (edit.cropped_rows.empty()) cells.resize(cell_count, kEmpty);
            else cells.insert(cells.end(), edit.cropped_rows[i].begin(), edit.cropped_rows[i].end());
        }
        edit.cropped_rows.clear();
    }
    std::swap(map_.height, edit.height);
}

bool TilemapEditDocument::Resize(u32 width, u32 height) {
    if (!CanResize(width, height)) return false;
    const u64 old_count = static_cast<u64>(map_.width) * map_.height;
    // For a deep crop, move the old allocation into history and copy the
    // smaller retained prefix instead of copying most of the original map.
    const bool deep_height_crop = width == map_.width && static_cast<u64>(height) * 2 < map_.height;
    if (width == map_.width && !deep_height_crop &&
        std::all_of(map_.layers.begin(), map_.layers.end(),
                    [old_count](const TileLayer& layer) { return layer.cells.size() == old_count; })) {
        undo_.push_back({HeightEdit{height, {}}, state_});
        SwapHeight(std::get<HeightEdit>(undo_.back().edit));
        redo_.clear();
        Changed();
    } else {
        // Build the replacement first, keeping each original layer allocation
        // for undo instead of copying both the map and its unchanged tileset.
        TilemapData replacement;
        replacement.tileset = map_.tileset;
        replacement.width = width;
        replacement.height = height;
        replacement.layers.reserve(map_.layers.size());
        const u32 overlap_width = std::min(width, map_.width);
        const u32 overlap_height = std::min(height, map_.height);
        const usize kept_count = static_cast<usize>(width) * height;
        for (const auto& old : map_.layers) {
            if (deep_height_crop && kept_count <= old.cells.size()) {
                const auto end = old.cells.begin() + static_cast<std::ptrdiff_t>(kept_count);
                replacement.layers.push_back({old.name, std::vector<i32>(old.cells.begin(), end), old.collides});
                continue;
            }
            const usize index = replacement.AddLayer(old.name);
            auto& next = replacement.layers[index];
            next.collides = old.collides;
            for (u32 y = 0; overlap_width != 0 && y < overlap_height; ++y) {
                const u64 offset = static_cast<u64>(y) * map_.width;
                if (offset >= old.cells.size()) break;
                const auto src = static_cast<usize>(offset);
                const usize count = std::min<usize>(overlap_width, old.cells.size() - src);
                std::copy_n(old.cells.data() + src, count, next.cells.data() + static_cast<usize>(y) * width);
            }
        }
        // Allocate the history entry before taking ownership of the live map.
        undo_.push_back({TilemapData{}, state_});
        std::get<TilemapData>(undo_.back().edit) = std::move(map_);
        map_ = std::move(replacement);
        redo_.clear();
        Changed();
    }
    return true;
}

void TilemapEditDocument::SetSolid(i32 tile, bool solid) {
    if (tile < 0 || tileset_.IsSolid(tile) == solid) return;
    // A stale flag from a resized sheet must still be removable.
    if (solid && !tileset_.texture.IsNull()) {
        const u32 count = tileset_.TileCount();
        if (count != 0 && static_cast<u32>(tile) >= count) return;
    }
    RecordTileset();
    tileset_.SetSolid(tile, solid);
}

usize TilemapEditDocument::AddAutotile(const std::string& name) {
    RecordTileset();
    return tileset_.AddAutotile(name, Autotile().tiles);
}

bool TilemapEditDocument::SetAutotileTile(usize autotile, u8 mask, i32 tile) {
    if (autotile >= tileset_.autotiles.size() || mask >= 16 || tileset_.autotiles[autotile].tiles[mask] == tile) return false;
    // Numbered maps without an authored texture grid still allow arbitrary tile IDs.
    if (tile >= 0 && !tileset_.texture.IsNull()) {
        const u32 count = tileset_.TileCount();
        if (count != 0 && static_cast<u32>(tile) >= count) return false;
    }
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
    if (!LoadTilemap(tilemap_file, map, error)) return false;
    return LoadDecodedMap(std::move(map), tilemap_file, tileset_file, error);
}

bool TilemapEditDocument::LoadDecodedMap(TilemapData map, const std::filesystem::path& tilemap_file,
                                         const std::filesystem::path& tileset_file, std::string* error) {
    Tileset tileset;
    if (!LoadTileset(tileset_file, tileset, error)) return false;
    if (map.layers.empty()) map.AddLayer("Layer 1");
    map_ = std::move(map);
    tileset_ = std::move(tileset);
    tilemap_file_ = tilemap_file;
    tileset_file_ = tileset_file;
    undo_.clear();
    redo_.clear();
    in_stroke_ = false;
    stroke_.clear();
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
