#pragma once

#include "aether/dev/profiler.h"

#include <functional>
#include <string>
#include <vector>

// The editor's Profiler panel (Phase 23 step 2). Shows frame-time graph,
// per-zone timing and memory-by-category from aether::dev::Profiler. Portable
// (ImGui + engine only) so it runs headless in tests like the rest of the
// editor_ui library; the editor's main loop installs, drives and reads the
// profiler, this panel just draws it.

namespace aether::editor {

class ProfilerPanel {
public:
    explicit ProfilerPanel(dev::Profiler& profiler);
    void Draw();          // fills the current ImGui window
    bool show = false;    // toggled from the View menu

private:
    dev::Profiler& profiler_;
    usize history_ = 300;
    bool paused_ = false;
    f32 scale_ = 1.0f; // ms-per-pixel vertical scale
};

} // namespace aether::editor
