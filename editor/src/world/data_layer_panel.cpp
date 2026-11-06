#include "world/data_layer_panel.h"

#include "aether/scene/data_layers.h"
#include "aether/scene/gameplay.h"
#include "core/commands.h"

#include <imgui.h>

namespace aether::editor {

DataLayerPanel::DataLayerPanel(World& world, GuidIndex& guids, CommandStack& stack)
    : world_(world), guids_(guids), stack_(stack) {}

std::vector<DataLayerPanel::Row> DataLayerPanel::Rows() const {
    std::vector<Row> rows;
    for (const std::string& name : DataLayerNames(world_)) {
        rows.push_back(Row{name, DataLayerSize(world_, name), IsDataLayerActive(world_, name)});
    }
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