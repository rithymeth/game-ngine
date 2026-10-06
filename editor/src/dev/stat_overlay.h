#pragma once

#include "aether/core/base.h"

#include <imgui.h>

#include <string>

// Phase 23 step: the in-viewport stat overlay HUD (FPS / GPU / memory).
// Portable: built only on Dear ImGui (imgui_core) and the engine's reflection
// base types, so it builds and runs headless in tests like the rest of the
// aether_editor_ui library. It keeps no state and only touches ImGui after
// the early-out below, so calling it with every flag off is safe even with no
// ImGui context.

namespace aether::editor {

// Which stat groups the user has enabled (driven by CVars `stat.fps`,
// `stat.gpu`, `stat.memory` from cvars::GetBool; defaults off).
struct StatOverlayFlags {
    bool fps    = false;
    bool gpu    = false;
    bool memory = false;
};

// Draws a small HUD box in the top-left-ish corner of the viewport (e.g. over
// the 3D scene, anchored at `anchor` upper-left in screen coords). Values come
// from the caller — the overlay itself is deliberately dumb/portable:
//
//   frame_ms      current/last frame time (ms)
//   fps           frames per second (float)
//   avg_ms        average frame ms over the last N frames
//   p99_ms        99th percentile frame ms
//   gpu_ms        last GPU render-pass time, or <0 if unknown (omit line)
//   draw_calls    draw calls this frame (omit if <0)
//   triangles     triangle count this frame (omit if <0)
//   memory_bytes  total engine memory currently tracked, or <0 to omit
//
// Returns whether the user clicked anywhere on the box (not used by the
// editor, kept for future wiring).
struct StatOverlayValues {
    f32 frame_ms = 0.0f;
    f32 fps      = 0.0f;
    f32 avg_ms   = 0.0f;
    f32 p99_ms   = 0.0f;
    f32 gpu_ms   = -1.0f;
    f32 draw_calls  = -1.0f;   // <0 = not shown
    f32 triangles   = -1.0f;
    i64 memory_bytes = -1;     // <0 = not shown
};

namespace detail {

// Formats a byte count as "0 B", "1.0 KB", "1.5 MB", "2.3 GB", one decimal
// place, up to TB. Exposed for the headless unit tests.
std::string FormatBytes(i64 bytes);

} // namespace detail

// Draws the stat HUD anchored at `anchor` (default top-left at viewport
// origin + {12,12}). Returns true if the user clicked anywhere on the box.
bool DrawStatOverlay(const StatOverlayFlags& flags, const StatOverlayValues& values, ImVec2 anchor = ImVec2(12, 12));

} // namespace aether::editor
