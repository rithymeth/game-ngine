#include "net/network_panel.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>

namespace aether::editor {

using namespace aether::net;

const std::vector<LinkPreset>& LinkPresets() {
    static const std::vector<LinkPreset> presets = {
        {"None", {0.0, 0.0, 0.0, 0.0}},
        {"LAN", {0.001, 0.001, 0.0, 0.0}},
        {"Good broadband", {0.025, 0.005, 0.001, 0.0}},
        {"Mobile", {0.060, 0.030, 0.01, 0.0}},
        {"Poor", {0.120, 0.060, 0.05, 0.01}},
        {"Terrible", {0.250, 0.150, 0.15, 0.03}},
    };
    return presets;
}

bool NetworkPanel::ApplyPreset(usize index) {
    const auto& presets = LinkPresets();
    if (index >= presets.size()) return false;
    session_.Conditions() = presets[index].conditions;
    preset_ = static_cast<int>(index);
    return true;
}

void NetworkPanel::Sample(f64 dt) {
    window_ += dt;
    if (window_ < 1.0) return;
    const u64 total = session_.ServerBytesSent();
    history_.push_back(static_cast<f32>(static_cast<f64>(total - std::min(total, last_total_)) / window_));
    last_total_ = total;
    window_ = 0.0;
    while (history_.size() > max_history) history_.erase(history_.begin());
}

std::vector<FieldCostRow> NetworkPanel::TopFields(usize count) const {
    std::vector<FieldCostRow> rows;
    for (const auto& [key, traffic] : session_.Replication().Profile().fields) {
        FieldCostRow row;
        row.bytes = traffic.bytes;
        row.sends = traffic.sends;
        const ReplicatedComponent* rc = FindReplicated(key.first);
        row.component = rc != nullptr ? rc->type->name : "?";
        row.field = rc != nullptr && key.second < rc->fields.size() ? rc->fields[key.second]->name : "?";
        rows.push_back(std::move(row));
    }
    std::sort(rows.begin(), rows.end(), [](const FieldCostRow& a, const FieldCostRow& b) {
        return a.bytes != b.bytes ? a.bytes > b.bytes : (a.component != b.component ? a.component < b.component : a.field < b.field);
    });
    if (rows.size() > count) rows.resize(count);
    return rows;
}

std::vector<EntityCostRow> NetworkPanel::TopEntities(usize count) const {
    std::vector<EntityCostRow> rows;
    for (const auto& [id, bytes] : session_.Replication().Profile().entity_bytes) rows.push_back({id, bytes});
    std::sort(rows.begin(), rows.end(), [](const EntityCostRow& a, const EntityCostRow& b) {
        return a.bytes != b.bytes ? a.bytes > b.bytes : a.net_id < b.net_id;
    });
    if (rows.size() > count) rows.resize(count);
    return rows;
}

void NetworkPanel::Draw() {
    if (ImGui::CollapsingHeader("Simulated link", ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto& presets = LinkPresets();
        if (ImGui::BeginCombo("Preset", presets[static_cast<usize>(preset_)].name)) {
            for (usize i = 0; i < presets.size(); ++i) {
                if (ImGui::Selectable(presets[i].name, static_cast<int>(i) == preset_)) ApplyPreset(i);
            }
            ImGui::EndCombo();
        }
        LinkConditions& c = session_.Conditions();
        float latency_ms = static_cast<float>(c.latency * 1000.0);
        float jitter_ms = static_cast<float>(c.jitter * 1000.0);
        float loss_pct = static_cast<float>(c.loss * 100.0);
        float dup_pct = static_cast<float>(c.duplicate * 100.0);
        if (ImGui::SliderFloat("Latency (one way)", &latency_ms, 0.0f, 500.0f, "%.0f ms")) c.latency = latency_ms / 1000.0f;
        if (ImGui::SliderFloat("Jitter", &jitter_ms, 0.0f, 300.0f, "%.0f ms")) c.jitter = jitter_ms / 1000.0f;
        if (ImGui::SliderFloat("Packet loss", &loss_pct, 0.0f, 50.0f, "%.1f %%")) c.loss = loss_pct / 100.0f;
        if (ImGui::SliderFloat("Duplication", &dup_pct, 0.0f, 20.0f, "%.1f %%")) c.duplicate = dup_pct / 100.0f;
    }

    if (ImGui::CollapsingHeader("Clients", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button("Add client")) session_.AddClient();
        if (ImGui::BeginTable("clients", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("#");
            ImGui::TableSetupColumn("State");
            ImGui::TableSetupColumn("RTT");
            ImGui::TableSetupColumn("Lost");
            ImGui::TableSetupColumn("Down");
            ImGui::TableSetupColumn("Entities");
            ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            for (usize i = 0; i < session_.ClientCount(); ++i) {
                const NetPlayClientInfo info = session_.Info(i);
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%zu", i);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(info.connected ? "Connected" : "Offline");
                ImGui::TableNextColumn(); ImGui::Text("%.0f ms", info.rtt * 1000.0);
                ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(info.packets_lost));
                ImGui::TableNextColumn(); ImGui::Text("%.1f KB", static_cast<double>(info.bytes_to_client) / 1024.0);
                ImGui::TableNextColumn(); ImGui::Text("%zu", info.entities);
                ImGui::TableNextColumn();
                if (info.connected) {
                    if (ImGui::SmallButton("Disconnect")) session_.DisconnectClient(i);
                } else if (ImGui::SmallButton("Reconnect")) {
                    session_.ReconnectClient(i);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader("Bandwidth", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Server send rate: %.1f KB/s", CurrentBandwidth() / 1024.0);
        if (!history_.empty()) ImGui::PlotLines("##bandwidth", history_.data(), static_cast<int>(history_.size()), 0, nullptr, 0.0f, FLT_MAX, ImVec2(-1, 60));
        const ReplicationStats& s = session_.Replication().Stats();
        ImGui::Text("Records: %llu (spawn %llu, update %llu, remove %llu, despawn %llu)", static_cast<unsigned long long>(s.records_sent),
                    static_cast<unsigned long long>(s.spawns), static_cast<unsigned long long>(s.updates),
                    static_cast<unsigned long long>(s.removals), static_cast<unsigned long long>(s.despawns));
        ImGui::Text("Deferred: %llu   Oversized: %llu", static_cast<unsigned long long>(s.deferred), static_cast<unsigned long long>(s.oversized));
    }

    if (ImGui::CollapsingHeader("Profiler", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button("Reset")) session_.Replication().ResetProfile();
        const u64 total = session_.Replication().Profile().total_bytes;
        ImGui::Text("Total replicated: %.1f KB", static_cast<double>(total) / 1024.0);
        if (ImGui::BeginTable("fields", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Field");
            ImGui::TableSetupColumn("Bytes");
            ImGui::TableSetupColumn("Sends");
            ImGui::TableSetupColumn("Share");
            ImGui::TableHeadersRow();
            for (const FieldCostRow& row : TopFields(10)) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%s.%s", row.component.c_str(), row.field.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(row.bytes));
                ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(row.sends));
                ImGui::TableNextColumn(); ImGui::Text("%.1f %%", total != 0 ? 100.0 * static_cast<double>(row.bytes) / static_cast<double>(total) : 0.0);
            }
            ImGui::EndTable();
        }
        if (ImGui::BeginTable("entities", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Entity (net id)");
            ImGui::TableSetupColumn("Bytes");
            ImGui::TableHeadersRow();
            for (const EntityCostRow& row : TopEntities(10)) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%u", row.net_id);
                ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(row.bytes));
            }
            ImGui::EndTable();
        }
    }
}

} // namespace aether::editor
