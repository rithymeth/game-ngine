#include "dev/stat_overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace aether::editor {

namespace detail {

std::string FormatBytes(i64 bytes) {
    static const char* const kUnits[] = {"B", "KB", "MB", "GB", "TB"};
    if (bytes == 0) {
        return "0 B";
    }
    const bool negative = bytes < 0;
    double value = std::fabs(static_cast<double>(bytes));
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f %s", value, kUnits[unit]);
    std::string out = buf;
    if (negative) {
        out.insert(out.begin(), '-');
    }
    return out;
}

} // namespace detail

// Formats an integer with thousands separators ("45678" -> "45,678").
static std::string FormatCount(i64 value) {
    const bool negative = value < 0;
    std::string digits = std::to_string(negative ? -value : value);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3);
    for (usize i = 0; i < digits.size(); ++i) {
        const usize remaining = digits.size() - i;
        if (i > 0 && remaining % 3 == 0) {
            out.push_back(',');
        }
        out.push_back(digits[i]);
    }
    if (negative) {
        out.insert(out.begin(), '-');
    }
    return out;
}

bool DrawStatOverlay(const StatOverlayFlags& flags, const StatOverlayValues& v, ImVec2 anchor) {
    // Early-out before touching ImGui, so this is safe with no context when
    // no group is enabled.
    if (!flags.fps && !flags.gpu && !flags.memory) {
        return false;
    }

    std::vector<std::string> lines;
    if (flags.fps) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "FPS %.1f  (avg %.1fms, p99 %.1fms)", static_cast<double>(v.fps),
                      static_cast<double>(v.avg_ms), static_cast<double>(v.p99_ms));
        lines.emplace_back(buf);

        if (v.draw_calls >= 0.0f || v.triangles >= 0.0f) {
            std::string detail;
            if (v.draw_calls >= 0.0f) {
                detail += "Draw calls " + FormatCount(static_cast<i64>(v.draw_calls));
            }
            if (v.triangles >= 0.0f) {
                if (!detail.empty()) {
                    detail += ", ";
                }
                detail += "Tris " + FormatCount(static_cast<i64>(v.triangles));
            }
            lines.push_back(std::move(detail));
        }
    }
    if (flags.gpu && v.gpu_ms >= 0.0f) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "GPU %.1f ms", static_cast<double>(v.gpu_ms));
        lines.push_back(buf);
    }
    if (flags.memory && v.memory_bytes >= 0) {
        lines.push_back("Memory " + detail::FormatBytes(v.memory_bytes));
    }
    if (lines.empty()) {
        return false;
    }

    // Measure each line and let the box auto-grow to fit.
    const float pad = 8.0f;
    const float gap = 1.0f;
    float line_height = 0.0f;
    float max_width = 0.0f;
    for (const std::string& line : lines) {
        const ImVec2 size = ImGui::CalcTextSize(line.c_str());
        max_width = std::max(max_width, size.x);
        line_height = std::max(line_height, size.y);
    }
    const float box_w = max_width + pad * 2.0f;
    const float box_h = line_height * static_cast<float>(lines.size()) + gap * static_cast<float>(lines.size() - 1) +
                        pad * 2.0f;

    // Screen anchor: viewport work origin (0 if none) plus the caller's
    // anchor offset (default {12,12}).
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float ox = (viewport != nullptr ? viewport->WorkPos.x : 0.0f) + anchor.x;
    const float oy = (viewport != nullptr ? viewport->WorkPos.y : 0.0f) + anchor.y;

    const ImVec2 rect_min(ox, oy);
    const ImVec2 rect_max(ox + box_w, oy + box_h);
    const float rounding = 4.0f;

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(rect_min, rect_max, IM_COL32(25, 28, 34, 220), rounding);
    draw->AddRect(rect_min, rect_max, IM_COL32(90, 100, 120, 255), rounding);

    // Text, upper-left aligned with a one-pixel gap between lines.
    const ImU32 text_color = IM_COL32(225, 231, 242, 255);
    float y = oy + pad;
    for (usize i = 0; i < lines.size(); ++i) {
        draw->AddText(ImVec2(ox + pad, y), text_color, lines[i].c_str());
        y += line_height + gap;
    }

    // Click hit-test on the box (kept for future wiring).
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    return ImGui::IsMouseClicked(0) && mouse.x >= rect_min.x && mouse.x <= rect_max.x && mouse.y >= rect_min.y &&
           mouse.y <= rect_max.y;
}

} // namespace aether::editor
