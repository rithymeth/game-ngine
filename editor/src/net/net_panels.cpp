#include "net/net_panels.h"

#include <imgui.h>

#include <cstdio>

#include <algorithm>
#include <map>

namespace aether::editor {

namespace {
f64 Rate(u64 bytes, const net::NetProfile& profile, f64 now) {
    const f64 seconds = now - profile.since;
    return seconds > 1e-9 ? static_cast<f64>(bytes) / seconds : 0.0;
}

std::string Bytes(u64 bytes) {
    char buf[32];
    if (bytes < 10 * 1024) std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    else if (bytes < 10ull * 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<f64>(bytes) / 1024.0);
    else std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<f64>(bytes) / (1024.0 * 1024.0));
    return buf;
}
} // namespace

std::vector<NetEntityRow> BuildEntityRows(const net::NetProfile& profile, f64 now) {
    std::vector<NetEntityRow> rows;
    for (const auto& [net_id, cost] : profile.entities) {
        NetEntityRow row;
        row.net_id = net_id, row.archetype = cost.archetype, row.bytes = cost.bytes;
        row.updates = cost.updates, row.spawns = cost.spawns;
        row.bytes_per_second = Rate(cost.bytes, profile, now);
        for (const auto& [hash, component] : cost.components) row.components.push_back({net::ReplicatedComponentName(hash), component.bytes});
        std::stable_sort(row.components.begin(), row.components.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        rows.push_back(std::move(row));
    }
    std::sort(rows.begin(), rows.end(), [](const NetEntityRow& a, const NetEntityRow& b) {
        return a.bytes != b.bytes ? a.bytes > b.bytes : a.net_id < b.net_id;
    });
    return rows;
}

std::vector<NetFieldRow> BuildFieldRows(const net::NetProfile& profile, f64 now) {
    std::map<std::pair<u32, u32>, NetFieldRow> by_field;
    for (const auto& [net_id, cost] : profile.entities)
        for (const auto& [hash, component] : cost.components)
            for (const auto& [field, fc] : component.fields) {
                NetFieldRow& row = by_field[{hash, field}];
                row.component = net::ReplicatedComponentName(hash);
                row.field = net::ReplicatedFieldName(hash, field);
                row.bytes += fc.bytes;
                row.sends += fc.sends;
            }
    std::vector<NetFieldRow> rows;
    for (auto& [key, row] : by_field) {
        row.bytes_per_second = Rate(row.bytes, profile, now);
        rows.push_back(std::move(row));
    }
    std::stable_sort(rows.begin(), rows.end(), [](const NetFieldRow& a, const NetFieldRow& b) { return a.bytes > b.bytes; });
    return rows;
}

bool NetPlayPanel::Start() {
    error_.clear();
    return session_.Start(edited_, settings, &error_);
}

void NetPlayPanel::Draw() {
    ImGui::PushID(this);
    const bool running = session_.Running();
    if (!running) {
        int mode = static_cast<int>(settings.mode);
        const char* modes[] = {NetPlayModeName(NetPlayMode::ListenServer), NetPlayModeName(NetPlayMode::DedicatedServer)};
        if (ImGui::Combo("Mode", &mode, modes, 2)) settings.mode = static_cast<NetPlayMode>(mode);
        int clients = static_cast<int>(settings.clients);
        if (ImGui::SliderInt("Clients", &clients, 1, 8)) settings.clients = static_cast<u32>(clients);
        ImGui::Checkbox("Replicate the scene", &settings.replicate_scene);
        ImGui::SameLine();
        ImGui::Checkbox("Spawn players", &settings.spawn_players);
    }
    // The simulated network: changeable while playing.
    ImGui::SeparatorText("Network");
    net::LinkConditions c = running ? session_.Settings().conditions : settings.conditions;
    f32 latency_ms = static_cast<f32>(c.latency * 1000.0), jitter_ms = static_cast<f32>(c.jitter * 1000.0);
    f32 loss = c.loss * 100.0f, dup = c.duplicate * 100.0f;
    bool changed = ImGui::SliderFloat("Latency", &latency_ms, 0.0f, 500.0f, "%.0f ms");
    changed |= ImGui::SliderFloat("Jitter", &jitter_ms, 0.0f, 200.0f, "%.0f ms");
    changed |= ImGui::SliderFloat("Loss", &loss, 0.0f, 50.0f, "%.1f %%");
    changed |= ImGui::SliderFloat("Duplicates", &dup, 0.0f, 20.0f, "%.1f %%");
    if (changed) {
        c.latency = latency_ms / 1000.0, c.jitter = jitter_ms / 1000.0, c.loss = loss / 100.0f, c.duplicate = dup / 100.0f;
        settings.conditions = c;
        if (running) session_.SetConditions(c);
    }
    ImGui::Separator();
    if (!running) {
        if (ImGui::Button("Play (networked)")) Start();
        if (!error_.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", error_.c_str());
        ImGui::PopID();
        return;
    }
    if (ImGui::Button("Stop")) {
        session_.Stop();
        ImGui::PopID();
        return;
    }
    ImGui::SameLine();
    ImGui::Text("%s, %.1f s, server sent %s", NetPlayModeName(session_.Settings().mode), session_.Now(),
                Bytes(session_.ServerBytesSent()).c_str());
    if (ImGui::BeginTable("clients", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        for (const char* h : {"Client", "State", "RTT", "Loss", "Sent / Received", "Entities", "Corrections"}) ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        for (usize i = 0; i < session_.ClientCount(); ++i) {
            const NetPlaySession::ClientView v = session_.Client(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("Client %zu", i + 1);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(v.connected ? "Connected" : "Connecting");
            ImGui::TableNextColumn();
            ImGui::Text("%.0f ms", v.rtt * 1000.0);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f %%", v.loss * 100.0f);
            ImGui::TableNextColumn();
            ImGui::Text("%s / %s", Bytes(v.bytes_sent).c_str(), Bytes(v.bytes_received).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%zu", v.entities);
            ImGui::TableNextColumn();
            ImGui::Text("%u", v.corrections);
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
}

void NetProfilerPanel::Draw() {
    ImGui::PushID(this);
    if (!session_.Running()) {
        ImGui::TextDisabled("Start networked play to profile it.");
        ImGui::PopID();
        return;
    }
    const net::NetProfile& p = session_.Profile();
    const f64 now = session_.Now();
    if (ImGui::Button("Reset")) session_.ResetProfile();
    ImGui::SameLine();
    ImGui::Text("%s in %u snapshots over %.1f s (%.2f KB/s); %u despawns", Bytes(p.snapshot_bytes).c_str(), p.snapshots,
                now - p.since, Rate(p.snapshot_bytes, p, now) / 1024.0, p.despawns);
    if (ImGui::BeginTabBar("views")) {
        if (ImGui::BeginTabItem("By entity")) {
            view = View::Entities;
            for (const NetEntityRow& row : BuildEntityRows(p, now)) {
                const std::string label = "#" + std::to_string(row.net_id) + " " + row.archetype + "  " + Bytes(row.bytes) + "  (" +
                                          std::to_string(static_cast<int>(row.bytes_per_second)) + " B/s, " +
                                          std::to_string(row.updates) + " updates)";
                if (ImGui::TreeNode(label.c_str())) {
                    for (const auto& [name, bytes] : row.components) ImGui::BulletText("%s: %s", name.c_str(), Bytes(bytes).c_str());
                    ImGui::TreePop();
                }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("By field")) {
            view = View::Fields;
            if (ImGui::BeginTable("fields", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                for (const char* h : {"Field", "Bytes", "Rate", "Sends"}) ImGui::TableSetupColumn(h);
                ImGui::TableHeadersRow();
                for (const NetFieldRow& row : BuildFieldRows(p, now)) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%s.%s", row.component.c_str(), row.field.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(Bytes(row.bytes).c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%.0f B/s", row.bytes_per_second);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", row.sends);
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::PopID();
}

} // namespace aether::editor
