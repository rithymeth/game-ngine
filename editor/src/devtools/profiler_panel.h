#pragma once

#include "aether/core/profiler.h"

#include <string>
#include <vector>

namespace aether::editor {

// The profiler window (Phase 23 step 2): the frame-time graph of the kept
// frames (click one to inspect it; following the newest otherwise), and
// for the inspected frame - or the whole history - the zones by total and
// self time, a per-thread timeline, the counters and GPU pass times, and
// memory by category. Pause, Clear and a Chrome-trace export. Portable;
// draws into the current ImGui window.
class ProfilerPanel {
public:
    explicit ProfilerPanel(Profiler& profiler = Profiler::Get()) : profiler_(profiler) {}
    void Draw();

    enum class Scope { SelectedFrame, AllFrames };
    Scope scope = Scope::SelectedFrame;
    enum class Tab { Zones, Timeline, Counters, Memory };
    Tab tab = Tab::Zones;
    std::string export_path = "profile.json";

    // -1 follows the newest frame; otherwise an index into the kept frames.
    int selected = -1;
    // What the Zones tab lists, for the current scope and selection.
    std::vector<ZoneStat> Stats() const;
    // The frame the panel shows (false when there are none).
    bool SelectedFrame(ProfileFrame& out) const;
    const std::string& Status() const { return status_; }
    bool Export();

private:
    void DrawGraph(const std::vector<ProfileFrame>& frames);
    void DrawTimeline(const ProfileFrame& frame);
    Profiler& profiler_;
    std::string status_;
};

} // namespace aether::editor
