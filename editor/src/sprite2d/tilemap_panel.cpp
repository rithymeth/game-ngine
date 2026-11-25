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

const char* kToolNames[] = {"Paint", "Erase", "Rectangle", "Fill", "Pick"};

// Both endpoints are valid map cells. Fill the gaps between pointer samples
// with an integer line, so one fast drag remains one undoable stroke.
void ApplyBrushLine(TilemapEditDocument& doc, i32 x, i32 y, i32 end_x, i32 end_y) {
    const i32 dx = std::abs(end_x - x), dy = -std::abs(end_y - y);
    const i32 step_x = x < end_x ? 1 : -1, step_y = y < end_y ? 1 : -1;
    i32 error = dx + dy;
    for (;;) {
        doc.Apply(x, y);
        if (x == end_x && y == end_y) return;
        const i32 twice = 2 * error;
        if (twice >= dy) { error += dy; x += step_x; }
        if (twice <= dx) { error += dx; y += step_y; }
    }
}

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

void TilemapPanel::DrawTools() {
    if (doc_.TilemapFile().empty()) {
        ImGui::TextDisabled("Practice map (not saved).");
        ImGui::TextWrapped("Open a project tilemap in the Content Browser to replace it.");
    } else {
        ImGui::BeginDisabled(!doc_.dirty());
        if (ImGui::Button("Save tilemap and tileset")) {
            save_error_.clear();
            if (!doc_.Save(&save_error_) && save_error_.empty()) save_error_ = "Could not save the tilemap and tileset.";
            save_error_revision_ = doc_.Revision();
        }
        ImGui::EndDisabled();
        if (!save_error_.empty() && save_error_revision_ == doc_.Revision())
            ImGui::TextWrapped("%s", save_error_.c_str());
    }
    for (int t = 0; t < 5; ++t) {
        if (t > 0) ImGui::SameLine();
        const bool on = static_cast<int>(doc_.tool) == t;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(kToolNames[t])) doc_.tool = static_cast<TileTool>(t);
        if (on) ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!doc_.CanUndo());
    if (ImGui::Button("Undo")) doc_.Undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!doc_.CanRedo());
    if (ImGui::Button("Redo")) doc_.Redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox("Grid", &show_grid);
    ImGui::SameLine();
    ImGui::Checkbox("Solid", &show_solid);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    ImGui::SliderFloat("Zoom", &zoom, 6.0f, 64.0f, "%.0f px");
    ImGui::SameLine();
    const std::string file = doc_.TilemapFile().empty() ? std::string() : doc_.TilemapFile().filename().string() + "  ";
    ImGui::TextDisabled("%s%ux%u%s", file.c_str(), doc_.Map().width, doc_.Map().height, doc_.dirty() ? "  (unsaved)" : "");
}

void TilemapPanel::DrawLayers() {
    ImGui::SeparatorText("Layers");
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
    ImGui::SeparatorText("Tiles");
    if (!sheet_status_.empty()) ImGui::TextWrapped("%s", sheet_status_.c_str());
    const f32 cell = 28.0f;
    const u32 available = static_cast<u32>(std::max(1.0f, (ImGui::GetContentRegionAvail().x + 2.0f) / (cell + 2.0f)));
    const u32 columns = std::clamp(ts.Columns(), 1u, available);
    const u32 count = ts.TileCount();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (u32 i = 0; i < count; ++i) {
        if (i % columns != 0) ImGui::SameLine(0.0f, 2.0f);
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##tile", ImVec2(cell, cell))) {
            doc_.brush = static_cast<i32>(i);
            if (doc_.tool == TileTool::Erase) doc_.tool = TileTool::Paint;
        }
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
    if (doc_.brush >= 0) {
        bool solid = ts.IsSolid(doc_.brush);
        if (ImGui::Checkbox("Tile blocks movement", &solid)) doc_.SetSolid(doc_.brush, solid);
    }

    ImGui::SeparatorText("Autotiles");
    for (usize i = 0; i < ts.autotiles.size(); ++i) {
        ImGui::PushID(static_cast<int>(1000 + i));
        const bool active = doc_.brush == AutotileCell(i);
        if (ImGui::Selectable(ts.autotiles[i].name.c_str(), active)) {
            doc_.brush = AutotileCell(i);
            selected_autotile_ = static_cast<int>(i);
        }
        ImGui::PopID();
    }
    if (ImGui::SmallButton("Add autotile")) {
        const usize n = doc_.AddAutotile("Autotile " + std::to_string(ts.autotiles.size() + 1));
        selected_autotile_ = static_cast<int>(n);
        doc_.brush = AutotileCell(n);
    }
    if (selected_autotile_ >= 0 && static_cast<usize>(selected_autotile_) < ts.autotiles.size() && doc_.brush >= 0) {
        // Assigns the brush tile to a neighbour mask of the selected autotile.
        const Autotile& at = ts.autotiles[static_cast<usize>(selected_autotile_)];
        ImGui::TextDisabled("Pick a tile, then click a mask to assign it:");
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
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##paint", ImVec2(std::max(size.x, 1.0f), std::max(size.y, 1.0f)),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();
    if (hovered && io.MouseWheel != 0.0f) zoom = std::clamp(zoom * (io.MouseWheel > 0 ? 1.15f : 1.0f / 1.15f), 6.0f, 64.0f);
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
        pan_x_ += io.MouseDelta.x;
        pan_y_ += io.MouseDelta.y;
    }

    // The map's bottom-left is at the canvas's (pan_x, height - pan_y), y up.
    const f32 base_x = origin.x + pan_x_;
    const f32 base_y = origin.y + size.y - pan_y_;
    const auto to_screen_x = [&](i32 x) { return base_x + static_cast<f32>(x) * zoom; };
    const auto to_screen_y = [&](i32 y) { return base_y - static_cast<f32>(y + 1) * zoom; }; // a cell's top edge

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    // Intersect in screen space first so even an enormous map only visits
    // the cells under the visible part of the canvas (including partial cells).
    const ImVec2 clip_min = dl->GetClipRectMin(), clip_max = dl->GetClipRectMax();
    const ImVec2 visible_min(std::max(clip_min.x, base_x),
                             std::max(clip_min.y, to_screen_y(static_cast<i32>(map.height) - 1)));
    const ImVec2 visible_max(std::min(clip_max.x, to_screen_x(static_cast<i32>(map.width))),
                             std::min(clip_max.y, base_y));
    if (visible_min.x < visible_max.x && visible_min.y < visible_max.y) {
        const auto index = [](f64 value, u32 count) {
            return static_cast<i32>(std::clamp(value, 0.0, static_cast<f64>(count)));
        };
        const i32 left = index(std::floor((static_cast<f64>(visible_min.x) - base_x) / zoom), map.width);
        const i32 right = index(std::ceil((static_cast<f64>(visible_max.x) - base_x) / zoom), map.width);
        const i32 bottom = index(std::floor((static_cast<f64>(base_y) - visible_max.y) / zoom), map.height);
        const i32 top = index(std::ceil((static_cast<f64>(base_y) - visible_min.y) / zoom), map.height);
        dl->AddRectFilled(visible_min, visible_max, IM_COL32(30, 30, 36, 255));
        for (usize layer = 0; layer < map.layers.size(); ++layer) {
            for (i32 y = bottom; y < top; ++y) {
                for (i32 x = left; x < right; ++x) {
                    const i32 tile = ResolveTile(map, ts, layer, x, y);
                    if (tile < 0) continue;
                    const ImVec2 a(to_screen_x(x), to_screen_y(y));
                    const ImVec2 b(a.x + zoom, a.y + zoom);
                    if (!DrawTileArt(dl, a, b, tile)) dl->AddRectFilled(a, b, TileColor(tile));
                    if (show_solid && map.layers[layer].collides && ts.IsSolid(tile))
                        dl->AddRect(a, b, IM_COL32(220, 40, 40, 255), 0, 0, 1.5f);
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
                const f32 sy = base_y - static_cast<f32>(y) * zoom;
                dl->AddLine(ImVec2(visible_min.x, sy), ImVec2(visible_max.x, sy), grid_color);
            }
        }
    }

    if (!ghost_before_.empty()) {
        const auto screen = [&](const GhostPoint& p) {
            return ImVec2(base_x + p.x * (zoom * ghost_pixels_per_unit_ / static_cast<f32>(std::max(ts.tile_width, 1u))),
                          base_y - p.y * (zoom * ghost_pixels_per_unit_ / static_cast<f32>(std::max(ts.tile_height, 1u))));
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
    const bool mouse_valid = !io.AppFocusLost && ImGui::IsMousePosValid();
    i32 cx = 0, cy = 0;
    if (mouse_valid) LocalToCell(io.MousePos.x - base_x, base_y - io.MousePos.y, zoom, cx, cy);
    const bool on_map = mouse_valid && hovered && map.InBounds(cx, cy);
    const bool left = ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::IsItemActive();
    const bool right = ImGui::IsMouseDown(ImGuiMouseButton_Right) && ImGui::IsItemActive();
    if (doc_.tool == TileTool::Rectangle) {
        if (left && on_map && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            rect_active_ = true;
            rect_x_ = cx;
            rect_y_ = cy;
        }
        const bool cancel = !mouse_valid || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                            !map.InBounds(rect_x_, rect_y_);
        if (rect_active_ && (cancel || !left)) {
            if (!cancel && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) doc_.Rectangle(rect_x_, rect_y_, cx, cy);
            rect_active_ = false;
        }
        if (rect_active_) {
            const i32 x = std::clamp(cx, 0, static_cast<i32>(map.width) - 1);
            const i32 y = std::clamp(cy, 0, static_cast<i32>(map.height) - 1);
            const ImVec2 a(to_screen_x(std::min(x, rect_x_)), to_screen_y(std::max(y, rect_y_)));
            const ImVec2 b(to_screen_x(std::max(x, rect_x_) + 1), to_screen_y(std::min(y, rect_y_)) + zoom);
            dl->AddRectFilled(a, b, IM_COL32(130, 214, 255, 48));
            dl->AddRect(a, b, IM_COL32(130, 214, 255, 255), 0.0f, 0, 2.0f);
            if (hovered) ImGui::SetTooltip("Release to fill the rectangle. Escape cancels.");
        }
    } else {
        rect_active_ = false;
    }
    if (on_map && !rect_active_) {
        const ImVec2 a(to_screen_x(cx), to_screen_y(cy));
        dl->AddRect(a, ImVec2(a.x + zoom, a.y + zoom), IM_COL32(255, 255, 255, 200), 0.0f, 0, 2.0f);
        ImGui::SetTooltip("(%d, %d)  tile %d", cx, cy, map.Get(doc_.active_layer, cx, cy));
    }
    dl->PopClipRect();

    if (doc_.tool != TileTool::Rectangle && mouse_valid && (left || right)) {
        const TileTool saved = doc_.tool;
        if (right) doc_.tool = TileTool::Erase;
        if (!dragging_) {
            doc_.BeginStroke();
            dragging_ = true;
        }
        if (doc_.tool == TileTool::Paint || doc_.tool == TileTool::Erase) {
            if (on_map) {
                if (last_brush_sample_ && last_brush_sample_->tool == doc_.tool) {
                    ApplyBrushLine(doc_, last_brush_sample_->x, last_brush_sample_->y, cx, cy);
                } else {
                    doc_.Apply(cx, cy);
                }
                last_brush_sample_ = BrushSample{cx, cy, doc_.tool};
            } else {
                // Do not connect across another panel or a trip outside the map.
                last_brush_sample_.reset();
            }
        } else {
            last_brush_sample_.reset();
            doc_.Apply(cx, cy);
        }
        doc_.tool = saved;
    } else if (dragging_) {
        doc_.EndStroke();
        last_brush_sample_.reset();
        dragging_ = false;
    }
    ImGui::EndChild();
}

} // namespace aether::editor
