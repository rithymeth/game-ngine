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
