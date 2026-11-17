#include "devtools/console_panel.h"
#include "test_framework.h"

#include <imgui.h>

#include <functional>

// Phase 23 step 1: the console panel - submitting, history, Tab completion
// and its suggestion list, and drawing (docked and as an overlay) headless.

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
        body();
        ImGui::Render();
    }

private:
    ImGuiContext* context_ = nullptr;
};
} // namespace

AETHER_TEST(ConsolePanel_InputHistoryAndCompletion) {
    CVarRegistry reg;
    CVar* cascades = reg.Register("r.shadows.cascades", CVarType::Int, "4", "Cascades", CVar_None, 1, 4);
    reg.Register("r.shadows.quality", CVarType::String, "high", "Quality");
    Console console(reg);
    ConsolePanel panel(console);

    panel.input = "r.shadows.cascades 2";
    panel.Submit(panel.input);
    CHECK(cascades->GetInt() == 2 && panel.input.empty());
    panel.Submit("echo two");
    panel.HistoryUp();
    CHECK(panel.input == "echo two");
    panel.HistoryUp();
    CHECK(panel.input == "r.shadows.cascades 2");
    panel.HistoryUp(); // stays at the oldest
    CHECK(panel.input == "r.shadows.cascades 2");
    panel.HistoryDown();
    CHECK(panel.input == "echo two");
    panel.HistoryDown(); // past the newest: empty again
    CHECK(panel.input.empty());

    panel.input = "r.sh";
    panel.TabComplete();
    CHECK(panel.input == "r.shadows." && panel.Suggestions().empty());
    panel.TabComplete(); // nothing more to add: remembers
    CHECK(panel.Suggestions().empty());
    panel.TabComplete(); // twice: lists them
    CHECK(panel.Suggestions() == (std::vector<std::string>{"r.shadows.cascades", "r.shadows.quality"}));
    panel.input = "r.shadows.q";
    panel.TabComplete();
    CHECK(panel.input == "r.shadows.quality " && panel.Suggestions().empty());
}

AETHER_TEST(ConsolePanel_DrawsHeadless) {
    HeadlessImGui imgui;
    CVarRegistry reg;
    reg.Register("r.gamma", CVarType::Float, "2.2", "Gamma");
    Console console(reg);
    ConsolePanel panel(console);
    console.Execute("r.gamma");
    console.Execute("nope");
    panel.filter = "gamma";
    for (int i = 0; i < 3; ++i) {
        imgui.Frame([&] {
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(800, 400));
            ImGui::Begin("Console");
            panel.Draw();
            ImGui::End();
        });
    }
    // The overlay: the toggle opens and closes it.
    CHECK(!panel.open);
    imgui.Frame([&] { panel.Overlay(true, 1200); });
    CHECK(panel.open);
    imgui.Frame([&] { panel.Overlay(false, 1200); });
    CHECK(panel.open);
    imgui.Frame([&] { panel.Overlay(true, 1200); });
    CHECK(!panel.open && console.Output().size() >= 4);
}

// The PIE console: severity and category filters, repeated lines collapsed to one row with a count, and an output cleared
// at the start of every play session.
AETHER_TEST(ConsolePanel_FiltersCollapsesAndStartsEachPlayClean) {
    CVarRegistry reg;
    Console console(reg);
    ConsolePanel panel(console);
    console.CaptureLog(true);
    Logger::Instance().Log(LogLevel::Info, "Editor", "ready");
    Logger::Instance().Log(LogLevel::Warn, "Physics", "slow");
    Logger::Instance().Log(LogLevel::Warn, "Physics", "slow");
    Logger::Instance().Log(LogLevel::Error, "Renderer", "gone");
    console.Pump();
    console.CaptureLog(false); // flushes what is left

    auto visible = panel.VisibleLines();
    CHECK(visible.size() == 3);
    CHECK(visible[1].count == 2 && visible[1].line->category == "Physics");
    panel.collapse_repeats = false;
    CHECK(panel.VisibleLines().size() == 4);
    panel.collapse_repeats = true;

    panel.show_warn = false;
    visible = panel.VisibleLines();
    CHECK(visible.size() == 2 && visible[0].line->text == "Editor: ready" && visible[1].line->text == "Renderer: gone");
    panel.show_warn = true;

    panel.category = "Physics";
    visible = panel.VisibleLines();
    CHECK(visible.size() == 1 && visible[0].count == 2);
    CHECK(panel.Categories() == (std::vector<std::string>{"Editor", "Physics", "Renderer"}));
    panel.category.clear();

    panel.filter = "go";
    CHECK(panel.VisibleLines().size() == 1);
    panel.filter.clear();

    HeadlessImGui imgui;
    auto frame = [&](bool playing) {
        imgui.Frame([&] {
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(800, 400));
            ImGui::Begin("Console");
            panel.Draw(playing);
            ImGui::End();
        });
    };
    frame(false);
    CHECK(console.Output().size() == 4);
    frame(true); // the first frame of the play session
    CHECK(console.Output().empty());
    console.Print("during play");
    frame(true);
    CHECK(console.Output().size() == 1); // not cleared again while it plays
    frame(false);
    frame(true); // a new play session
    CHECK(console.Output().empty());
    panel.clear_on_play = false;
    console.Print("keep me");
    frame(false);
    frame(true);
    CHECK(console.Output().size() == 1);
}
