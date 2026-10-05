#include "test_framework.h"
#include "workspace/editor_workspace.h"

#include <imgui.h>

#include <cstring>
#include <functional>
#include <set>
#include <string>

// The editor workspace: every tool editor on a sample document, as the
// Windows editor and the portable shell host it, drawn headless.

using namespace aether;
using namespace aether::editor;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigMacOSXBehaviors = false;
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1600, 960);
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

AETHER_TEST(EditorWorkspace_HasEveryTool) {
    HeadlessImGui imgui;
    EditorWorkspace ws;
    CHECK(ws.ToolCount() >= 19);
    std::set<std::string> names, categories;
    for (usize i = 0; i < ws.ToolCount(); ++i) {
        names.insert(ws.ToolName(i));
        categories.insert(ws.ToolCategory(i));
    }
    CHECK(names.size() == ws.ToolCount()); // unique: they're window titles
    for (const char* c : {"Scripting", "Rendering", "Animation", "AI", "Audio", "UI", "World", "Networking", "Debug"}) {
        CHECK(categories.count(c) == 1);
    }
    for (const char* t : {"Blueprint", "Luau", "Material", "Particle", "Animation", "Behavior", "Navigation", "Sound Cue",
                          "Mixer", "UI Designer", "Terrain", "Foliage", "Spline", "World Partition", "Net Play",
                          "Net Profiler", "Console", "Profiler", "Crash", "Sequencer"}) {
        CHECK(ws.FindTool(t) >= 0);
    }
    CHECK(ws.FindTool("material") == ws.FindTool("Material - M_Lit")); // any case, a prefix
    CHECK(ws.FindTool("Gizmo") == -1 && ws.FindTool("") == -1);
}

AETHER_TEST(EditorWorkspace_DrawsEveryToolHeadless) {
    HeadlessImGui imgui;
    EditorWorkspace ws;
    // Each tool in the hub, a couple of frames apiece.
    for (usize i = 0; i < ws.ToolCount(); ++i) {
        ws.Select(i);
        CHECK(ws.Selected() == i);
        for (int f = 0; f < 2; ++f) {
            imgui.Frame([&] {
                ws.Update(1.0f / 60.0f);
                ws.DrawHub();
                ws.DrawWindows();
            });
        }
    }
    // Every tool popped out at once, with the menu bar's Tools menu.
    for (usize i = 0; i < ws.ToolCount(); ++i) ws.SetWindowOpen(i, true);
    for (int f = 0; f < 3; ++f) {
        imgui.Frame([&] {
            if (ImGui::BeginMainMenuBar()) {
                ws.DrawToolsMenu();
                ImGui::EndMainMenuBar();
            }
            ws.DrawHub();
            ws.DrawWindows();
        });
    }
    CHECK(ws.IsWindowOpen(0));
    ws.SetWindowOpen(0, false);
    CHECK(!ws.IsWindowOpen(0));
    ws.Select(ws.ToolCount() + 5); // out of range: ignored
    CHECK(ws.Selected() < ws.ToolCount());
}
