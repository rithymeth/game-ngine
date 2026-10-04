#include "tilemap_panel.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

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

} // namespace

void TilemapPanel::Draw() {
    DrawTools();
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
    ImGui::TextDisabled("%ux%u%s", doc_.Map().width, doc_.Map().height, doc_.dirty() ? "  (unsaved)" : "");
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
        if (ImGui::Selectable(map.layers[i].name.c_str(), doc_.active_layer == i)) doc_.active_layer = i;
        ImGui::PopID();
    }
    if (ImGui::SmallButton("Add layer")) doc_.AddLayer("Layer " + std::to_string(map.layers.size() + 1));
    ImGui::SameLine();
    if (ImGui::SmallButton("Remove")) doc_.RemoveLayer(doc_.active_layer);
}

void TilemapPanel::DrawPalette() {
    const Tileset& ts = doc_.GetTileset();
    ImGui::SeparatorText("Tiles");
    const u32 columns = std::max(ts.Columns(), 1u);
    const u32 count = ts.TileCount();
    const f32 cell = 28.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (u32 i = 0; i < count; ++i) {
        if (i % columns != 0) ImGui::SameLine(0.0f, 2.0f);
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##tile", ImVec2(cell, cell))) {
            doc_.brush = static_cast<i32>(i);
            if (doc_.tool == TileTool::Erase) doc_.tool = TileTool::Paint;
        }
        dl->AddRectFilled(p, ImVec2(p.x + cell, p.y + cell), TileColor(static_cast<i32>(i)));
        char label[16];
        std::snprintf(label, sizeof(label), "%u", i);
        dl->AddText(ImVec2(p.x + 3, p.y + 2), IM_COL32(0, 0, 0, 255), label);
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
    dl->AddRectFilled(ImVec2(to_screen_x(0), to_screen_y(static_cast<i32>(map.height) - 1)),
                      ImVec2(to_screen_x(static_cast<i32>(map.width)), base_y), IM_COL32(30, 30, 36, 255));
    for (usize layer = 0; layer < map.layers.size(); ++layer) {
        for (u32 y = 0; y < map.height; ++y) {
            for (u32 x = 0; x < map.width; ++x) {
                const i32 tile = ResolveTile(map, ts, layer, static_cast<i32>(x), static_cast<i32>(y));
                if (tile < 0) continue;
                const ImVec2 a(to_screen_x(static_cast<i32>(x)), to_screen_y(static_cast<i32>(y)));
                const ImVec2 b(a.x + zoom, a.y + zoom);
                if (b.x < origin.x || a.x > origin.x + size.x || b.y < origin.y || a.y > origin.y + size.y) continue;
                dl->AddRectFilled(a, b, TileColor(tile));
                if (show_solid && map.layers[layer].collides && ts.IsSolid(tile)) {
                    dl->AddRect(a, b, IM_COL32(220, 40, 40, 255), 0, 0, 1.5f);
                }
            }
        }
    }
    if (show_grid && zoom >= 10.0f) {
        for (u32 x = 0; x <= map.width; ++x) {
            dl->AddLine(ImVec2(to_screen_x(static_cast<i32>(x)), to_screen_y(static_cast<i32>(map.height) - 1)),
                        ImVec2(to_screen_x(static_cast<i32>(x)), base_y), IM_COL32(255, 255, 255, 30));
        }
        for (u32 y = 0; y <= map.height; ++y) {
            const f32 sy = base_y - static_cast<f32>(y) * zoom;
            dl->AddLine(ImVec2(to_screen_x(0), sy), ImVec2(to_screen_x(static_cast<i32>(map.width)), sy), IM_COL32(255, 255, 255, 30));
        }
    }

    // The cell under the mouse, and painting.
    const i32 cx = static_cast<i32>(std::floor((io.MousePos.x - base_x) / zoom));
    const i32 cy = static_cast<i32>(std::floor((base_y - io.MousePos.y) / zoom));
    if (hovered && map.InBounds(cx, cy)) {
        const ImVec2 a(to_screen_x(cx), to_screen_y(cy));
        dl->AddRect(a, ImVec2(a.x + zoom, a.y + zoom), IM_COL32(255, 255, 255, 200), 0, 0, 2.0f);
        ImGui::SetTooltip("(%d, %d)  tile %d", cx, cy, map.Get(doc_.active_layer, cx, cy));
    }
    dl->PopClipRect();

    const bool left = ImGui::IsMouseDown(ImGuiMouseButton_Left) && ImGui::IsItemActive();
    const bool right = ImGui::IsMouseDown(ImGuiMouseButton_Right) && ImGui::IsItemActive();
    if (doc_.tool == TileTool::Rectangle) {
        if (left && hovered && !rect_active_) {
            rect_active_ = true;
            rect_x_ = cx;
            rect_y_ = cy;
        }
        if (rect_active_ && !left) {
            doc_.Rectangle(rect_x_, rect_y_, cx, cy);
            rect_active_ = false;
        }
    } else if (left || right) {
        const TileTool saved = doc_.tool;
        if (right) doc_.tool = TileTool::Erase;
        if (!dragging_) {
            doc_.BeginStroke();
            dragging_ = true;
        }
        doc_.Apply(cx, cy);
        doc_.tool = saved;
    } else if (dragging_) {
        doc_.EndStroke();
        dragging_ = false;
    }
    ImGui::EndChild();
}

} // namespace aether::editor
