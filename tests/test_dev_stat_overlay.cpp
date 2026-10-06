// Headless tests for the portable stat overlay HUD (editor/src/dev). A real
// Dear ImGui context with no window or renderer, driven by the same pattern
// as test_editor_ui.cpp; pure-value tests for FormatBytes; and an all-flags-off
// call that must return false without even touching ImGui.

#include "dev/stat_overlay.h"
#include "test_framework.h"

#include <imgui.h>

#include <functional>
#include <string>

using namespace aether;
using namespace aether::editor;

namespace {

// Owns an ImGui context for one test. No platform or renderer backend: the
// font atlas is built on the CPU and draw data is generated but never drawn.
class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1024, 768);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(context_); }

    void Frame(const std::function<void()>& body) {
        ImGui::NewFrame();
        body();
        ImGui::Render();
    }

private:
    ImGuiContext* context_ = nullptr;
};

} // namespace

AETHER_TEST(StatOverlay_FormatBytes) {
    AETHER_CHECK(detail::FormatBytes(0) == "0 B");
    AETHER_CHECK(detail::FormatBytes(1024) == "1.0 KB");
    AETHER_CHECK(detail::FormatBytes(1536) == "1.5 KB");
    AETHER_CHECK(detail::FormatBytes(5 * 1024 * 1024) == "5.0 MB");
    AETHER_CHECK(detail::FormatBytes(2LL * 1024 * 1024 * 1024) == "2.0 GB");
    AETHER_CHECK(detail::FormatBytes(3LL * 1024 * 1024 * 1024 * 1024) == "3.0 TB");
}

AETHER_TEST(StatOverlay_AllFlagsOffReturnsFalseWithoutImGui) {
    // Deliberately no context: exercises the early-out before any ImGui call.
    StatOverlayFlags flags;
    AETHER_CHECK(!flags.fps && !flags.gpu && !flags.memory);
    AETHER_CHECK(!DrawStatOverlay(flags, StatOverlayValues(), ImVec2(12, 12)));
}

AETHER_TEST(StatOverlay_DrawsHeadlessWithFlagsOn) {
    HeadlessImGui ui;
    StatOverlayValues values;
    values.fps = 123.4f;
    values.avg_ms = 11.8f;
    values.p99_ms = 22.1f;
    values.gpu_ms = 2.3f;
    values.draw_calls = 123;
    values.triangles = 45678;
    values.memory_bytes = 128 * 1024 * 1024 + 128 * 1024; // ~128.1 MB

    StatOverlayFlags flags; // fps on
    flags.fps = true;
    flags.gpu = true;
    flags.memory = true;

    bool clicked = false;
    ui.Frame([&] { clicked = DrawStatOverlay(flags, values); });
    AETHER_CHECK(!clicked); // no mouse input simulated
    AETHER_CHECK(ImGui::GetDrawData() != nullptr);
    AETHER_CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
    AETHER_CHECK(ImGui::GetDrawData()->TotalIdxCount > 0);
    AETHER_CHECK(ImGui::GetDrawData()->CmdListsCount > 0);
}

AETHER_TEST(StatOverlay_OffButGpuLineUnused) {
    // gpu flag off but gpu_ms provided - must not crash, returns false.
    HeadlessImGui ui;
    StatOverlayValues values;
    values.fps = 0.0f;
    values.gpu_ms = 1.5f;
    StatOverlayFlags flags;
    flags.fps = false;
    flags.gpu = false;
    flags.memory = false;
    bool clicked = false;
    ui.Frame([&] { clicked = DrawStatOverlay(flags, values); });
    AETHER_CHECK(!clicked);
    AETHER_CHECK(ImGui::GetDrawData()->TotalVtxCount == 0);
}
