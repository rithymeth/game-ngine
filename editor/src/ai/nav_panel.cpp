#include "ai/nav_panel.h"

#include "core/json_edit.h"

#include <cstdio>
#include <imgui.h>

namespace aether::editor {

using nlohmann::json;

json NavSettingsToJson(const nav::NavMeshSettings& s) {
    return {{"cell_size", s.cell_size},
            {"cell_height", s.cell_height},
            {"agent_height", s.agent_height},
            {"agent_radius", s.agent_radius},
            {"agent_max_climb", s.agent_max_climb},
            {"agent_max_slope", s.agent_max_slope},
            {"region_min_size", s.region_min_size},
            {"region_merge_size", s.region_merge_size},
            {"edge_max_length", s.edge_max_length},
            {"edge_max_error", s.edge_max_error},
            {"verts_per_poly", s.verts_per_poly},
            {"detail_sample_distance", s.detail_sample_distance},
            {"detail_sample_max_error", s.detail_sample_max_error},
            {"tile_size", s.tile_size}};
}

bool NavSettingsFromJson(const json& j, nav::NavMeshSettings& out, std::string* error) {
    nav::NavMeshSettings s;
    try {
        s.cell_size = j.value("cell_size", s.cell_size);
        s.cell_height = j.value("cell_height", s.cell_height);
        s.agent_height = j.value("agent_height", s.agent_height);
        s.agent_radius = j.value("agent_radius", s.agent_radius);
        s.agent_max_climb = j.value("agent_max_climb", s.agent_max_climb);
        s.agent_max_slope = j.value("agent_max_slope", s.agent_max_slope);
        s.region_min_size = j.value("region_min_size", s.region_min_size);
        s.region_merge_size = j.value("region_merge_size", s.region_merge_size);
        s.edge_max_length = j.value("edge_max_length", s.edge_max_length);
        s.edge_max_error = j.value("edge_max_error", s.edge_max_error);
        s.verts_per_poly = j.value("verts_per_poly", s.verts_per_poly);
        s.detail_sample_distance = j.value("detail_sample_distance", s.detail_sample_distance);
        s.detail_sample_max_error = j.value("detail_sample_max_error", s.detail_sample_max_error);
        s.tile_size = j.value("tile_size", s.tile_size);
    } catch (const json::exception& e) {
        if (error != nullptr) *error = e.what();
        return false;
    }
    out = s;
    return true;
}

NavigationPanel::NavigationPanel(World& world, nav::NavWorld& nav, const nav::NavCrowd* crowd) : world_(world), nav_(nav), crowd_(crowd) {}

bool NavigationPanel::Bake() {
    std::string error;
    if (!nav_.Bake(settings, &error, &stats_)) {
        status_ = "Bake failed: " + error;
        return false;
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf), "Baked %zu tiles, %zu polygons in %.1f ms", stats_.tiles, stats_.polygons, stats_.milliseconds);
    status_ = buf;
    return true;
}

void NavigationPanel::BuildOverlay(nav::NavDebugDraw& out) {
    out.Clear();
    if (!show_overlay) return;
    if (nav_.Ready()) nav::DrawDynamicNavMesh(nav_.Dynamic(), mesh_options, out);
    ai::DrawAIDebug(world_, crowd_, ai_options, out);
}

void NavigationPanel::Draw() {
    if (ImGui::CollapsingHeader("Bake settings", ImGuiTreeNodeFlags_DefaultOpen)) {
        json j = NavSettingsToJson(settings);
        for (auto& [key, value] : j.items()) {
            ImGui::PushID(key.c_str());
            bool dragging = false;
            json v = value;
            if (EditJsonField(key, v, dragging)) {
                json next = NavSettingsToJson(settings);
                next[key] = v;
                std::string error;
                if (!NavSettingsFromJson(next, settings, &error)) status_ = error;
            }
            ImGui::PopID();
        }
    }
    if (ImGui::Button("Bake")) Bake();
    ImGui::SameLine();
    ImGui::BeginDisabled(!nav_.Ready());
    if (ImGui::Button("Rebuild all tiles")) nav_.Dynamic().MarkAllDirty();
    ImGui::EndDisabled();
    if (!status_.empty()) ImGui::TextDisabled("%s", status_.c_str());
    if (ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (!nav_.Ready()) {
            ImGui::TextDisabled("Not baked.");
        } else {
            const nav::DynamicNavMesh& d = nav_.Dynamic();
            ImGui::Text("Tiles: %zu (grid %d x %d)", d.Mesh().TileCount(), d.Mesh().Data().tiles_x, d.Mesh().Data().tiles_z);
            ImGui::Text("Polygons: %zu", d.Mesh().PolygonCount());
            ImGui::Text("Waiting to rebake: %zu", d.PendingTiles());
            ImGui::Text("Rebaked so far: %zu", d.TilesRebuilt());
            ImGui::Text("Revision: %llu", static_cast<unsigned long long>(d.Revision()));
            ImGui::Text("Volumes: %zu, links: %zu", d.VolumeCount() + d.Geometry().volumes.size(), d.Mesh().Links().size());
            if (crowd_ != nullptr) ImGui::Text("Agents: %zu", crowd_->AgentCount());
        }
    }
    if (ImGui::CollapsingHeader("Overlay", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Show", &show_overlay);
        ImGui::BeginDisabled(!show_overlay);
        ImGui::Checkbox("Polygons", &mesh_options.polygons);
        ImGui::Checkbox("Edges", &mesh_options.edges);
        ImGui::Checkbox("Tile bounds", &mesh_options.tile_bounds);
        ImGui::Checkbox("Links", &mesh_options.links);
        ImGui::Checkbox("Volumes", &mesh_options.volumes);
        ImGui::Checkbox("Agent paths", &ai_options.agent_paths);
        ImGui::Checkbox("Sight", &ai_options.sight);
        ImGui::Checkbox("Hearing", &ai_options.hearing);
        ImGui::Checkbox("What AIs know", &ai_options.known);
        ImGui::EndDisabled();
    }
}

} // namespace aether::editor
