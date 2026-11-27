#include "tilemap_panel.h"

#include "aether/assets/asset_database.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>

namespace aether::editor {

using namespace sprite2d;

namespace {

// A stable colour per tile number, so a tileset reads at a glance.
ImU32 TileColor(i32 tile) {
    const u32 h = static_cast<u32>(tile) * 2654435761u;
    const f32 hue = static_cast<f32>((h >> 8) & 0xFFFF) / 65535.0f;
    ImVec4 c;
    ImGui::ColorConvertHSVtoRGB(hue, 0.55f, 0.85f, c.x, c.y, c.z);
    return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, 1.0f));
}

bool IsInvalidPaint(i32 cell, u32 grid_count, usize autotile_count) {
    if (IsAutotileCell(cell)) return AutotileIndex(cell) >= autotile_count;
    return cell >= 0 && grid_count != 0 && static_cast<u32>(cell) >= grid_count;
}

const char* kToolNames[] = {"Paint", "Erase", "Rectangle", "Fill", "Pick", "Line"};

} // namespace

bool TilemapPanel::LoadJumpProofOverlay(const std::filesystem::path& path, std::string* error) {
    const auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > 4 * 1024 * 1024) return fail("Choose an overlay JSON file up to 4 MiB.");
    std::ifstream in(path, std::ios::binary);
    std::string text(static_cast<usize>(size), '\0');
    if (!in.read(text.data(), static_cast<std::streamsize>(size)) || in.peek() != std::ifstream::traits_type::eof())
        return fail("Couldn't read the overlay file.");
    const auto data = nlohmann::json::parse(text, nullptr, false);
    if (!data.is_object() || !data.contains("frames") || !data["frames"].is_number_unsigned())
        return fail("Expected Jump Proof overlay data.");
    const u64 count = data["frames"].get<u64>();
    if (count == 0 || count > 20000 || !data.contains("first_difference_frame"))
        return fail("Invalid overlay frame count or marker.");
    const auto& marker = data["first_difference_frame"];
    if (!marker.is_null() && (!marker.is_number_unsigned() || marker.get<u64>() == 0 || marker.get<u64>() > count))
        return fail("The divergence marker is outside the trace.");
    const auto parse = [&](const char* key, std::vector<GhostPoint>& points) {
        if (!data.contains(key) || !data[key].is_array() || data[key].size() != count) return false;
        points.reserve(static_cast<usize>(count));
        for (const auto& raw : data[key]) {
            if (!raw.is_array() || raw.size() != 3 || !raw[0].is_number() || !raw[1].is_number()) return false;
            const f64 x = raw[0].get<f64>(), y = raw[1].get<f64>();
            if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 1000000 || std::abs(y) > 1000000) return false;
            points.push_back({static_cast<f32>(x), static_cast<f32>(y)});
        }
        return true;
    };
    std::vector<GhostPoint> before, after;
    if (!parse("before_path", before) || !parse("after_path", after))
        return fail("The file must contain two complete position paths. Export with --overlay-data.");
    std::optional<GhostVerdicts> verdicts;
    if (data.contains("before_status") || data.contains("after_status") || data.contains("verdict_changed")) {
        const auto status = [&](const char* key) -> std::optional<bool> {
            const auto it = data.find(key);
            if (it == data.end() || !it->is_string()) return std::nullopt;
            if (*it == "pass") return true;
            if (*it == "missed") return false;
            return std::nullopt;
        };
        const auto a = status("before_status"), b = status("after_status");
        if (!a || !b) return fail("Expected both Jump Proof target verdicts (pass or missed).");
        if (const auto changed = data.find("verdict_changed"); changed != data.end() &&
            (!changed->is_boolean() || changed->get<bool>() != (*a != *b)))
            return fail("The target verdict change does not match the two verdicts.");
        verdicts = GhostVerdicts{*a, *b};
    }
    std::optional<GhostCell> cell;
    if (data.contains("asset") || data.contains("cell")) {
        if (marker.is_null() || !data.contains("asset") || !data["asset"].is_string() ||
            !data.contains("cell") || !data["cell"].is_object())
            return fail("Invalid changed-cell evidence.");
        const auto name = data["asset"].get<std::string>();
        if (name.rfind("Content/", 0) != 0 || name.size() > 512 || name.find('\\') != std::string::npos ||
            name.find(':') != std::string::npos || name.find('\0') != std::string::npos)
            return fail("Invalid tilemap asset path in the evidence.");
        const auto asset = std::filesystem::path(std::u8string(name.begin(), name.end()));
        if (asset.lexically_normal() != asset || asset.extension() != ".atilemap")
            return fail("Invalid tilemap asset path in the evidence.");
        const auto& fields = data["cell"];
        const auto integer = [&](const char* key, i32& value) {
            const auto it = fields.find(key);
            if (it == fields.end() || !it->is_number_integer()) return false;
            const auto n = it->get<f64>();
            if (n < std::numeric_limits<i32>::min() || n > std::numeric_limits<i32>::max()) return false;
            value = static_cast<i32>(n);
            return true;
        };
        GhostCell record{asset, 0, 0, 0, 0};
        i32 original = 0;
        if (!integer("layer", record.layer) || !integer("x", record.x) || !integer("y", record.y) ||
            !integer("before", original) || !integer("after", record.edited_value) ||
            record.layer < 0 || record.x < 0 || record.y < 0 || original == record.edited_value)
            return fail("Invalid changed-cell coordinates or values.");
        cell = std::move(record);
    }
    ghost_cell_ = std::move(cell);
    ghost_verdicts_ = verdicts;
    ghost_before_ = std::move(before);
    ghost_after_ = std::move(after);
    ghost_difference_ = marker.is_null() ? 0 : static_cast<usize>(marker.get<u64>());
    ghost_map_ = doc_.TilemapFile();
    if (error) error->clear();
    return true;
}

bool TilemapPanel::JumpProofCellMatchesMap() const {
    if (!ghost_cell_ || doc_.TilemapFile().empty()) return false;
    const auto& cell = *ghost_cell_;
    const auto map_path = doc_.TilemapFile().lexically_normal();
    auto full = map_path.end(), suffix = cell.asset.end();
    while (suffix != cell.asset.begin()) {
        if (full == map_path.begin() || *--full != *--suffix) return false;
    }
    const auto& map = doc_.Map();
    const auto layer = static_cast<usize>(cell.layer);
    return layer < map.layers.size() && map.InBounds(cell.x, cell.y) &&
           map.Get(layer, cell.x, cell.y) == cell.edited_value;
}

void TilemapPanel::DrawJumpProofTools() {
    if (!ImGui::CollapsingHeader("Jump Proof overlay")) return;
    ImGui::InputTextWithHint("##ghost_file", "Path to Jump Proof overlay JSON", &ghost_file_);
    if (ImGui::SmallButton("Load overlay")) LoadJumpProofOverlay(ghost_file_, &ghost_error_);
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear overlay")) {
        ghost_before_.clear();
        ghost_after_.clear();
        ghost_difference_ = 0;
        ghost_cell_.reset();
        ghost_verdicts_.reset();
        ghost_error_.clear();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::SliderFloat("Pixels per unit", &ghost_pixels_per_unit_, 1.0f, 256.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
    if (!ghost_error_.empty()) ImGui::TextWrapped("%s", ghost_error_.c_str());
    if (!ghost_before_.empty()) {
        ImGui::TextColored(ImVec4(.30f, .80f, 1.0f, 1.0f), "Blue: before");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, .63f, .29f, 1.0f), "Orange: after");
        if (ghost_difference_ != 0) {
            ImGui::SameLine();
            ImGui::Text("First difference: frame %zu", ghost_difference_);
        }
        if (ghost_verdicts_) {
            const auto [before, after] = *ghost_verdicts_;
            ImGui::Text("Target result: %s -> %s%s", before ? "pass" : "missed", after ? "pass" : "missed",
                        before == after ? "" : " (changed)");
        }
        if (JumpProofCellMatchesMap())
            ImGui::Text("Green: reported changed cell (%d, %d), layer %d", ghost_cell_->x, ghost_cell_->y, ghost_cell_->layer);
        else if (ghost_cell_)
            ImGui::TextWrapped("The evidence cell does not match this edited map.");
    }
}

void TilemapPanel::LoadTexturePreview(const assets::AssetDatabase& database) {
    pending_sheet_ = {};
    show_sheet_ = false;
    const Tileset& set = doc_.GetTileset();
    const auto* record = database.Find(set.texture);
    if (!record || record->missing || record->importer != "Texture") {
        sheet_status_ = "Tileset texture unavailable. Showing numbered swatches.";
        return;
    }
    constexpr u64 kMaxPreviewPixels = 16 * 1024 * 1024;
    if (!assets::DecodeImageFile(database.SourcePath(record->guid).string(), pending_sheet_, kMaxPreviewPixels)) {
        sheet_status_ = "Cannot preview " + record->path + ": image unreadable or too large.";
    } else if (pending_sheet_.width != set.texture_width || pending_sheet_.height != set.texture_height) {
        sheet_status_ = "Tileset pixel dimensions do not match " + record->path + ". Showing swatches.";
        pending_sheet_ = {};
    } else {
        sheet_status_.clear();
    }
}

bool TilemapPanel::DrawTileArt(ImDrawList* draw, ImVec2 a, ImVec2 b, i32 tile) const {
    f32 uv[4];
    if (!show_sheet_ || !doc_.GetTileset().Uv(tile, uv)) return false;
    draw->AddImage(sheet_.Ref(), a, b, ImVec2(uv[0], uv[1]), ImVec2(uv[2], uv[3]));
    return true;
}

void TilemapPanel::ResetView() {
    zoom = kDefaultZoom;
    pan_x_ = pan_y_ = kDefaultPan;
    pending_focus_.reset();
    zoom_before_slider_.reset();
}

void TilemapPanel::ResetForNewMap() {
    ResetView();
    selected_autotile_ = 0;
    shape_active_ = dragging_ = false;
    last_brush_sample_.reset();
}

void TilemapPanel::Draw() {
    if (!pending_sheet_.pixels.empty()) {
        show_sheet_ = sheet_.Set(pending_sheet_.width, pending_sheet_.height, pending_sheet_.pixels);
        if (!show_sheet_) sheet_status_ = "Cannot upload the tileset preview. Showing numbered swatches.";
        pending_sheet_ = {};
    }
    if (ghost_map_ != doc_.TilemapFile()) {
        ghost_before_.clear();
        ghost_after_.clear();
        ghost_difference_ = 0;
        ghost_cell_.reset();
        ghost_verdicts_.reset();
        ghost_map_ = doc_.TilemapFile();
    }
    DrawTools();
    DrawJumpProofTools();
    ImGui::Separator();
    if (ImGui::BeginChild("##left", ImVec2(260, 0), ImGuiChildFlags_Borders)) {
        DrawLayers();
        ImGui::Separator();
        DrawPalette();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    DrawCanvas();
}

bool TilemapPanel::FindNextInvalidPaint() {
    const auto& map = doc_.Map();
    const auto& tiles = doc_.GetTileset();
    const u32 count = tiles.texture.IsNull() ? 0 : tiles.TileCount();
    if (invalid_search_revision_ != doc_.Revision()) invalid_paint_cell_.reset();
    invalid_search_revision_ = doc_.Revision();
    invalid_autotile_mask_.reset();
    // Strict cooking checks every variant, even in rules not painted on this map.
    for (usize rule = 0; count != 0 && rule < tiles.autotiles.size() && !invalid_autotile_mask_; ++rule) {
        const auto& variants = tiles.autotiles[rule].tiles;
        for (usize mask = 0; mask < variants.size(); ++mask) {
            const i32 tile = variants[mask];
            if (tile >= 0 && static_cast<u32>(tile) >= count) {
                invalid_autotile_mask_ = InvalidAutotileMask{rule, mask, tile, count};
                break;
            }
        }
    }
    const auto focus_rule = [&] {
        if (invalid_autotile_mask_) selected_autotile_ = static_cast<i32>(invalid_autotile_mask_->rule);
        return false; // there is no canvas cell to focus
    };
    const auto previous = invalid_paint_cell_;
    invalid_paint_cell_.reset();
    if (map.width == 0 || map.height == 0) return focus_rule();
    // The valid encodings form one contiguous signed range: autotiles,
    // empty, and concrete tiles. A single unsigned comparison tests it.
    constexpr i32 lowest = std::numeric_limits<i32>::min();
    const auto max_rules = static_cast<usize>(static_cast<i64>(kEmpty) - lowest);
    const auto first_valid = static_cast<i32>(static_cast<i64>(kEmpty) - std::min(tiles.autotiles.size(), max_rules));
    const auto last_valid = count == 0 ? std::numeric_limits<i32>::max() : static_cast<i32>(count - 1);
    if (first_valid == lowest && last_valid == std::numeric_limits<i32>::max()) return focus_rule();
    const u32 valid_start = static_cast<u32>(first_valid);
    const u32 valid_span = static_cast<u32>(last_valid) - valid_start;
    const auto scan_layer = [&](usize layer, usize begin, usize end) {
        const auto& cells = map.layers[layer].cells;
        const auto stop = cells.begin() + static_cast<std::ptrdiff_t>(end);
        const auto it = std::find_if(cells.begin() + static_cast<std::ptrdiff_t>(begin), stop,
                                     [&](i32 tile) { return static_cast<u32>(tile) - valid_start > valid_span; });
        if (it == stop) return false;
        invalid_paint_cell_ = InvalidPaintCell{layer, static_cast<usize>(it - cells.begin()), *it};
        return true;
    };
    // Resume after the last hit. Only visit the prefix when the search wraps.
    const usize start_layer = previous ? previous->layer : 0;
    for (usize layer = start_layer; layer < map.layers.size(); ++layer) {
        const usize begin = previous && layer == start_layer ? previous->index + 1 : 0;
        if (scan_layer(layer, begin, map.layers[layer].cells.size())) break;
    }
    if (!invalid_paint_cell_ && previous) {
        for (usize layer = 0; layer <= start_layer; ++layer) {
            // Include the previous hit so a single remaining problem can be found again.
            const usize end = layer == start_layer ? previous->index + 1 : map.layers[layer].cells.size();
            if (scan_layer(layer, 0, end)) break;
        }
    }
    if (!invalid_paint_cell_) return focus_rule();
    const auto hit = *invalid_paint_cell_;
    return FocusCell(hit.layer, static_cast<i32>(hit.index % map.width), static_cast<i32>(hit.index / map.width));
}

bool TilemapPanel::FocusCell(usize layer, i32 x, i32 y) {
    const auto& map = doc_.Map();
    if (layer >= map.layers.size() || !map.InBounds(x, y)) return false;
    doc_.active_layer = layer;
    pending_focus_ = PendingFocus{x, y, doc_.Revision()};
    return true;
}

void TilemapPanel::DrawTools() {
    bool save_shortcut = false;
    const auto& io = ImGui::GetIO();
    const bool show_command = native_command_ || io.ConfigMacOSXBehaviors;
    if (!dragging_ && !shape_active_ && !io.WantTextInput && !ImGui::IsAnyItemActive() &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        const auto shortcut = [&](ImGuiKeyChord chord) {
            const bool ctrl = ImGui::Shortcut(ImGuiMod_Ctrl | chord);
            // The shell leaves ImGui's global swap off. Bind raw Command
            // (Super) locally on macOS, but not Windows/Linux Super.
            const bool cmd = native_command_ && !io.ConfigMacOSXBehaviors && ImGui::Shortcut(ImGuiMod_Super | chord);
            return ctrl || cmd;
        };
        const bool undo = shortcut(ImGuiKey_Z);
        const bool redo_shift = shortcut(ImGuiMod_Shift | ImGuiKey_Z);
        const bool redo_alt = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y);
        if (undo) doc_.Undo();
        else if (redo_shift || redo_alt) doc_.Redo();
        save_shortcut = shortcut(ImGuiKey_S);
    }
    if (!doc_.TilemapFile().empty()) {
        ImGui::BeginDisabled(!doc_.dirty());
        const bool clicked = ImGui::Button("Save tilemap and tileset");
        ImGui::SetItemTooltip("%s", show_command ? "Save (Cmd+S)" : "Save (Ctrl+S)");
        if (clicked || (save_shortcut && doc_.dirty())) {
            save_error_.clear();
            if (!doc_.Save(&save_error_) && save_error_.empty()) save_error_ = "Could not save the tilemap and tileset.";
            save_error_revision_ = doc_.Revision();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    DrawResizeDialog();
    if (doc_.TilemapFile().empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("Practice map (not saved).");
        ImGui::TextWrapped("Open a project tilemap in the Content Browser to replace it.");
    } else if (!save_error_.empty() && save_error_revision_ == doc_.Revision()) {
        ImGui::TextWrapped("%s", save_error_.c_str());
    }
    for (int t = 0; t < static_cast<int>(std::size(kToolNames)); ++t) {
        if (t > 0) ImGui::SameLine();
        const bool on = static_cast<int>(doc_.tool) == t;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(kToolNames[t])) doc_.tool = static_cast<TileTool>(t);
        if (static_cast<TileTool>(t) == TileTool::Line)
            ImGui::SetItemTooltip("Drag to preview a straight line. Release to paint it.");
        if (on) ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!doc_.CanUndo());
    if (ImGui::Button("Undo")) doc_.Undo();
    ImGui::SetItemTooltip("%s", show_command ? "Undo (Cmd+Z)" : "Undo (Ctrl+Z)");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!doc_.CanRedo());
    if (ImGui::Button("Redo")) doc_.Redo();
    ImGui::SetItemTooltip("%s", show_command ? "Redo (Cmd+Shift+Z)" : "Redo (Ctrl+Y or Ctrl+Shift+Z)");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox("Grid", &show_grid);
    ImGui::SameLine();
    ImGui::Checkbox("Solid", &show_solid);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    const f32 old_zoom = zoom;
    if (ImGui::SliderFloat("Zoom", &zoom, 6.0f, 64.0f, "%.0f px") && !zoom_before_slider_)
        zoom_before_slider_ = old_zoom;
    ImGui::SetItemTooltip("Zoom around the center of the view.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset view")) ResetView();
    ImGui::SetItemTooltip("Return to the lower-left corner at the default zoom.");
    ImGui::SameLine();
    DrawGoToCell();
    ImGui::SameLine();
    const std::string file = doc_.TilemapFile().empty() ? std::string() : doc_.TilemapFile().filename().string() + "  ";
    ImGui::TextDisabled("%s%ux%u%s", file.c_str(), doc_.Map().width, doc_.Map().height, doc_.dirty() ? "  (unsaved)" : "");
    ImGui::SameLine();
    if (ImGui::SmallButton("Find next invalid tile")) FindNextInvalidPaint();
    if (doc_.HasMissingAutotileBrush())
        ImGui::TextWrapped("This autotile rule no longer exists. Pick another brush to paint.");
    else if (doc_.HasOutOfGridBrush())
        ImGui::TextWrapped("Tile %d is outside the current tileset. Pick a tile from the palette to paint.", doc_.brush);
    if (invalid_search_revision_ == doc_.Revision()) {
        if (invalid_paint_cell_) {
            const auto& hit = *invalid_paint_cell_;
            const auto& map = doc_.Map();
            const auto* name = map.layers[hit.layer].name.c_str();
            const usize x = hit.index % map.width, y = hit.index / map.width;
            if (IsAutotileCell(hit.tile))
                ImGui::Text("Invalid paint: %s (%zu, %zu), missing autotile rule %zu", name, x, y, AutotileIndex(hit.tile));
            else
                ImGui::Text("Invalid paint: %s (%zu, %zu), tile %d", name, x, y, hit.tile);
        } else if (!invalid_autotile_mask_) {
            ImGui::TextDisabled("No invalid painted tiles or autotile masks.");
        }
        if (invalid_autotile_mask_) {
            const auto& hit = *invalid_autotile_mask_;
            const auto& name = doc_.GetTileset().autotiles[hit.rule].name;
            ImGui::TextWrapped("Invalid autotile: %s, mask %zu uses tile %d (grid has %u tiles).",
                               name.c_str(), hit.mask, hit.tile, hit.grid_count);
        }
    }
}

void TilemapPanel::DrawGoToCell() {
    const auto& map = doc_.Map();
    ImGui::BeginDisabled(map.width == 0 || map.height == 0 || doc_.active_layer >= map.layers.size());
    if (ImGui::SmallButton("Go to cell...")) {
        jump_x_ = jump_y_ = 0;
        ImGui::OpenPopup("Go to tile cell");
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Center the view on exact tile coordinates without editing the map.");
    if (!ImGui::BeginPopupModal("Go to tile cell", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::Text("Map size: %u x %u. Coordinates start at (0, 0) in the lower-left.", map.width, map.height);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("X", &jump_x_, 0);
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("Y", &jump_y_, 0);
    const bool allowed = map.InBounds(jump_x_, jump_y_) && doc_.active_layer < map.layers.size();
    if (!allowed) ImGui::TextWrapped("Enter coordinates inside the map.");
    ImGui::BeginDisabled(!allowed);
    if (ImGui::Button("Go") && FocusCell(doc_.active_layer, jump_x_, jump_y_)) ImGui::CloseCurrentPopup();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void TilemapPanel::DrawResizeDialog() {
    const auto& map = doc_.Map();
    if (ImGui::Button("Resize map...")) {
        resize_width_ = static_cast<int>(std::min<u32>(map.width, std::numeric_limits<int>::max()));
        resize_height_ = static_cast<int>(std::min<u32>(map.height, std::numeric_limits<int>::max()));
        ImGui::OpenPopup("Resize tilemap");
    }
    if (!ImGui::BeginPopupModal("Resize tilemap", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::Text("Current size: %u x %u cells", map.width, map.height);
    ImGui::SetNextItemWidth(140);
    ImGui::InputInt("Width", &resize_width_);
    ImGui::SetNextItemWidth(140);
    ImGui::InputInt("Height", &resize_height_);
    const bool positive = resize_width_ > 0 && resize_height_ > 0;
    const u32 width = positive ? static_cast<u32>(resize_width_) : 0;
    const u32 height = positive ? static_cast<u32>(resize_height_) : 0;
    const bool allowed = doc_.CanResize(width, height);
    if (!positive || ((width != map.width || height != map.height) && !allowed))
        ImGui::TextWrapped("Enter positive dimensions. Above 4,194,304 cells, only cropping is allowed.");
    else if (width < map.width || height < map.height)
        ImGui::TextUnformatted("Cells beyond the new top and right edges will be removed. Undo restores them.");
    else
        ImGui::TextDisabled("New cells are empty. Existing cells keep their positions.");
    ImGui::BeginDisabled(!allowed);
    if (ImGui::Button("Resize") && doc_.Resize(width, height)) ImGui::CloseCurrentPopup();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void TilemapPanel::DrawLayers() {
    ImGui::SeparatorText("Layers");
    ImGui::Checkbox("Solo layer art", &solo_layer_art);
    ImGui::SetItemTooltip("Show only the selected layer's art. Collision and invalid tile markers still cover the whole map.");
    const TilemapData& map = doc_.Map();
    for (usize i = map.layers.size(); i-- > 0;) { // the top layer first
        ImGui::PushID(static_cast<int>(i));
        bool collides = map.layers[i].collides;
        if (ImGui::Checkbox("##collides", &collides)) doc_.SetLayerCollides(i, collides);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The layer's solid tiles block");
        ImGui::SameLine();
        const ImVec2 name_position = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##layer", doc_.active_layer == i, 0, ImVec2(0, ImGui::GetTextLineHeight())))
            doc_.active_layer = i;
        // Draw the label literally; a name such as "Ground ## 2" is not an ImGui ID.
        ImGui::GetWindowDrawList()->AddText(name_position, ImGui::GetColorU32(ImGuiCol_Text), map.layers[i].name.c_str());
        ImGui::PopID();
    }
    if (ImGui::SmallButton("Add layer")) doc_.AddLayer("Layer " + std::to_string(map.layers.size() + 1));
    ImGui::SameLine();
    if (ImGui::SmallButton("Remove")) doc_.RemoveLayer(doc_.active_layer);
    ImGui::SameLine();
    if (ImGui::SmallButton("Duplicate")) doc_.DuplicateLayer(doc_.active_layer);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy the selected layer and its collision setting");
    if (doc_.active_layer < map.layers.size()) {
        const auto active = doc_.active_layer;
        ImGui::BeginDisabled(active + 1 >= map.layers.size());
        if (ImGui::SmallButton("Move up")) doc_.MoveLayer(active, active + 1);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(active == 0);
        if (ImGui::SmallButton("Move down")) doc_.MoveLayer(active, active - 1);
        ImGui::EndDisabled();
        ImGui::PushID(static_cast<int>(doc_.active_layer));
        std::string name = map.layers[doc_.active_layer].name;
        ImGui::SetNextItemWidth(138.0f);
        if (ImGui::InputText("Layer name", &name, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
            doc_.RenameLayer(doc_.active_layer, name);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enter saves a nonblank name. Escape cancels.");
        ImGui::PopID();
    }
}

void TilemapPanel::DrawPalette() {
    const Tileset& ts = doc_.GetTileset();
    const auto pick_brush = [&](i32 tile) {
        doc_.brush = tile;
        if (doc_.tool == TileTool::Erase) doc_.tool = TileTool::Paint;
    };
    ImGui::SeparatorText("Tiles");
    if (!sheet_status_.empty()) ImGui::TextWrapped("%s", sheet_status_.c_str());
    const f32 cell = 28.0f;
    const u32 available = static_cast<u32>(std::max(1.0f, (ImGui::GetContentRegionAvail().x + 2.0f) / (cell + 2.0f)));
    const u32 columns = std::clamp(ts.Columns(), 1u, available);
    const u32 count = ts.TileCount();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const u64 row_count = (static_cast<u64>(count) + columns - 1) / columns;
    const int rows = static_cast<int>(std::min<u64>(row_count, std::numeric_limits<int>::max() - 1));
    ImGuiListClipper clipper;
    clipper.Begin(rows, cell + ImGui::GetStyle().ItemSpacing.y);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const u32 first = static_cast<u32>(row) * columns;
            const u32 last = std::min(first + columns, count);
            for (u32 i = first; i < last; ++i) {
                if (i != first) ImGui::SameLine(0.0f, 2.0f);
                ImGui::PushID(static_cast<int>(i));
                const ImVec2 p = ImGui::GetCursorScreenPos();
                if (ImGui::InvisibleButton("##tile", ImVec2(cell, cell))) pick_brush(static_cast<i32>(i));
                const ImVec2 end(p.x + cell, p.y + cell);
                dl->AddRectFilled(p, end, IM_COL32(30, 30, 36, 255));
                if (!DrawTileArt(dl, p, end, static_cast<i32>(i))) {
                    dl->AddRectFilled(p, end, TileColor(static_cast<i32>(i)));
                    char label[16];
                    std::snprintf(label, sizeof(label), "%u", i);
                    dl->AddText(ImVec2(p.x + 3, p.y + 2), IM_COL32(0, 0, 0, 255), label);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tile %u", i);
                if (ts.IsSolid(static_cast<i32>(i))) dl->AddRect(p, ImVec2(p.x + cell, p.y + cell), IM_COL32(220, 40, 40, 255), 0, 0, 2.0f);
                if (doc_.brush == static_cast<i32>(i)) dl->AddRect(p, ImVec2(p.x + cell, p.y + cell), IM_COL32(255, 255, 255, 255), 0, 0, 2.0f);
                ImGui::PopID();
            }
        }
    }
    const bool outside_grid = doc_.brush >= 0 && !ts.texture.IsNull() && count != 0 &&
                              static_cast<u32>(doc_.brush) >= count;
    if (doc_.brush >= 0) {
        bool solid = ts.IsSolid(doc_.brush);
        ImGui::BeginDisabled(outside_grid && !solid);
        if (ImGui::Checkbox("Tile blocks movement", &solid)) doc_.SetSolid(doc_.brush, solid);
        ImGui::EndDisabled();
        if (outside_grid && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(solid ? "Clear this collision flag: the tile is outside the current tileset."
                                    : "Pick a tile from the palette to set collision.");
    }

    ImGui::SeparatorText("Autotiles");
    for (usize i = 0; i < ts.autotiles.size(); ++i) {
        ImGui::PushID(static_cast<int>(1000 + i));
        const bool active = doc_.brush == AutotileCell(i);
        const char* name = ts.autotiles[i].name.c_str();
        const ImVec2 name_position = ImGui::GetCursorScreenPos();
        const f32 row_height = std::max(ImGui::GetTextLineHeight(), ImGui::CalcTextSize(name).y);
        if (ImGui::Selectable("##autotile", active, 0, ImVec2(0.0f, row_height))) {
            pick_brush(AutotileCell(i));
            selected_autotile_ = static_cast<int>(i);
        }
        // Authored names are display text, not ImGui labels (which interpret ##).
        if (ImGui::IsItemVisible()) dl->AddText(name_position, ImGui::GetColorU32(ImGuiCol_Text), name);
        ImGui::PopID();
    }
    if (ImGui::SmallButton("Add autotile")) {
        const usize n = doc_.AddAutotile("Autotile " + std::to_string(ts.autotiles.size() + 1));
        selected_autotile_ = static_cast<int>(n);
        pick_brush(AutotileCell(n));
    }
    if (selected_autotile_ >= 0 && static_cast<usize>(selected_autotile_) < ts.autotiles.size() && doc_.brush >= 0) {
        // Assigns the brush tile to a neighbour mask of the selected autotile.
        const Autotile& at = ts.autotiles[static_cast<usize>(selected_autotile_)];
        ImGui::TextWrapped("Editing masks: %s", at.name.c_str());
        const bool can_fill = static_cast<u32>(doc_.brush) < count &&
                              std::find(at.tiles.begin(), at.tiles.end(), kEmpty) != at.tiles.end();
        ImGui::BeginDisabled(!can_fill);
        if (ImGui::SmallButton("Fill empty masks"))
            doc_.FillEmptyAutotileMasks(static_cast<usize>(selected_autotile_), doc_.brush);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Use tile %d for every empty mask. Assigned tiles stay as they are.", doc_.brush);
        ImGui::TextDisabled("Or click one mask to assign it:");
        ImGui::BeginDisabled(outside_grid);
        for (int m = 0; m < 16; ++m) {
            ImGui::PushID(2000 + m);
            if (m % 4 != 0) ImGui::SameLine();
            char label[24];
            std::snprintf(label, sizeof(label), "%c%c%c%c %d", (m & kNorth) ? 'N' : '-', (m & kEast) ? 'E' : '-',
                          (m & kSouth) ? 'S' : '-', (m & kWest) ? 'W' : '-', at.tiles[static_cast<usize>(m)]);
            if (ImGui::SmallButton(label)) {
                doc_.SetAutotileTile(static_cast<usize>(selected_autotile_), static_cast<u8>(m), doc_.brush);
            }
            ImGui::PopID();
        }
        ImGui::EndDisabled();
        if (outside_grid)
            ImGui::TextWrapped("Tile %d is outside this tileset. Pick a tile from the palette to assign a mask.", doc_.brush);
    }
}

void TilemapPanel::DrawCanvas() {
    if (!ImGui::BeginChild("##canvas", ImVec2(0, 0), ImGuiChildFlags_Borders,
                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::EndChild();
        return;
    }
    const TilemapData& map = doc_.Map();
    const Tileset& ts = doc_.GetTileset();
    const u32 grid_tiles = ts.texture.IsNull() ? 0 : ts.TileCount();
    const auto invalid_paint = [&](i32 tile) { return IsInvalidPaint(tile, grid_tiles, ts.autotiles.size()); };
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##paint", ImVec2(std::max(size.x, 1.0f), std::max(size.y, 1.0f)),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();
    // Pan is measured from the canvas's bottom-left, with y pointing up.
    const auto reanchor_zoom = [&](f32 from, f32 to, f64 x, f64 y) {
        if (!std::isfinite(from) || !std::isfinite(to) || from <= 0.0f || from == to) return;
        const f64 correction = 1.0 - static_cast<f64>(to) / from;
        pan_x_ += (x - pan_x_) * correction;
        pan_y_ += (y - pan_y_) * correction;
    };
    if (zoom_before_slider_) {
        reanchor_zoom(*zoom_before_slider_, zoom, size.x * 0.5, size.y * 0.5);
        zoom_before_slider_.reset();
    }
    if (hovered && std::isfinite(io.MouseWheel) && io.MouseWheel != 0.0f) {
        const f32 next = std::clamp(zoom * std::pow(1.15f, io.MouseWheel), 6.0f, 64.0f);
        reanchor_zoom(zoom, next, static_cast<f64>(io.MousePos.x) - origin.x,
                      static_cast<f64>(origin.y) + size.y - io.MousePos.y);
        zoom = next;
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
        pan_x_ += io.MouseDelta.x;
        pan_y_ += io.MouseDelta.y;
    }

    if (pending_focus_) {
        const auto focus = *pending_focus_;
        pending_focus_.reset();
        if (focus.revision == doc_.Revision()) {
            pan_x_ = size.x * 0.5 - (static_cast<f64>(focus.x) + 0.5) * zoom;
            pan_y_ = size.y * 0.5 - (static_cast<f64>(focus.y) + 0.5) * zoom;
        }
    }

    // Use double until the last screen-space conversion: on very long maps
    // the map origin and cell offset can be hundreds of millions of pixels
    // apart even when the resulting point is inside the canvas.
    const f64 base_x = origin.x + pan_x_;
    const f64 base_y = static_cast<f64>(origin.y) + size.y - pan_y_;
    const auto edge_x = [&](i32 x) { return base_x + static_cast<f64>(x) * zoom; };
    const auto edge_y = [&](i32 y) { return base_y - static_cast<f64>(y) * zoom; };
    const auto to_screen_x = [&](i32 x) { return static_cast<f32>(edge_x(x)); };
    const auto to_screen_y = [&](i32 y) { return static_cast<f32>(edge_y(y + 1)); }; // top edge

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    // Intersect in screen space first so even an enormous map only visits
    // the cells under the visible part of the canvas (including partial cells).
    const ImVec2 clip_min = dl->GetClipRectMin(), clip_max = dl->GetClipRectMax();
    const f64 x0 = std::max(static_cast<f64>(clip_min.x), base_x);
    const f64 y0 = std::max(static_cast<f64>(clip_min.y), edge_y(static_cast<i32>(map.height)));
    const f64 x1 = std::min(static_cast<f64>(clip_max.x), edge_x(static_cast<i32>(map.width)));
    const f64 y1 = std::min(static_cast<f64>(clip_max.y), base_y);
    if (x0 < x1 && y0 < y1) {
        const auto index = [](f64 value, u32 count) {
            return static_cast<i32>(std::clamp(value, 0.0, static_cast<f64>(count)));
        };
        const i32 left = index(std::floor((x0 - base_x) / zoom), map.width);
        const i32 right = index(std::ceil((x1 - base_x) / zoom), map.width);
        const i32 bottom = index(std::floor((base_y - y1) / zoom), map.height);
        const i32 top = index(std::ceil((base_y - y0) / zoom), map.height);
        const ImVec2 visible_min(static_cast<f32>(x0), static_cast<f32>(y0));
        const ImVec2 visible_max(static_cast<f32>(x1), static_cast<f32>(y1));
        dl->AddRectFilled(visible_min, visible_max, IM_COL32(30, 30, 36, 255));
        const auto columns = static_cast<usize>(right - left);
        const auto visible_cell = [&](i32 x, i32 y) {
            return static_cast<usize>(y - bottom) * columns + static_cast<usize>(x - left);
        };
        constexpr u8 kBlocked = 1, kInvalidPaint = 2;
        const usize visible_count = columns * static_cast<usize>(top - bottom);
        std::vector<u8> marks(show_solid || grid_tiles != 0 ? visible_count : 0);
        for (usize layer = 0; layer < map.layers.size(); ++layer) {
            const bool draw_art = !solo_layer_art || layer == doc_.active_layer;
            const bool mark_solid = show_solid && map.layers[layer].collides;
            const auto& stored = map.layers[layer].cells;
            for (i32 y = bottom; y < top; ++y) {
                const u64 start = static_cast<u64>(y) * map.width + static_cast<u32>(left);
                if (start >= stored.size()) break;
                const usize count = std::min(columns, stored.size() - static_cast<usize>(start));
                const i32* row = stored.data() + static_cast<usize>(start);
                for (usize offset = 0; offset < count; ++offset) {
                    const i32 x = left + static_cast<i32>(offset);
                    const i32 raw = row[offset];
                    if (raw == kEmpty) continue;
                    if (invalid_paint(raw)) {
                        if (marks.empty()) marks.resize(visible_count);
                        marks[visible_cell(x, y)] |= kInvalidPaint;
                    }
                    if (!draw_art && !mark_solid) continue;
                    const i32 tile = raw >= kEmpty ? raw : ResolveTile(map, ts, layer, x, y);
                    if (tile < 0) continue;
                    if (draw_art) {
                        const ImVec2 a(to_screen_x(x), to_screen_y(y));
                        const ImVec2 b(a.x + zoom, a.y + zoom);
                        if (!DrawTileArt(dl, a, b, tile)) dl->AddRectFilled(a, b, TileColor(tile));
                    }
                    if (mark_solid && ts.IsSolid(tile)) marks[visible_cell(x, y)] |= kBlocked;
                }
            }
        }
        // Draw map diagnostics once per visible cell, after all tile art.
        if (!marks.empty()) {
            for (i32 y = bottom; y < top; ++y) {
                for (i32 x = left; x < right; ++x) {
                    const u8 flags = marks[visible_cell(x, y)];
                    if (flags == 0) continue;
                    const ImVec2 a(to_screen_x(x), to_screen_y(y));
                    const ImVec2 b(a.x + zoom, a.y + zoom);
                    if (flags & kBlocked) dl->AddRect(a, b, IM_COL32(220, 40, 40, 255), 0, 0, 1.5f);
                    if (flags & kInvalidPaint) {
                        constexpr ImU32 invalid_color = IM_COL32(255, 96, 215, 255);
                        const f32 inset = zoom * 0.23f;
                        dl->AddLine(ImVec2(a.x + inset, a.y + inset), ImVec2(b.x - inset, b.y - inset), invalid_color, 2.0f);
                        dl->AddLine(ImVec2(a.x + inset, b.y - inset), ImVec2(b.x - inset, a.y + inset), invalid_color, 2.0f);
                    }
                }
            }
        }
        if (show_grid && zoom >= 10.0f) {
            constexpr ImU32 grid_color = IM_COL32(255, 255, 255, 30);
            for (i32 x = left; x <= right; ++x) {
                const f32 sx = to_screen_x(x);
                dl->AddLine(ImVec2(sx, visible_min.y), ImVec2(sx, visible_max.y), grid_color);
            }
            for (i32 y = bottom; y <= top; ++y) {
                const f32 sy = static_cast<f32>(edge_y(y));
                dl->AddLine(ImVec2(visible_min.x, sy), ImVec2(visible_max.x, sy), grid_color);
            }
        }
    }

    if (!ghost_before_.empty()) {
        const auto screen = [&](const GhostPoint& p) {
            const f64 scale = static_cast<f64>(zoom) * ghost_pixels_per_unit_;
            return ImVec2(static_cast<f32>(base_x + p.x * scale / std::max(ts.tile_width, 1u)),
                          static_cast<f32>(base_y - p.y * scale / std::max(ts.tile_height, 1u)));
        };
        const auto path = [&](const std::vector<GhostPoint>& points, ImU32 color) {
            const usize stride = std::max<usize>(1, (points.size() + 2047) / 2048);
            for (usize i = stride; i < points.size(); i += stride)
                dl->AddLine(screen(points[i - stride]), screen(points[i]), color, 2.0f);
            const usize last_drawn = ((points.size() - 1) / stride) * stride;
            if (last_drawn + 1 < points.size())
                dl->AddLine(screen(points[last_drawn]), screen(points.back()), color, 2.0f);
            dl->AddCircleFilled(screen(points.back()), 3.0f, color);
        };
        constexpr ImU32 before = IM_COL32(75, 204, 255, 255), after = IM_COL32(255, 160, 74, 255);
        path(ghost_before_, before);
        path(ghost_after_, after);
        if (ghost_difference_ != 0) {
            dl->AddCircle(screen(ghost_before_[ghost_difference_ - 1]), 5.0f, before, 16, 2.0f);
            dl->AddCircle(screen(ghost_after_[ghost_difference_ - 1]), 8.0f, after, 16, 2.0f);
        }
    }

    if (JumpProofCellMatchesMap()) {
        const auto& cell = *ghost_cell_;
        const ImVec2 a(to_screen_x(cell.x), to_screen_y(cell.y));
        const ImVec2 b(a.x + zoom, a.y + zoom);
        dl->AddRect(a, b, IM_COL32(138, 255, 133, 255), 0.0f, 2.0f);
    }

    // No mouse position is reported when the host loses focus.
    const bool mouse_valid = !io.AppFocusLost && ImGui::IsMousePosValid() &&
                             std::isfinite(base_x) && std::isfinite(base_y) && std::isfinite(zoom) && zoom > 0.0f;
    const auto to_cell = [&](f64 local) {
        return static_cast<i32>(std::clamp(std::floor(local / zoom),
                                          static_cast<f64>(std::numeric_limits<i32>::min()),
                                          static_cast<f64>(std::numeric_limits<i32>::max())));
    };
    const i32 cx = mouse_valid ? to_cell(io.MousePos.x - base_x) : 0;
    const i32 cy = mouse_valid ? to_cell(base_y - io.MousePos.y) : 0;
    const bool on_map = mouse_valid && hovered && map.InBounds(cx, cy);
    const bool left = ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::IsItemActive();
    const bool right = ImGui::IsMouseDown(ImGuiMouseButton_Right) && ImGui::IsItemActive();
    const TileTool selected = doc_.tool;
    const bool line_tool = selected == TileTool::Line;
    const bool shape_tool = selected == TileTool::Rectangle || line_tool;
    if (shape_tool) {
        if (left && !right && !dragging_ && on_map && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            shape_active_ = true;
            shape_x_ = cx;
            shape_y_ = cy;
        }
        const bool cancel = !mouse_valid || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                            !map.InBounds(shape_x_, shape_y_);
        if (shape_active_ && (cancel || !left)) {
            if (!cancel && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                if (line_tool) doc_.Line(shape_x_, shape_y_, std::clamp(cx, 0, static_cast<i32>(map.width) - 1),
                                         std::clamp(cy, 0, static_cast<i32>(map.height) - 1));
                else doc_.Rectangle(shape_x_, shape_y_, cx, cy);
            }
            shape_active_ = false;
        }
        if (shape_active_) {
            const i32 x = std::clamp(cx, 0, static_cast<i32>(map.width) - 1);
            const i32 y = std::clamp(cy, 0, static_cast<i32>(map.height) - 1);
            if (line_tool) {
                const auto center = [&](i32 tile_x, i32 tile_y) {
                    return ImVec2(to_screen_x(tile_x) + zoom * 0.5f, to_screen_y(tile_y) + zoom * 0.5f);
                };
                dl->AddLine(center(shape_x_, shape_y_), center(x, y), IM_COL32(130, 214, 255, 255), 2.0f);
                if (hovered) ImGui::SetTooltip("Release to paint the line. Escape cancels.");
            } else {
                const ImVec2 a(to_screen_x(std::min(x, shape_x_)), to_screen_y(std::max(y, shape_y_)));
                const ImVec2 b(to_screen_x(std::max(x, shape_x_) + 1), to_screen_y(std::min(y, shape_y_)) + zoom);
                dl->AddRectFilled(a, b, IM_COL32(130, 214, 255, 48));
                dl->AddRect(a, b, IM_COL32(130, 214, 255, 255), 0.0f, 0, 2.0f);
                if (hovered) ImGui::SetTooltip("Release to fill the rectangle. Escape cancels.");
            }
        }
    } else {
        shape_active_ = false;
    }
    if (on_map && !shape_active_) {
        const ImVec2 a(to_screen_x(cx), to_screen_y(cy));
        dl->AddRect(a, ImVec2(a.x + zoom, a.y + zoom), IM_COL32(255, 255, 255, 200), 0.0f, 0, 2.0f);
        if (!left && !right && ImGui::BeginTooltip()) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 260.0f);
            ImGui::Text("Cell (%d, %d)", cx, cy);
            if (doc_.active_layer < map.layers.size()) {
                ImGui::Text("Editing: %s", map.layers[doc_.active_layer].name.c_str());
                const i32 tile = ResolveTile(map, ts, doc_.active_layer, cx, cy);
                if (tile >= 0) ImGui::Text("Tile on this layer: %d", tile);
                else ImGui::TextUnformatted("No visible tile on this layer");
            }
            const TileLayer* top_blocker = nullptr;
            usize blockers = 0, invalid_layers = 0;
            for (usize layer = map.layers.size(); layer-- > 0;) {
                const i32 raw = map.Get(layer, cx, cy);
                if (invalid_paint(raw) && invalid_layers++ == 0) {
                    const auto* name = map.layers[layer].name.c_str();
                    if (IsAutotileCell(raw))
                        ImGui::Text("Invalid paint: %s uses missing autotile rule %zu", name, AutotileIndex(raw));
                    else
                        ImGui::Text("Invalid paint: %s uses tile %d (grid has %u tiles)", name, raw, grid_tiles);
                }
                if (map.layers[layer].collides && ts.IsSolid(ResolveTile(map, ts, layer, cx, cy))) {
                    if (!top_blocker) top_blocker = &map.layers[layer];
                    ++blockers;
                }
            }
            if (invalid_layers > 1)
                ImGui::Text("Plus %zu more invalid %s", invalid_layers - 1, invalid_layers == 2 ? "layer" : "layers");
            if (top_blocker) {
                ImGui::Text("Blocks movement: %s", top_blocker->name.c_str());
                if (blockers > 1)
                    ImGui::Text("Plus %zu more blocking %s", blockers - 1, blockers == 2 ? "layer" : "layers");
            } else {
                ImGui::TextUnformatted("No blocking tiles");
            }
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
    dl->PopClipRect();

    // In shape modes a fresh right press starts a freehand eraser, but
    // never takes over a left drag or starts late after its release.
    const bool shape_erase_drag = right && !shape_active_ &&
        (dragging_ || (!left && ImGui::IsMouseClicked(ImGuiMouseButton_Right)));
    // A press on blank canvas must not start painting when the pointer later enters the map.
    const bool brush_start = on_map && ImGui::IsMouseClicked(right ? ImGuiMouseButton_Right : ImGuiMouseButton_Left);
    const bool brush_drag = mouse_valid && (dragging_ || brush_start) &&
        (shape_tool ? shape_erase_drag :
         right || (left && (selected == TileTool::Paint || selected == TileTool::Erase)));
    if (brush_drag) {
        if (right) doc_.tool = TileTool::Erase;
        if (!dragging_) {
            doc_.BeginStroke();
            dragging_ = true;
        }
        if (on_map) {
            if (last_brush_sample_ && last_brush_sample_->tool == doc_.tool) {
                doc_.Line(last_brush_sample_->x, last_brush_sample_->y, cx, cy);
            } else {
                doc_.Apply(cx, cy);
            }
            last_brush_sample_ = BrushSample{cx, cy, doc_.tool};
        } else {
            // Do not connect across another panel or a trip outside the map.
            last_brush_sample_.reset();
        }
        doc_.tool = selected;
    } else {
        if (dragging_) {
            doc_.EndStroke();
            last_brush_sample_.reset();
            dragging_ = false;
        }
        // Fill and Pick are one-shot tools. Crossing into the map or another
        // region after a press must not choose a new tile.
        if (on_map && left && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            (selected == TileTool::Fill || selected == TileTool::Pick))
            doc_.Apply(cx, cy);
    }
    ImGui::EndChild();
}

} // namespace aether::editor
