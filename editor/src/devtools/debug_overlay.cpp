#include "devtools/debug_overlay.h"

#include <algorithm>

namespace aether::editor {

namespace {

constexpr f32 kNearW = 1e-4f;

ImVec2 ToScreen(const Vec4& clip, ImVec2 origin, ImVec2 size) {
    const f32 x = clip.x / clip.w, y = clip.y / clip.w;
    return ImVec2(origin.x + (x * 0.5f + 0.5f) * size.x, origin.y + (0.5f - y * 0.5f) * size.y);
}

} // namespace

bool ProjectToScreen(const Mat4& view_proj, const Vec3& p, ImVec2 origin, ImVec2 size, ImVec2& out) {
    const Vec4 clip = view_proj * Vec4(p.x, p.y, p.z, 1.0f);
    if (clip.w <= kNearW) return false;
    out = ToScreen(clip, origin, size);
    return true;
}

void DebugOverlay::Draw(ImDrawList* dl, const Mat4& view_proj, ImVec2 origin, ImVec2 size) {
    lines_drawn_ = texts_drawn_ = stat_lines_drawn_ = 0;
    const ImVec2 max(origin.x + size.x, origin.y + size.y);
    dl->PushClipRect(origin, max, true);
    if (show_debug_draw) {
        for (const DebugLine& line : DebugDrawList::Get().Lines()) {
            Vec4 a = view_proj * Vec4(line.a.x, line.a.y, line.a.z, 1.0f);
            Vec4 b = view_proj * Vec4(line.b.x, line.b.y, line.b.z, 1.0f);
            if (a.w <= kNearW && b.w <= kNearW) continue;
            // Clip the part behind the camera.
            if (a.w <= kNearW || b.w <= kNearW) {
                const f32 t = (kNearW - a.w) / (b.w - a.w);
                const Vec4 c = a + (b - a) * t;
                (a.w <= kNearW ? a : b) = c;
            }
            dl->AddLine(ToScreen(a, origin, size), ToScreen(b, origin, size), line.color, 1.5f);
            ++lines_drawn_;
        }
        for (const DebugText& text : DebugDrawList::Get().Texts()) {
            ImVec2 at;
            if (text.screen) at = ImVec2(origin.x + text.position.x, origin.y + text.position.y);
            else if (!ProjectToScreen(view_proj, text.position, origin, size, at)) continue;
            else at.x -= ImGui::CalcTextSize(text.text.c_str()).x * 0.5f; // centred on the point
            dl->AddText(ImVec2(at.x + 1, at.y + 1), IM_COL32(0, 0, 0, 200), text.text.c_str());
            dl->AddText(at, text.color, text.text.c_str());
            ++texts_drawn_;
        }
    }
    if (show_stats) {
        const auto groups = StatGroups::Get().Collect();
        if (!groups.empty()) {
            const f32 line_h = ImGui::GetTextLineHeight() + 1.0f;
            f32 width = 0.0f, height = 6.0f;
            for (const auto& [name, lines] : groups) {
                height += line_h * static_cast<f32>(lines.size() + 1) + 4.0f;
                width = std::max(width, ImGui::CalcTextSize(name.c_str()).x);
                for (const StatLine& l : lines) width = std::max(width, ImGui::CalcTextSize(l.text.c_str()).x);
            }
            const ImVec2 box_min(max.x - width - 18.0f, origin.y + 6.0f);
            dl->AddRectFilled(box_min, ImVec2(max.x - 6.0f, box_min.y + height), IM_COL32(10, 10, 14, 190), 4.0f);
            f32 y = box_min.y + 4.0f;
            for (const auto& [name, lines] : groups) {
                dl->AddText(ImVec2(box_min.x + 6.0f, y), IM_COL32(150, 170, 255, 255), name.c_str());
                y += line_h;
                for (const StatLine& l : lines) {
                    dl->AddText(ImVec2(box_min.x + 6.0f, y), l.color, l.text.c_str());
                    y += line_h;
                    ++stat_lines_drawn_;
                }
                y += 4.0f;
            }
        }
    }
    dl->PopClipRect();
}

} // namespace aether::editor
