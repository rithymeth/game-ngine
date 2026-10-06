#include "dev/profiler_panel.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace aether::editor {

using dev::FrameProfile;
using dev::MemoryCategory;
using dev::Profiler;
using dev::ZoneStat;

ProfilerPanel::ProfilerPanel(Profiler& profiler) : profiler_(profiler) {}

void ProfilerPanel::Draw() {
    if (ImGui::Button(paused_ ? "Resume" : "Pause")) {
        paused_ = !paused_;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", profiler_.Installed() ? "installed" : "not installed");

    const std::vector<f32> times = profiler_.FrameTimesMs(history_);
    const f32 avg = static_cast<f32>(profiler_.AverageFrameMs(history_));
    const f32 p99 = static_cast<f32>(profiler_.PercentileFrameMs(0.99, history_));
    const f32 fps = static_cast<f32>(profiler_.Fps(history_));

    char header[96];
    std::snprintf(header, sizeof(header), "Frame time  (last %zu frames)", std::min(history_, profiler_.FrameCount()));
    ImGui::SeparatorText(header);

    // A fixed-size child gives the graph a stable canvas; the ImDrawList
    // draws a histogram of frame times with a budget line at 16.7 ms.
    const ImVec2 graph_size = ImGui::GetContentRegionAvail();
    const float graph_height = 96.0f;
    ImGui::BeginChild("##frame_graph", ImVec2(graph_size.x, graph_height), true);
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 area(origin.x + graph_size.x, origin.y + graph_height);
        const float max_ms = 33.3f; // 30 fps floor so spikes stay visible

        auto y_of_ms = [&](float ms) { return area.y - std::min(ms, max_ms) / max_ms * graph_height; };

        // Budget guides: 16.7 ms (60 fps) and 8.3 ms (120 fps).
        draw->AddLine(ImVec2(origin.x, y_of_ms(16.7f)), ImVec2(area.x, y_of_ms(16.7f)),
                      IM_COL32(90, 140, 90, 110));
        draw->AddLine(ImVec2(origin.x, y_of_ms(8.3f)), ImVec2(area.x, y_of_ms(8.3f)),
                      IM_COL32(90, 90, 140, 90));

        if (times.empty()) {
            ImGui::SetCursorScreenPos(ImVec2(origin.x + 4, origin.y + 4));
            ImGui::TextDisabled("no frames yet");
            ImGui::EndChild();
            return;
        }
        const size_t n = times.size();
        const float bin = graph_size.x / static_cast<float>(n);
        const ImU32 color = IM_COL32(120, 170, 255, 220);
        for (size_t i = 0; i < n; ++i) {
            const float x0 = origin.x + bin * static_cast<float>(i);
            const float y0 = y_of_ms(times[i]);
            const float y1 = area.y;
            if (bin > 2.0f) {
                draw->AddRectFilled(ImVec2(x0 + 1, y0), ImVec2(x0 + bin - 2, y1), color);
            } else {
                draw->AddLine(ImVec2(x0 + 1, y0), ImVec2(x0 + 1, y1), color);
            }
        }
        ImGui::EndChild();
    }

    // Summary line: avg / p99 / fps.
    char summary[128];
    std::snprintf(summary, sizeof(summary), "Avg %.2f ms | p99 %.2f ms | %.1f FPS", avg, p99, fps);
    ImGui::TextUnformatted(summary);

    ImGui::SeparatorText("Zones  (inclusive ms, costliest first)");
    const std::vector<ZoneStat> stats = profiler_.Stats();
    if (stats.empty()) {
        ImGui::TextDisabled("no zones recorded");
    } else {
        if (ImGui::BeginTable("##zone_table", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Zone");
            ImGui::TableSetupColumn("Inclusive");
            ImGui::TableSetupColumn("Self");
            ImGui::TableSetupColumn("Count / Max");
            ImGui::TableHeadersRow();
            for (const ZoneStat& s : stats) {
                if (s.inclusive_ms < 0.005f) {
                    continue; // skip noise
                }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(s.name.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.3f", s.inclusive_ms);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.3f", s.self_ms);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%um  %.3f", s.count, s.max_ms);
            }
            ImGui::EndTable();
        }
    }

    ImGui::SeparatorText("Memory");
    const std::vector<MemoryCategory> memory = profiler_.Memory();
    const i64 total = profiler_.TotalMemory();
    if (memory.empty()) {
        ImGui::TextDisabled("no memory tracked");
    } else {
        if (ImGui::BeginTable("##mem_table", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Category");
            ImGui::TableSetupColumn("Current");
            ImGui::TableSetupColumn("Peak");
            ImGui::TableHeadersRow();
            for (const MemoryCategory& c : memory) {
                if (c.bytes == 0 && c.peak == 0) {
                    continue;
                }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(c.name.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.2f MB", c.bytes / 1048576.0);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.2f MB", c.peak / 1048576.0);
            }
            ImGui::EndTable();
        }
        ImGui::Text("Total: %.2f MB", total / 1048576.0);
    }
}

} // namespace aether::editor
