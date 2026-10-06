#include "world/world_panels.h"

#include "aether/scene/components.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace aether::editor {

using namespace aether::terrain;
using namespace aether::streaming;

// --- Terrain ------------------------------------------------------------------------------------------

void TerrainToolPanel::Pointer(bool down, f32 wx, f32 wz) {
    if (!down) {
        if (doc_.InStroke()) doc_.EndStroke();
        has_last_ = false;
        return;
    }
    if (!doc_.InStroke()) doc_.BeginStroke();
    // Dabs a quarter radius apart along the drag, so speed doesn't change the result.
    const f32 spacing = std::max(doc_.brush.radius * 0.25f, 0.01f);
    if (has_last_ && std::hypot(wx - last_x_, wz - last_z_) < spacing) return;
    doc_.Dab(wx, wz);
    ++dabs_;
    has_last_ = true, last_x_ = wx, last_z_ = wz;
}

void TerrainToolPanel::Draw() {
    for (TerrainTool t : {TerrainTool::Raise, TerrainTool::Lower, TerrainTool::Smooth, TerrainTool::Flatten, TerrainTool::Paint}) {
        if (ImGui::RadioButton(TerrainToolName(t), doc_.tool == t)) doc_.tool = t;
        ImGui::SameLine();
    }
    ImGui::NewLine();
    ImGui::SliderFloat("Radius", &doc_.brush.radius, 0.25f, 128.0f, "%.2f m");
    f32 strength = std::fabs(doc_.brush.strength);
    if (ImGui::SliderFloat("Strength", &strength, 0.0f, 1.0f)) doc_.brush.strength = strength;
    ImGui::SliderFloat("Falloff", &doc_.brush.falloff, 0.0f, 1.0f);
    if (doc_.tool == TerrainTool::Raise || doc_.tool == TerrainTool::Lower) ImGui::DragFloat("Height per dab", &doc_.raise_height, 0.01f, 0.0f, 100.0f, "%.2f m");
    if (doc_.tool == TerrainTool::Paint) {
        const auto& layers = doc_.Data().layers;
        const char* name = doc_.brush.layer_index >= 0 && doc_.brush.layer_index < static_cast<i32>(layers.size()) ? layers[static_cast<usize>(doc_.brush.layer_index)].name.c_str() : "Layer";
        if (ImGui::BeginCombo("Layer", name)) {
            for (usize i = 0; i < std::min<usize>(layers.size(), 4); ++i)
                if (ImGui::Selectable(layers[i].name.c_str(), static_cast<i32>(i) == doc_.brush.layer_index)) doc_.brush.layer_index = static_cast<i32>(i);
            ImGui::EndCombo();
        }
        if (doc_.Splatmap().width == 0) ImGui::TextDisabled("This terrain has no splatmap to paint.");
    }
    ImGui::BeginDisabled(!doc_.CanUndo());
    if (ImGui::Button("Undo")) doc_.Undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!doc_.CanRedo());
    if (ImGui::Button("Redo")) doc_.Redo();
    ImGui::EndDisabled();
    ImGui::Text("%.0f x %.0f m, %u x %u chunks, %zu waiting to rebuild", doc_.WorldWidth(), doc_.WorldDepth(), doc_.Data().chunk_count_x,
                doc_.Data().chunk_count_z, doc_.DirtyCount());
}

// --- Foliage ------------------------------------------------------------------------------------------

void FoliagePanel::Pointer(bool down, const Vec3& ground) {
    if (!down) {
        doc_.EndStroke();
        has_last_ = false;
        return;
    }
    doc_.BeginStroke();
    // Painting again in the same place piles up: dab only when the brush has moved half its radius.
    if (has_last_ && std::hypot(ground.x - last_.x, ground.z - last_.z) < doc_.radius * 0.5f) return;
    doc_.Dab(ground);
    has_last_ = true, last_ = ground;
}

void FoliagePanel::Draw() {
    const FoliageLayer& layer = doc_.Layer();
    const std::vector<usize> counts = doc_.CountsByType();
    ImGui::Text("%zu instances (at most %u)", layer.instances.size(), layer.max_instances);
    if (ImGui::RadioButton("Paint", !doc_.erase)) doc_.erase = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("Erase", doc_.erase)) doc_.erase = true;
    ImGui::SliderFloat("Radius", &doc_.radius, 0.25f, 64.0f, "%.2f m");
    f32 density = layer.density;
    if (ImGui::DragFloat("Density", &density, 0.01f, 0.0f, 100.0f, "%.2f / m2") && !ImGui::IsMouseDragging(0)) doc_.SetDensity(density);
    if (ImGui::IsItemDeactivatedAfterEdit()) doc_.SetDensity(density);
    for (usize i = 0; i < layer.types.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        FoliageType t = layer.types[i];
        if (ImGui::TreeNode("type", "Type %zu (%zu)", i, i < counts.size() ? counts[i] : 0)) {
            bool changed = false;
            f32 scale[2] = {t.scale_min, t.scale_max};
            if (ImGui::DragFloat2("Scale", scale, 0.01f, 0.01f, 100.0f)) t.scale_min = std::min(scale[0], scale[1]), t.scale_max = std::max(scale[0], scale[1]), changed = true;
            changed |= ImGui::SliderFloat("Rotation", &t.rotation_range, 0.0f, 3.14159f, "+-%.2f rad");
            changed |= ImGui::Checkbox("Align to ground", &t.align_to_normal);
            changed |= ImGui::DragFloat("Anchor offset", &t.anchor_offset, 0.01f, -10.0f, 10.0f, "%.2f m");
            changed |= ImGui::DragFloat("LOD distance", &t.lod_distance, 0.5f, 0.0f, 10000.0f, "%.0f m");
            changed |= ImGui::DragFloat("Impostor distance", &t.imposter_distance, 0.5f, 0.0f, 10000.0f, "%.0f m");
            if (changed && !ImGui::IsMouseDragging(0)) doc_.SetType(i, t);
            if (ImGui::IsItemDeactivatedAfterEdit()) doc_.SetType(i, t);
            if (ImGui::SmallButton("Remove type")) {
                doc_.RemoveType(i);
                ImGui::TreePop();
                ImGui::PopID();
                break;
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    if (ImGui::Button("Add type")) doc_.AddType(FoliageType{});
    ImGui::SameLine();
    if (ImGui::Button("Clear all")) doc_.Clear();
    ImGui::SameLine();
    ImGui::BeginDisabled(!doc_.CanUndo());
    if (ImGui::Button("Undo")) doc_.Undo();
    ImGui::EndDisabled();
}

// --- Splines ------------------------------------------------------------------------------------------

void SplinePanel::Pointer(bool down, bool add, const Vec3& ground, f32 pick_radius) {
    if (!down) {
        dragging_ = false;
        return;
    }
    if (!dragging_) {
        dragging_ = true;
        if (add) {
            doc_.AddPoint(ground);
            return;
        }
        doc_.selected = doc_.PickPoint(ground, pick_radius);
        return;
    }
    // A drag moves the picked point: one undo step for the whole drag.
    if (doc_.selected) doc_.MovePoint(*doc_.selected, ground, true);
}

void SplinePanel::Draw() {
    const Spline& s = doc_.Get();
    ImGui::Text("%u points, %.1f m", s.PointCount(), s.PointCount() >= 2 ? s.Length() : 0.0f);
    for (u32 i = 0; i < s.PointCount(); ++i) {
        char label[32];
        std::snprintf(label, sizeof(label), "Point %u", i);
        if (ImGui::Selectable(label, doc_.selected == i)) doc_.selected = i;
    }
    if (doc_.selected && *doc_.selected < s.PointCount()) {
        const u32 i = *doc_.selected;
        const SplinePoint p = s.Points()[i];
        f32 pos[3] = {p.position.x, p.position.y, p.position.z};
        if (ImGui::DragFloat3("Position", pos, 0.05f)) doc_.MovePoint(i, Vec3(pos[0], pos[1], pos[2]), ImGui::IsMouseDragging(0));
        f32 width = p.width;
        if (ImGui::DragFloat("Width", &width, 0.05f, 0.0f, 1000.0f, "%.2f m")) doc_.SetWidth(i, width);
        f32 roll = p.roll;
        if (ImGui::DragFloat("Roll", &roll, 0.5f, -180.0f, 180.0f, "%.1f deg")) doc_.SetRoll(i, roll);
        if (ImGui::Button("Insert after")) doc_.InsertAfter(i);
        ImGui::SameLine();
        if (ImGui::Button("Remove")) doc_.RemovePoint(i);
    }
    if (ImGui::Button("Add point")) {
        const Vec3 at = s.PointCount() > 0 ? s.Points()[s.PointCount() - 1].position + Vec3(5, 0, 0) : Vec3();
        doc_.AddPoint(at);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!doc_.CanUndo());
    if (ImGui::Button("Undo")) doc_.Undo();
    ImGui::EndDisabled();
}

// --- World partition ----------------------------------------------------------------------------------

bool WorldPartitionPanel::IsPinned(const CellCoord& cell) const { return pinned_.count(cell) != 0; }

void WorldPartitionPanel::TogglePin(const CellCoord& cell) {
    if (pinned_.erase(cell) != 0) {
        streamer_.Unpin(cell);
    } else {
        pinned_.insert(cell);
        streamer_.Pin(cell);
    }
}

std::optional<CellCoord> WorldPartitionPanel::CellAt(f32 sx, f32 sy) const {
    if (cols_ <= 0 || rows_ <= 0 || pixels_per_cell <= 0.0f) return std::nullopt;
    const i32 cx = static_cast<i32>(std::floor((sx - map_x_) / pixels_per_cell));
    const i32 cz = static_cast<i32>(std::floor((sy - map_y_) / pixels_per_cell));
    if (cx < 0 || cz < 0 || cx >= cols_ || cz >= rows_) return std::nullopt;
    return CellCoord{min_x_ + cx, min_z_ + cz};
}

void WorldPartitionPanel::Draw() {
    const WorldIndex& index = streamer_.Index();
    const auto loaded = streamer_.LoadedCells();
    ImGui::Text("%zu cells of %.0f m, %zu loaded, %zu waiting, %u persistent entities", index.cells.size(), index.settings.cell_size, loaded.size(),
                streamer_.PendingLoads(), index.persistent_count);
    for (const std::string& p : streamer_.Problems()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", p.c_str());
    if (index.cells.empty()) {
        cols_ = rows_ = 0;
        ImGui::TextDisabled("No cells: partition the level first.");
        return;
    }
    i32 x0 = index.cells.front().coord.x, x1 = x0, z0 = index.cells.front().coord.z, z1 = z0;
    for (const CellInfo& c : index.cells) x0 = std::min(x0, c.coord.x), x1 = std::max(x1, c.coord.x), z0 = std::min(z0, c.coord.z), z1 = std::max(z1, c.coord.z);
    min_x_ = x0, min_z_ = z0, cols_ = x1 - x0 + 1, rows_ = z1 - z0 + 1;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    map_x_ = origin.x, map_y_ = origin.y;
    const f32 s = pixels_per_cell;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (const CellInfo& c : index.cells) {
        const f32 px = map_x_ + static_cast<f32>(c.coord.x - x0) * s, py = map_y_ + static_cast<f32>(c.coord.z - z0) * s;
        const CellState st = streamer_.State(c.coord);
        const u32 fill = st == CellState::Loaded ? IM_COL32(60, 170, 80, 255) : st == CellState::Failed ? IM_COL32(190, 60, 60, 255) : IM_COL32(70, 70, 80, 255);
        dl->AddRectFilled(ImVec2(px + 1, py + 1), ImVec2(px + s - 1, py + s - 1), fill);
        if (IsPinned(c.coord)) dl->AddRect(ImVec2(px + 1, py + 1), ImVec2(px + s - 1, py + s - 1), IM_COL32(255, 220, 60, 255), 0.0f, 0, 2.0f);
        char count[16];
        std::snprintf(count, sizeof(count), "%u", c.entity_count);
        dl->AddText(ImVec2(px + 3, py + 2), IM_COL32(230, 230, 230, 255), count);
    }
    // The sources and their reach.
    if (world_ != nullptr) {
        const WorldPosition& off = streamer_.OriginOffset();
        world_->ForEachArchetype([&](Archetype& a) {
            if (!a.Mask().test(GetComponentId<StreamingSource>())) return;
            for (usize ch = 0; ch < a.ChunkCount(); ++ch) {
                for (usize i = 0; i < a.ChunkEntityCount(ch); ++i) {
                    const Entity e = a.EntityArray(ch)[i];
                    const StreamingSource* src = world_->GetComponent<StreamingSource>(e);
                    const Transform* t = world_->GetComponent<Transform>(e);
                    if (src == nullptr || t == nullptr) continue;
                    const f32 wx = static_cast<f32>(off.x + t->position.x) - index.settings.origin.x;
                    const f32 wz = static_cast<f32>(off.z + t->position.z) - index.settings.origin.z;
                    const ImVec2 c(map_x_ + (wx / index.settings.cell_size - static_cast<f32>(x0)) * s, map_y_ + (wz / index.settings.cell_size - static_cast<f32>(z0)) * s);
                    dl->AddCircleFilled(c, 4.0f, src->enabled ? IM_COL32(80, 200, 255, 255) : IM_COL32(120, 120, 120, 255));
                    dl->AddCircle(c, src->radius / index.settings.cell_size * s, IM_COL32(80, 200, 255, 160));
                }
            }
        });
    }
    ImGui::InvisibleButton("##partition_map", ImVec2(static_cast<f32>(cols_) * s, static_cast<f32>(rows_) * s));
    if (ImGui::IsItemClicked(0)) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        if (const auto cell = CellAt(m.x, m.y); cell && index.Find(*cell) != nullptr) TogglePin(*cell);
    }
    if (ImGui::IsItemHovered()) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        if (const auto cell = CellAt(m.x, m.y)) {
            const CellInfo* info = index.Find(*cell);
            ImGui::SetTooltip("Cell %s: %u entities%s", CellName(*cell).c_str(), info != nullptr ? info->entity_count : 0u, IsPinned(*cell) ? " (pinned)" : "");
        }
    }
}

} // namespace aether::editor
