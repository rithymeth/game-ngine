#include "devtools/debug_overlay.h"
#include "test_framework.h"

#include <imgui.h>

#include <cmath>

// Phase 23 step 3: the debug overlay - projection, lines clipped at the
// near plane, world and screen text, and stat groups, drawn headless.

using namespace aether;
using namespace aether::editor;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

AETHER_TEST(DebugOverlay_ProjectsAndDraws) {
    // A camera at z = 10 looking at the origin.
    const Mat4 view = Mat4::LookAtRH(Vec3(0, 0, 10), Vec3(0, 0, 0), Vec3(0, 1, 0));
    const Mat4 proj = Mat4::PerspectiveRH(1.0f, 800.0f / 600.0f, 0.1f, 100.0f);
    const Mat4 vp = proj * view;
    ImVec2 at;
    CHECK(ProjectToScreen(vp, Vec3(0, 0, 0), ImVec2(100, 50), ImVec2(800, 600), at));
    CHECK(std::fabs(at.x - 500.0f) < 0.5f && std::fabs(at.y - 350.0f) < 0.5f); // the centre
    CHECK(ProjectToScreen(vp, Vec3(0, 1, 0), ImVec2(0, 0), ImVec2(800, 600), at) && at.y < 300.0f); // up is up
    CHECK(!ProjectToScreen(vp, Vec3(0, 0, 20), ImVec2(0, 0), ImVec2(800, 600), at));                 // behind

    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::GetIO().ConfigMacOSXBehaviors = false; // tests press Ctrl on every platform
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(800, 600);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);

    DebugDrawList& d = DebugDrawList::Get();
    d.Clear();
    d.enabled = true;
    d.Line(Vec3(-1, 0, 0), Vec3(1, 0, 0), 0xFFFFFFFFu);
    d.Line(Vec3(0, 0, 0), Vec3(0, 0, 30), 0xFFFFFFFFu);  // half behind: clipped
    d.Line(Vec3(0, 0, 20), Vec3(0, 1, 30), 0xFFFFFFFFu); // all behind: skipped
    d.Text(Vec3(0, 2, 0), "world", 0xFFFFFFFFu);
    d.Text(Vec3(0, 0, 50), "behind", 0xFFFFFFFFu);
    d.ScreenText("screen", 0xFFFFFFFFu);
    StatGroups::Get().HideAll();
    StatGroups::Get().Register("test.overlay", "", [] { return std::vector<StatLine>{{"a", kStatGood}, {"b", kStatBad}}; });
    StatGroups::Get().Show("test.overlay", true);

    DebugOverlay overlay;
    ImGui::NewFrame();
    overlay.Draw(ImGui::GetForegroundDrawList(), vp, ImVec2(0, 0), ImVec2(800, 600));
    ImGui::Render();
    CHECK(overlay.LinesDrawn() == 2 && overlay.TextsDrawn() == 2 && overlay.StatLinesDrawn() == 2);
    overlay.show_debug_draw = false;
    overlay.show_stats = false;
    ImGui::NewFrame();
    overlay.Draw(ImGui::GetForegroundDrawList(), vp, ImVec2(0, 0), ImVec2(800, 600));
    ImGui::Render();
    CHECK(overlay.LinesDrawn() == 0 && overlay.TextsDrawn() == 0 && overlay.StatLinesDrawn() == 0);
    ImGui::DestroyContext(ctx);
    StatGroups::Get().Unregister("test.overlay");
    d.Clear();
}
