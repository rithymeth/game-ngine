#include "devtools/profiler_panel.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <cmath>

namespace aether::editor {

namespace {

ImU32 ZoneColor(const char* name) {
    u32 h = 2166136261u;
    for (const char* p = name; *p; ++p) h = (h ^ static_cast<u8>(*p)) * 16777619u;
    const float hue = static_cast<float>(h % 360) / 360.0f;
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(hue, 0.45f, 0.85f, r, g, b);
    return ImGui::GetColorU32(ImVec4(r, g, b, 1.0f));
}

std::string Bytes(i64 bytes) {
    char buf[32];
    const f64 b = static_cast<f64>(bytes);
    if (std::abs(bytes) < 10 * 1024) std::snprintf(buf, sizeof(buf), "%lld B", static_cast<long long>(bytes));
    else if (std::abs(bytes) < 10ll * 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.1f KB", b / 1024.0);
    else std::snprintf(buf, sizeof(buf), "%.1f MB", b / (1024.0 * 1024.0));
    return buf;
}

} // namespace

bool ProfilerPanel::SelectedFrame(ProfileFrame& out) const {
    const std::vector<ProfileFrame> frames = profiler_.Frames();
    if (frames.empty()) return false;
    const int i = selected < 0 || selected >= static_cast<int>(frames.size()) ? static_cast<int>(frames.size()) - 1 : selected;
    out = frames[static_cast<usize>(i)];
    return true;
}

std::vector<ZoneStat> ProfilerPanel::Stats() const {
    if (scope == Scope::AllFrames) {
        const std::vector<ProfileFrame> frames = profiler_.Frames();
        return Profiler::Aggregate(frames);
    }
    ProfileFrame frame;
    if (!SelectedFrame(frame)) return {};
    return Profiler::Aggregate(std::span<const ProfileFrame>(&frame, 1));
}

bool ProfilerPanel::Export() {
    const std::vector<ProfileFrame> frames = profiler_.Frames();
    std::string error;
    const bool ok = profiler_.WriteChromeTrace(export_path, frames, &error);
    status_ = ok ? "Wrote " + std::to_string(frames.size()) + " frames to " + export_path : error;
    return ok;
}

void ProfilerPanel::DrawGraph(const std::vector<ProfileFrame>& frames) {
    const ImVec2 size(ImGui::GetContentRegionAvail().x, 90.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), ImGui::GetColorU32(ImGuiCol_FrameBg));
    f64 max_ms = 33.4;
    for (const ProfileFrame& f : frames) max_ms = std::max(max_ms, f.Ms());
    const auto y_of = [&](f64 ms) { return origin.y + size.y - static_cast<float>(ms / max_ms) * size.y; };
    for (f64 budget : {1000.0 / 60.0, 1000.0 / 30.0}) {
        dl->AddLine(ImVec2(origin.x, y_of(budget)), ImVec2(origin.x + size.x, y_of(budget)), IM_COL32(255, 255, 255, 60));
    }
    const usize n = std::max<usize>(frames.size(), 1);
    const float w = size.x / static_cast<float>(std::max<usize>(n, 60));
    const int shown = selected < 0 ? static_cast<int>(frames.size()) - 1 : selected;
    for (usize i = 0; i < frames.size(); ++i) {
        const f64 ms = frames[i].Ms();
        const ImU32 color = static_cast<int>(i) == shown ? IM_COL32(255, 255, 255, 255)
                            : ms > 1000.0 / 30.0 ? IM_COL32(230, 80, 80, 255)
                            : ms > 1000.0 / 60.0 ? IM_COL32(230, 190, 70, 255)
                                                 : IM_COL32(90, 190, 110, 255);
        const float x = origin.x + static_cast<float>(i) * w;
        dl->AddRectFilled(ImVec2(x, y_of(ms)), ImVec2(x + std::max(w - 1.0f, 1.0f), origin.y + size.y), color);
    }
    ImGui::InvisibleButton("graph", size);
    if (ImGui::IsItemHovered() && !frames.empty()) {
        const int i = std::clamp(static_cast<int>((ImGui::GetIO().MousePos.x - origin.x) / w), 0, static_cast<int>(frames.size()) - 1);
        ImGui::SetTooltip("Frame %llu: %.2f ms", static_cast<unsigned long long>(frames[static_cast<usize>(i)].index),
                          frames[static_cast<usize>(i)].Ms());
        if (ImGui::IsItemClicked()) selected = i;
    }
}

void ProfilerPanel::DrawTimeline(const ProfileFrame& frame) {
    std::map<u32, u16> depth_by_thread;
    for (const ProfileZone& z : frame.zones) depth_by_thread[z.thread] = std::max<u16>(depth_by_thread[z.thread], static_cast<u16>(z.depth + 1));
    const float row = ImGui::GetTextLineHeight() + 4.0f, label = 90.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(ImGui::GetContentRegionAvail().x - label, 50.0f);
    const f64 span_ns = static_cast<f64>(std::max<u64>(frame.end_ns - frame.start_ns, 1));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y = origin.y;
    const ProfileZone* hovered = nullptr;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    for (const auto& [thread, depth] : depth_by_thread) {
        dl->AddText(ImVec2(origin.x, y), ImGui::GetColorU32(ImGuiCol_Text), profiler_.ThreadName(thread).c_str());
        for (const ProfileZone& z : frame.zones) {
            if (z.thread != thread) continue;
            const f64 s = static_cast<f64>(std::max(z.start_ns, frame.start_ns) - frame.start_ns) / span_ns;
            const f64 e = static_cast<f64>(std::min(z.end_ns, frame.end_ns) - frame.start_ns) / span_ns;
            if (z.end_ns < frame.start_ns || e <= s) continue;
            const ImVec2 a(origin.x + label + static_cast<float>(s) * width, y + static_cast<float>(z.depth) * row);
            const ImVec2 b(origin.x + label + std::max(static_cast<float>(e) * width, static_cast<float>(s) * width + 1.0f), a.y + row - 1.0f);
            dl->AddRectFilled(a, b, ZoneColor(z.name));
            if (b.x - a.x > 30.0f) {
                dl->PushClipRect(a, b, true);
                dl->AddText(ImVec2(a.x + 2, a.y + 1), IM_COL32(20, 20, 20, 255), z.name);
                dl->PopClipRect();
            }
            if (mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y) hovered = &z;
        }
        y += static_cast<float>(depth) * row + 6.0f;
    }
    ImGui::Dummy(ImVec2(label + width, std::max(y - origin.y, row)));
    if (hovered) ImGui::SetTooltip("%s\n%.3f ms", hovered->name, hovered->Ms());
}

void ProfilerPanel::Draw() {
    ImGui::PushID(this);
    bool enabled = profiler_.Enabled();
    if (ImGui::Checkbox("Record", &enabled)) profiler_.SetEnabled(enabled);
    ImGui::SameLine();
    if (ImGui::Button(profiler_.Paused() ? "Resume" : "Pause")) profiler_.SetPaused(!profiler_.Paused());
    ImGui::SameLine();
    if (ImGui::Button("Clear")) profiler_.Clear(), selected = -1;
    ImGui::SameLine();
    if (ImGui::Button("Export trace")) Export();
    ImGui::SameLine();
    char path[256];
    std::snprintf(path, sizeof(path), "%s", export_path.c_str());
    ImGui::SetNextItemWidth(200);
    if (ImGui::InputText("##path", path, sizeof(path))) export_path = path;
    if (!status_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", status_.c_str());
    }

    const std::vector<ProfileFrame> frames = profiler_.Frames();
    if (frames.empty()) {
        ImGui::TextDisabled("No frames yet: the game loop calls Profiler::BeginFrame/EndFrame.");
        ImGui::PopID();
        return;
    }
    f64 avg = 0, worst = 0;
    for (const ProfileFrame& f : frames) avg += f.Ms(), worst = std::max(worst, f.Ms());
    avg /= static_cast<f64>(frames.size());
    ImGui::Text("%zu frames: average %.2f ms (%.0f fps), worst %.2f ms%s", frames.size(), avg, avg > 0 ? 1000.0 / avg : 0.0, worst,
                selected >= 0 ? "  - inspecting a frame (right-click the graph to follow the newest)" : "");
    DrawGraph(frames);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) selected = -1;
    ProfileFrame frame;
    SelectedFrame(frame);
    ImGui::Text("Frame %llu: %.3f ms, %zu zones", static_cast<unsigned long long>(frame.index), frame.Ms(), frame.zones.size());

    if (ImGui::BeginTabBar("profiler")) {
        if (ImGui::BeginTabItem("Zones")) {
            tab = Tab::Zones;
            int s = static_cast<int>(scope);
            ImGui::RadioButton("This frame", &s, 0);
            ImGui::SameLine();
            ImGui::RadioButton("All kept frames", &s, 1);
            scope = static_cast<Scope>(s);
            if (ImGui::BeginTable("zones", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                                  ImVec2(0, 260))) {
                for (const char* h : {"Zone", "Calls", "Total ms", "Self ms", "Max ms"}) ImGui::TableSetupColumn(h);
                ImGui::TableHeadersRow();
                for (const ZoneStat& st : Stats()) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(st.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", st.calls);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", scope == Scope::AllFrames ? st.per_frame_ms : st.total_ms);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", st.self_ms);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", st.max_ms);
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Timeline")) {
            tab = Tab::Timeline;
            DrawTimeline(frame);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Counters")) {
            tab = Tab::Counters;
            for (const auto& [name, value] : frame.counters) ImGui::BulletText("%s: %.0f", name.c_str(), value);
            if (frame.counters.empty()) ImGui::TextDisabled("No counters this frame.");
            ImGui::SeparatorText("GPU passes");
            f64 gpu_total = 0;
            for (const GpuTiming& g : frame.gpu) ImGui::BulletText("%s: %.3f ms", g.name.c_str(), g.ms), gpu_total += g.ms;
            if (frame.gpu.empty()) ImGui::TextDisabled("No GPU timings (the render backend reports them).");
            else ImGui::Text("GPU total: %.3f ms", gpu_total);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Memory")) {
            tab = Tab::Memory;
            if (ImGui::BeginTable("memory", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                for (const char* h : {"Category", "In use", "Peak", "Allocations", "Frees"}) ImGui::TableSetupColumn(h);
                ImGui::TableHeadersRow();
                i64 total = 0;
                for (const MemoryCategoryStats& m : MemoryTracker::Get().Categories()) {
                    total += m.bytes;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(m.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(Bytes(m.bytes).c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(Bytes(m.peak).c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%llu", static_cast<unsigned long long>(m.allocations));
                    ImGui::TableNextColumn();
                    ImGui::Text("%llu", static_cast<unsigned long long>(m.frees));
                }
                ImGui::EndTable();
                ImGui::Text("Total tracked: %s", Bytes(total).c_str());
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::PopID();
}

} // namespace aether::editor
