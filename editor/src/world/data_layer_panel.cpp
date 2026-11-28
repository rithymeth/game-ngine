#include "world/data_layer_panel.h"

#include "aether/scene/data_layers.h"
#include "aether/scene/gameplay.h"
#include "core/commands.h"

#include <imgui.h>

#include <algorithm>
#include <string_view>
#include <unordered_map>

namespace aether::editor {

DataLayerPanel::DataLayerPanel(World& world, GuidIndex& guids, CommandStack& stack)
    : world_(world), guids_(guids), stack_(stack) {}

std::vector<DataLayerPanel::Row> DataLayerPanel::Rows() const {
    struct Summary {
        usize entities = 0;
        bool all_active = true;
    };
    // Borrow the names only while reading the unchanged world. Count every
    // layer member, then find inactive overrides without scanning per layer.
    std::unordered_map<std::string_view, Summary> summaries;
    world_.ForEach<DataLayer>([&](const DataLayer& layer) {
        if (!layer.layer.empty()) ++summaries[layer.layer].entities;
    });
    world_.ForEach<DataLayer, Active>([&](const DataLayer& layer, const Active& active) {
        if (!layer.layer.empty() && !active.active) summaries[layer.layer].all_active = false;
    });

    std::vector<Row> rows;
    rows.reserve(summaries.size());
    for (const auto& [name, summary] : summaries)
        rows.push_back({std::string(name), summary.entities, summary.all_active});
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.layer < b.layer; });
    return rows;
}

bool DataLayerPanel::SetLayerActive(const std::string& layer, bool active) {
    CommandContext ctx{world_, guids_};
    std::unique_ptr<ICommand> command = SetDataLayerActiveCommand::Make(ctx, layer, active);
    if (command == nullptr) return false; // no such layer, or every member already matches
    command->Do(ctx);
    stack_.Record(std::move(command));
    return true;
}

void DataLayerPanel::Draw() {
    if (!open) return;
    bool shown = open;
    if (ImGui::Begin("Data Layers", &shown)) {
        const std::vector<Row> rows = Rows();
        if (rows.empty()) ImGui::TextDisabled("No data layers in this scene.");
        for (const Row& row : rows) {
            bool on = row.active;
            ImGui::PushID(row.layer.c_str());
            if (ImGui::Checkbox("##active", &on)) SetLayerActive(row.layer, on);
            ImGui::PopID();
            ImGui::SameLine();
            ImGui::Text("%s (%zu entit%s)", row.layer.c_str(), row.entities, row.entities == 1 ? "y" : "ies");
        }
    }
    ImGui::End();
    open = shown;
}

} // namespace aether::editor