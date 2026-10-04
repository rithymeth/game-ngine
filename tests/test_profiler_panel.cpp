#include "devtools/profiler_panel.h"
#include "test_framework.h"

#include <imgui.h>

#include <filesystem>
#include <functional>

// Phase 23 step 2: the profiler panel - frame selection, zone stats for a
// frame and for the history, export, and every tab drawn headless.

using namespace aether;
using namespace aether::editor;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {
class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGui::GetIO().ConfigMacOSXBehaviors = false; // tests press Ctrl on every platform
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1200, 800);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(context_); }
    void Frame(const std::function<void()>& body) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1200, 800));
        ImGui::Begin("Profiler");
        body();
        ImGui::End();
        ImGui::Render();
    }

private:
    ImGuiContext* context_ = nullptr;
};

void RecordFrames(Profiler& p, int count) {
    for (int i = 0; i < count; ++i) {
        p.BeginFrame();
        {
            AETHER_PROFILE_ZONE("Panel.Update");
            for (int k = 0; k <= i; ++k) {
                AETHER_PROFILE_ZONE("Panel.System");
            }
        }
        p.Count("Panel.Draws", i);
        p.SubmitGpuTimings({{"Main pass", 1.0 + i}});
        p.EndFrame();
    }
}
} // namespace

AETHER_TEST(ProfilerPanel_SelectionStatsAndDrawing) {
    Profiler& p = Profiler::Get();
    p.SetEnabled(true), p.SetPaused(false), p.history = 300;
    p.EndFrame();
    p.Clear();
    MemoryTracker::Get().Alloc("Panel.Textures", 4096);
    ProfilerPanel panel(p);
    HeadlessImGui imgui;
    imgui.Frame([&] { panel.Draw(); }); // no frames yet
    RecordFrames(p, 5);

    ProfileFrame f;
    CHECK(panel.SelectedFrame(f) && f.Counter("Panel.Draws") == 4.0); // follows the newest
    std::vector<ZoneStat> stats = panel.Stats();
    const auto calls = [&](const char* name) {
        for (const ZoneStat& s : stats)
            if (s.name == name) return s.calls;
        return 0u;
    };
    CHECK(calls("Panel.System") == 5 && calls("Panel.Update") == 1);
    panel.selected = 0;
    CHECK(panel.SelectedFrame(f) && f.Counter("Panel.Draws", -1) == 0.0); // the first frame
    stats = panel.Stats();
    CHECK(calls("Panel.System") == 1);
    panel.scope = ProfilerPanel::Scope::AllFrames;
    stats = panel.Stats();
    CHECK(calls("Panel.System") == 15 && calls("Panel.Update") == 5);
    panel.selected = 99; // out of range: the newest
    CHECK(panel.SelectedFrame(f) && f.Counter("Panel.Draws") == 4.0);

    for (int i = 0; i < 3; ++i) imgui.Frame([&] { panel.Draw(); });
    panel.export_path = (std::filesystem::temp_directory_path() / "aether_test_profiler_panel.json").string();
    CHECK(panel.Export() && panel.Status().find("Wrote 5 frames") == 0);
    std::filesystem::remove(panel.export_path);
    MemoryTracker::Get().Free("Panel.Textures", 4096);
    p.Clear();
}
