#include "test_framework.h"
#include "ui/script_inspector.h"

#include <imgui.h>

#include <functional>

using namespace aether;

namespace {

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1024, 768);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(context_); }
    void Frame(const std::function<void()>& body) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(600, 700));
        ImGui::Begin("Inspector");
        body();
        ImGui::End();
        ImGui::Render();
    }

private:
    ImGuiContext* context_ = nullptr;
};

ScriptClassInfo SpinInfo() {
    ScriptClassInfo info;
    info.ok = true;
    ExposedVariable label;
    label.name = "label";
    label.kind = ExposedVariable::Kind::String;
    label.default_value = "spinner";
    ExposedVariable speed;
    speed.name = "speed";
    speed.kind = ExposedVariable::Kind::Number;
    speed.default_value = 180.0;
    speed.has_range = true;
    speed.range_max = 720.0;
    ExposedVariable up;
    up.name = "up";
    up.kind = ExposedVariable::Kind::Vector;
    up.default_value = nlohmann::json::array({0.0, 1.0, 0.0});
    info.variables = {label, speed, up};
    return info;
}

// Types into the focusable widget `widget` and presses Enter; true if any
// frame reported a change.
bool TypeInto(HeadlessImGui& ui, const ScriptClassInfo& info, ScriptComponent& component, int widget, const char* text) {
    bool changed = false;
    auto frame = [&](bool focus) {
        ui.Frame([&] {
            if (focus) {
                ImGui::SetKeyboardFocusHere(widget);
            }
            changed |= editor::InspectScriptVariables(info, component);
        });
    };
    frame(true);  // activating the field selects its text (AutoSelectAll)...
    frame(false);
    ImGui::GetIO().AddInputCharactersUTF8(text); // ...so typing replaces it
    frame(false);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    frame(false);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    frame(false);
    return changed;
}

} // namespace

AETHER_TEST(ScriptInspector_EditsBecomeOverrides) {
    HeadlessImGui ui;
    const ScriptClassInfo info = SpinInfo();
    ScriptComponent component;
    bool changed = false;
    for (int i = 0; i < 2; ++i) {
        ui.Frame([&] { changed |= editor::InspectScriptVariables(info, component); });
    }
    AETHER_CHECK(!changed && component.properties.empty()); // drawing alone changes nothing

    // Typing a new label overrides it.
    AETHER_CHECK(TypeInto(ui, info, component, 0, "fast"));
    AETHER_CHECK(component.properties.size() == 1 && component.FindProperty("label")->value == "\"fast\"");
    // Typing the default back removes the override.
    AETHER_CHECK(TypeInto(ui, info, component, 0, "spinner"));
    AETHER_CHECK(component.properties.empty());

    // An existing override is shown, and survives drawing.
    component.SetProperty("speed", 360.0);
    ui.Frame([&] { changed = editor::InspectScriptVariables(info, component); });
    AETHER_CHECK(!changed && component.FindProperty("speed")->value == "360.0");
    // A stored value of the wrong type falls back to showing the default.
    component.SetProperty("up", "sideways");
    ui.Frame([&] { editor::InspectScriptVariables(info, component); });

    // Errors and empty scripts draw a message instead.
    ScriptClassInfo broken;
    broken.error = "Spin.luau:3: oops";
    ScriptClassInfo empty;
    empty.ok = true;
    ui.Frame([&] {
        AETHER_CHECK(!editor::InspectScriptVariables(broken, component, "a"));
        AETHER_CHECK(!editor::InspectScriptVariables(empty, component, "b"));
    });
}
