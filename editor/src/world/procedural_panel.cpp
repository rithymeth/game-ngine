#include "world/procedural_panel.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace aether::editor {

using namespace aether::terrain;
using namespace aether::assets::procedural;

namespace {
// The scatter rule's biome mask as one checkbox per biome.
bool BiomeMask(const char* label, u32& mask) {
    bool changed = false;
    ImGui::TextUnformatted(label);
    for (u32 b = 0; b < kBiomeCount; ++b) {
        if (b > 0 && b != 4) ImGui::SameLine();
        bool on = (mask & (1u << b)) != 0;
        if (ImGui::Checkbox(BiomeName(static_cast<Biome>(b)), &on)) {
            mask = on ? (mask | (1u << b)) : (mask & ~(1u << b));
            changed = true;
        }
    }
    return changed;
}
} // namespace

void ProceduralWorldPanel::DrawPreview() {
    if (preview_revision_ != doc_.Revision() || preview_.empty()) {
        if (doc_.Generated()) {
            preview_ = doc_.Preview(96, preview_w_, preview_h_);
            preview_revision_ = doc_.Revision();
        }
    }
    if (preview_.empty()) {
        ImGui::TextDisabled("Press Generate to see the terrain.");
        return;
    }
    // One filled square per sample of the downsampled picture (headless-friendly: no texture upload).
    const f32 side = std::min(ImGui::GetContentRegionAvail().x, 384.0f);
    const f32 cell = std::max(side / static_cast<f32>(std::max(preview_w_, 1u)), 1.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    for (u32 y = 0; y < preview_h_; ++y)
        for (u32 x = 0; x < preview_w_; ++x) {
            const usize i = (static_cast<usize>(y) * preview_w_ + x) * 3;
            draw->AddRectFilled(ImVec2(origin.x + static_cast<f32>(x) * cell, origin.y + static_cast<f32>(y) * cell),
                                ImVec2(origin.x + static_cast<f32>(x + 1) * cell, origin.y + static_cast<f32>(y + 1) * cell), IM_COL32(preview_[i], preview_[i + 1], preview_[i + 2], 255));
        }
    ImGui::Dummy(ImVec2(cell * static_cast<f32>(preview_w_), cell * static_cast<f32>(preview_h_)));
    const GeneratedTerrain& t = doc_.Terrain();
    ImGui::Text("%u x %u samples, %.0f x %.0f m, heights %.0f to %.0f m", t.Width(), t.Height(), static_cast<f32>(t.Width() - 1) * t.heightmap.cell_size,
                static_cast<f32>(t.Height() - 1) * t.heightmap.cell_size, t.height_min, t.height_max);
    ImGui::Text("%u river cells, %u lake cells, %zu scattered objects, generated in %.0f ms%s", t.river_cells, t.lake_cells, doc_.Instances().size(),
                doc_.GenerateMilliseconds(), doc_.Stale() ? " (settings changed since)" : "");
}

void ProceduralWorldPanel::DrawTerrainSettings(ProceduralWorldSettings& s, bool& changed) {
    TerrainGenSettings& t = s.terrain;
    auto slider = [&](const char* label, f32& v, f32 lo, f32 hi, const char* fmt, const char* tip) {
        changed |= ImGui::SliderFloat(label, &v, lo, hi, fmt);
        if (ImGui::IsItemHovered() && tip) ImGui::SetTooltip("%s", tip);
    };
    auto slider_u = [&](const char* label, u32& v, u32 lo, u32 hi, const char* tip) {
        int value = static_cast<int>(v);
        if (ImGui::SliderInt(label, &value, static_cast<int>(lo), static_cast<int>(hi))) { v = static_cast<u32>(value); changed = true; }
        if (ImGui::IsItemHovered() && tip) ImGui::SetTooltip("%s", tip);
    };
    if (ImGui::CollapsingHeader("Shape", ImGuiTreeNodeFlags_DefaultOpen)) {
        int seed = static_cast<int>(t.seed);
        if (ImGui::InputInt("Seed", &seed)) { t.seed = static_cast<u32>(seed); changed = true; }
        slider_u("Width (samples)", t.width, 33, 513, "Samples along X; 129, 193, 257 or 513 are good sizes");
        slider_u("Depth (samples)", t.height, 33, 513, "Samples along Z");
        slider("Cell size", t.cell_size, 0.25f, 8.0f, "%.2f m", "Metres per sample");
        slider("Height", t.height_scale, 5.0f, 400.0f, "%.0f m", "From the lowest point to the highest");
        slider("Frequency", t.frequency, 0.5f, 12.0f, "%.1f", "Features across the map");
        slider_u("Octaves", t.octaves, 1, 10, "Layers of detail");
        slider("Persistence", t.persistence, 0.2f, 0.8f, "%.2f", "How much each finer layer matters");
        slider("Mountain chains", t.ridge_amount, 0.0f, 1.0f, "%.2f", "0 rolling hills, 1 all ridges");
        slider("Warp", t.warp, 0.0f, 0.6f, "%.2f", "Bends the features so they wind");
        slider("Valley flatness", t.lowland_power, 0.6f, 3.0f, "%.2f", "Above 1 flattens the lowlands and keeps the peaks");
        slider_u("Smoothing", t.smoothing, 0, 8, "Blur passes that take the needles out of the finest detail");
    }
    if (ImGui::CollapsingHeader("Erosion and water")) {
        slider_u("Thermal passes", t.thermal_iterations, 0, 100, "Slides loose material off steep slopes");
        slider("Talus", t.talus, 0.2f, 3.0f, "%.2f", "The steepest slope that stays put");
        int droplets = static_cast<int>(t.hydraulic_droplets);
        if (ImGui::SliderInt("Droplets", &droplets, 0, 300000)) { t.hydraulic_droplets = static_cast<u32>(droplets); changed = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rain droplets that carve gullies; 0 skips it");
        slider_u("River catchment", t.river_min_catchment, 20, 4000, "Cells upstream before a cell is a river: fewer rivers as it grows");
        slider("River depth", t.river_depth, 0.0f, 0.15f, "%.3f", "The deepest cut, as a share of the height");
        slider("Lake depth", t.lake_depth, 0.05f, 3.0f, "%.2f m", "How deep a hollow must fill before it is a lake");
    }
    if (ImGui::CollapsingHeader("Biomes")) {
        slider("Sea level", t.sea_level, 0.0f, 0.8f, "%.2f", "Share of the height range under water");
        slider("Beach band", t.beach_band, 0.0f, 0.15f, "%.3f", "The height range above the sea that is sand");
        slider("Tree line", t.tree_line, 0.2f, 1.0f, "%.2f", "Above this no forest grows");
        slider("Snow line", t.snow_line, 0.3f, 1.0f, "%.2f", "Above this the ground is snow where it can hold");
        slider("Rock slope", t.rock_slope, 0.3f, 3.0f, "%.2f", "Steeper than this is bare rock");
    }
}

void ProceduralWorldPanel::DrawRules(ProceduralWorldSettings& s, bool& changed) {
    if (s.rules.empty()) { ImGui::TextDisabled("No scatter rules."); }
    const std::vector<usize> counts = doc_.CountsByRule();
    selected_rule = std::min(selected_rule, s.rules.empty() ? 0 : s.rules.size() - 1);
    for (usize i = 0; i < s.rules.size(); ++i) {
        char label[160];
        std::snprintf(label, sizeof(label), "%s  (%zu)##rule%zu", s.rules[i].name.c_str(), i < counts.size() ? counts[i] : 0, i);
        if (ImGui::Selectable(label, selected_rule == i)) selected_rule = i;
    }
    if (ImGui::Button("Add rule")) doc_.AddRule(ScatterRule{.name = "New rule"});
    ImGui::SameLine();
    ImGui::BeginDisabled(s.rules.empty());
    if (ImGui::Button("Duplicate")) doc_.DuplicateRule(selected_rule);
    ImGui::SameLine();
    if (ImGui::Button("Remove")) doc_.RemoveRule(selected_rule);
    ImGui::EndDisabled();
    if (s.rules.empty() || selected_rule >= s.rules.size()) return;
    // The buttons above may have changed the rules; edit the live copy.
    s = doc_.Settings();
    if (selected_rule >= s.rules.size()) return;
    ScatterRule& r = s.rules[selected_rule];
    MeshSlot& slot = s.slots[selected_rule];
    ImGui::Separator();
    char name[96];
    std::snprintf(name, sizeof(name), "%s", r.name.c_str());
    if (ImGui::InputText("Name", name, sizeof(name))) { r.name = name; changed = true; }
    changed |= BiomeMask("Grows in", r.biomes);
    changed |= ImGui::SliderFloat("Height min", &r.height_min, 0.0f, 1.0f);
    changed |= ImGui::SliderFloat("Height max", &r.height_max, 0.0f, 1.0f);
    changed |= ImGui::SliderFloat("Slope max", &r.slope_max, 0.0f, 3.0f, "%.2f");
    changed |= ImGui::SliderFloat("Water: at least", &r.water_distance_min, 0.0f, 50.0f, "%.1f m");
    float far_water = std::min(r.water_distance_max, 200.0f);
    if (ImGui::SliderFloat("Water: at most", &far_water, 1.0f, 200.0f, far_water >= 200.0f ? "any distance" : "%.1f m")) { r.water_distance_max = far_water >= 200.0f ? 1.0e9f : far_water; changed = true; }
    changed |= ImGui::SliderFloat("Patchiness", &r.patch_scale, 0.0f, 12.0f, "%.1f");
    if (r.patch_scale > 0.0f) changed |= ImGui::SliderFloat("Patch threshold", &r.patch_threshold, 0.0f, 1.0f);
    changed |= ImGui::SliderFloat("Density", &r.density, 0.0f, 60.0f, "%.2f per 100 m2", ImGuiSliderFlags_Logarithmic);
    int clump = static_cast<int>(r.clump_size);
    if (ImGui::SliderInt("Clump size", &clump, 1, 16)) { r.clump_size = static_cast<u32>(clump); changed = true; }
    if (r.clump_size > 1) changed |= ImGui::SliderFloat("Clump radius", &r.clump_radius, 0.5f, 20.0f, "%.1f m");
    changed |= ImGui::SliderFloat("Scale min", &r.scale_min, 0.05f, 5.0f, "%.2f");
    changed |= ImGui::SliderFloat("Scale max", &r.scale_max, 0.05f, 5.0f, "%.2f");
    changed |= ImGui::SliderFloat("Footprint", &r.footprint, 0.0f, 6.0f, "%.2f m");
    changed |= ImGui::SliderFloat("Overlap", &r.overlap, 0.0f, 1.0f);
    changed |= ImGui::SliderFloat("Tilt to ground", &r.align_to_normal, 0.0f, 1.0f);
    changed |= ImGui::Checkbox("Random yaw", &r.random_yaw);
    ImGui::Separator();
    changed |= ImGui::Checkbox("Bake a mesh for this rule", &slot.enabled);
    if (slot.enabled) {
        const char* kind = ProceduralKindName(slot.kind);
        if (ImGui::BeginCombo("Mesh family", kind)) {
            for (u32 k = 0; k < kProceduralKindCount; ++k)
                if (ImGui::Selectable(ProceduralKindName(static_cast<ProceduralKind>(k)), slot.kind == static_cast<ProceduralKind>(k))) { slot.kind = static_cast<ProceduralKind>(k); changed = true; }
            ImGui::EndCombo();
        }
        int variants = static_cast<int>(slot.variants);
        if (ImGui::SliderInt("Variants", &variants, 1, 24)) { slot.variants = static_cast<u32>(variants); changed = true; }
        changed |= ImGui::SliderFloat("Mesh scale", &slot.scale, 0.1f, 5.0f, "%.2f");
        const ProceduralMesh sample = doc_.BuildMesh(selected_rule, static_cast<u32>(variant_shown_));
        ImGui::Text("Variant %zu: %u triangles, %u vertices, %zu materials", variant_shown_ % std::max(slot.variants, 1u), sample.TriangleCount(), sample.VertexCount(), sample.materials.size());
        if (ImGui::Button("Next variant")) ++variant_shown_;
    } else {
        ImGui::TextDisabled("Scattered and shown in the preview, baked as nothing.");
    }
}

void ProceduralWorldPanel::Draw() {
    ProceduralWorldSettings s = doc_.Settings();
    bool changed = false;

    // Top row: presets, dice, undo.
    for (u32 p = 0; p < kWorldPresetCount; ++p) {
        if (p > 0) ImGui::SameLine();
        if (ImGui::Button(WorldPresetName(static_cast<WorldPreset>(p)))) { doc_.LoadPreset(static_cast<WorldPreset>(p)); s = doc_.Settings(); }
    }
    ImGui::SameLine();
    if (ImGui::Button("Roll seed")) { doc_.RandomizeSeed(); s = doc_.Settings(); }
    ImGui::SameLine();
    ImGui::BeginDisabled(!doc_.CanUndo());
    if (ImGui::Button("Undo")) { doc_.Undo(); s = doc_.Settings(); }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!doc_.CanRedo());
    if (ImGui::Button("Redo")) { doc_.Redo(); s = doc_.Settings(); }
    ImGui::EndDisabled();

    if (ImGui::Button("Generate")) doc_.Generate();
    ImGui::SameLine();
    ImGui::Checkbox("Regenerate as I edit", &auto_generate);

    DrawPreview();
    if (ImGui::BeginTabBar("##procedural")) {
        if (ImGui::BeginTabItem("Terrain")) { DrawTerrainSettings(s, changed); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Scatter")) {
            int scatter_seed = static_cast<int>(s.scatter.seed);
            if (ImGui::InputInt("Scatter seed", &scatter_seed)) { s.scatter.seed = static_cast<u32>(scatter_seed); changed = true; }
            int mesh_seed = static_cast<int>(s.mesh_seed);
            if (ImGui::InputInt("Mesh seed", &mesh_seed)) { s.mesh_seed = static_cast<u32>(mesh_seed); changed = true; }
            DrawRules(s, changed);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Output")) {
            ImGui::BeginDisabled(!doc_.Generated() || terrain_ == nullptr);
            if (ImGui::Button("Apply terrain to the Terrain editor")) status_ = doc_.ApplyToTerrain(*terrain_) ? "Terrain sent to the Terrain editor." : "Nothing to apply.";
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!doc_.Generated() || foliage_ == nullptr);
            if (ImGui::Button("Apply scatter to the Foliage editor")) status_ = doc_.ApplyToFoliage(*foliage_) ? "Scatter sent to the Foliage editor." : "Nothing to apply.";
            ImGui::EndDisabled();
            ImGui::Separator();
            char root[256], name[96];
            std::snprintf(root, sizeof(root), "%s", content_root.c_str());
            std::snprintf(name, sizeof(name), "%s", bake_options.name.c_str());
            if (ImGui::InputText("Content folder", root, sizeof(root))) content_root = root;
            if (ImGui::InputText("Name", name, sizeof(name))) bake_options.name = name;
            int cap = static_cast<int>(bake_options.max_scene_instances);
            if (ImGui::SliderInt("Objects in the scene", &cap, 100, 20000)) bake_options.max_scene_instances = static_cast<u32>(cap);
            ImGui::BeginDisabled(!doc_.Generated());
            if (ImGui::Button("Bake to files")) {
                bake_ = doc_.Bake(content_root, bake_options);
                char line[512];
                if (bake_.ok) std::snprintf(line, sizeof(line), "Baked %zu files; scene %s has %u objects (%u left out).", bake_.files.size(), bake_.scene.c_str(), bake_.instances_in_scene, bake_.instances_left_out);
                else std::snprintf(line, sizeof(line), "Bake failed: %s", bake_.error.c_str());
                status_ = line;
            }
            ImGui::EndDisabled();
            if (bake_.ok && !bake_.rules_without_mesh.empty()) {
                std::string list;
                for (const std::string& n : bake_.rules_without_mesh) list += (list.empty() ? "" : ", ") + n;
                ImGui::TextWrapped("Rules baked as nothing (no mesh): %s", list.c_str());
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (!status_.empty()) ImGui::TextWrapped("%s", status_.c_str());

    // One undo step per edit: the step is taken when a widget first changes a value, not on every tick of a drag.
    if (changed) {
        if (!was_editing_) doc_.Checkpoint();
        doc_.Set(s);
    }
    was_editing_ = ImGui::IsAnyItemActive();
    // Regenerate once the edit settles, if the last generation was quick enough to do it live.
    if (auto_generate && doc_.Stale() && doc_.Generated() && !was_editing_ && doc_.GenerateMilliseconds() <= auto_generate_budget_ms) doc_.Generate();
}

} // namespace aether::editor
