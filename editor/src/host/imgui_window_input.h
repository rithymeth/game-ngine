#pragma once

#include "aether/input/keys.h"
#include "aether/platform/window.h"

#include <imgui.h>

#include <span>

namespace aether::editor {

// Feeds a frame's platform-neutral window events (aether::WindowEvent, the
// same from Win32 and GLFW) to Dear ImGui: keys and modifiers, mouse
// position, buttons and wheel, typed characters and focus (Phase 24,
// docs/design/PHASE_SPECS.md §24.4). It replaces a per-platform ImGui
// backend (imgui_impl_win32, imgui_impl_glfw), so any window the engine can
// open drives the editor UI the same way.
ImGuiKey ToImGuiKey(input::Key key);
void FeedImGuiEvents(std::span<const WindowEvent> events, ImGuiIO& io);

} // namespace aether::editor
