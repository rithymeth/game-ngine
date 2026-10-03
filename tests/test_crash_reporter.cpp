#include "aether/debug/crash.h"
#include "devtools/crash_reporter.h"
#include "test_framework.h"

#include <imgui.h>

#include <filesystem>

// Phase 23 step 4: the crash reporter dialog - opening for unseen
// reports, the report text, dismissing and deleting, and drawing headless.

using namespace aether;
using namespace aether::editor;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

AETHER_TEST(CrashReporter_ShowsAndDismissesReports) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_test_crash_reporter";
    std::filesystem::remove_all(dir);
    CrashReporterDialog empty(dir.string());
    empty.Refresh();
    CHECK(!empty.open && empty.Reports().empty());

    CrashConfig config;
    config.directory = dir.string();
    CHECK(CrashHandler::Install(config));
    CrashHandler::SetContext("map", "Docks");
    for (const char* reason : {"first", "second", "third"}) CrashHandler::WriteReport(reason);
    CrashHandler::ClearContext();
    CrashHandler::Uninstall();

    CrashReporterDialog dialog(dir.string());
    dialog.Refresh();
    CHECK(dialog.open && dialog.Reports().size() == 3);
    CHECK(dialog.ReportText(0).rfind("Aether crash report", 0) == 0 && dialog.ReportText(0).find("context.map: Docks") != std::string::npos);
    CHECK(dialog.ReportText(9).empty());

    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1200, 800);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    for (int i = 0; i < 2; ++i) {
        ImGui::NewFrame();
        dialog.Draw();
        ImGui::Render();
    }

    dialog.selected = 2;
    CHECK(dialog.Dismiss(0) && dialog.Reports().size() == 2 && dialog.selected == 1);
    CHECK(dialog.Delete(1) && dialog.Reports().size() == 1 && dialog.open);
    CHECK(!dialog.Dismiss(5));
    dialog.DismissAll();
    CHECK(!dialog.open && dialog.Reports().empty());
    ImGui::NewFrame();
    dialog.Draw(); // closed: nothing
    ImGui::Render();
    ImGui::DestroyContext(ctx);

    dialog.Refresh();
    CHECK(!dialog.open); // the rest were dismissed
    CHECK(ListCrashReports(dir.string(), true).size() == 2); // seen, not deleted
    std::filesystem::remove_all(dir);
}
