#pragma once

#include "aether/debug/debug_draw.h"
#include "aether/debug/stats.h"

#include <imgui.h>

namespace aether::editor {

// Projects a world point through `view_proj` into the screen rectangle at
// `origin` of `size` (y down). False when it's behind the camera.
bool ProjectToScreen(const Mat4& view_proj, const Vec3& p, ImVec2 origin, ImVec2 size, ImVec2& out);

// Draws what the game asked for (Phase 23 step 3) over a viewport with
// ImGui: debug lines (clipped at the near plane), world text at its
// projected position, screen text top-left, and the shown stat groups
// top-right on a dark backing. The renderer's debug pass can draw the
// lines with depth testing instead; this works anywhere ImGui does,
// including the editor's viewport and game builds.
class DebugOverlay {
public:
    void Draw(ImDrawList* draw_list, const Mat4& view_proj, ImVec2 origin, ImVec2 size);
    bool show_debug_draw = true;
    bool show_stats = true;
    usize LinesDrawn() const { return lines_drawn_; }
    usize TextsDrawn() const { return texts_drawn_; }
    usize StatLinesDrawn() const { return stat_lines_drawn_; }

private:
    usize lines_drawn_ = 0, texts_drawn_ = 0, stat_lines_drawn_ = 0;
};

} // namespace aether::editor
