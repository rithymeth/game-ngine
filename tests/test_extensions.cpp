#include "test_framework.h"
#include "ui/editor_scripts.h"
#include "ui/extensions.h"
#include "ui/reflected_inspector.h"
#include "workspace/editor_workspace.h"

#include "aether/core/base.h"
#include "aether/math/math.h"
#include "aether/reflection/reflection.h"

#include <imgui.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

// The editor extensibility API (Phase 26 step 4, §26.5): panels, menu items,
// property drawers and asset types, from C++ and from Luau editor scripts.

using namespace aether;
using namespace aether::editor;
namespace stdfs = std::filesystem;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace ext_test {
struct Gadget {
    Vec3 offset{1, 2, 3};
    i32 count = 4;
};
} // namespace ext_test
AETHER_REFLECT(ext_test::Gadget, 1, AETHER_FIELD(offset, Field_EditAnywhere), AETHER_FIELD(count, Field_EditAnywhere))

namespace {

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigMacOSXBehaviors = false;
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1280, 800);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    ~HeadlessImGui() { ImGui::DestroyContext(context_); }
    void Frame(const std::function<void()>& body) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(900, 700));
        ImGui::Begin("Test");
        body();
        ImGui::End();
        ImGui::Render();
    }

private:
    ImGuiContext* context_ = nullptr;
};

stdfs::path TempFolder(const char* name) {
    std::error_code ec;
    const stdfs::path dir = stdfs::temp_directory_path(ec) / "aether_ext_test" / name;
    stdfs::remove_all(dir, ec);
    stdfs::create_directories(dir, ec);
    return dir;
}

void Write(const stdfs::path& file, const std::string& text) {
    std::error_code ec;
    stdfs::create_directories(file.parent_path(), ec);
    std::ofstream(file, std::ios::binary) << text;
}

} // namespace

AETHER_TEST(Extensions_PanelsMenusAndOwners) {
    ExtensionRegistry reg;
    const u64 v0 = reg.Version();
    int draws = 0;
    reg.AddPanel({"Stats", "", [&] { ++draws; }, "me"});
    reg.AddPanel({"Stats", "", [&] { draws += 10; }, "me"}); // same owner and name: replaced
    reg.AddPanel({"Other", "Tools X", {}, "you"});
    CHECK(reg.Panels().size() == 2);
    CHECK(reg.FindPanel("Stats")->category == "Extensions");
    CHECK(reg.FindPanel("Other")->category == "Tools X");
    CHECK(reg.Version() > v0);
    reg.FindPanel("Stats")->draw();
    CHECK(draws == 10);

    int fired = 0;
    reg.AddMenuItem({"Level/Bake/Lighting", "Ctrl+B", [&] { ++fired; }, {}, "me"});
    reg.AddMenuItem({"Level/Clear", "", [&] { fired += 100; }, [] { return false; }, "me"});
    CHECK(reg.TriggerShortcut("Ctrl+B"));
    CHECK(fired == 1);
    CHECK(!reg.TriggerShortcut("Ctrl+Z"));

    HeadlessImGui ui;
    for (int frame = 0; frame < 2; ++frame) {
        ImGui::NewFrame();
        if (ImGui::BeginMainMenuBar()) {
            reg.DrawMenus();
            ImGui::EndMainMenuBar();
        }
        reg.PollShortcuts(); // no keys down: nothing fires
        ImGui::Render();
    }
    CHECK(fired == 1);

    reg.RemoveOwner("me");
    CHECK(reg.Panels().size() == 1 && reg.Panels()[0].name == "Other");
    CHECK(reg.MenuItems().empty());
    reg.Clear();
    CHECK(reg.Panels().empty());
}

AETHER_TEST(Extensions_PropertyDrawerReplacesTheWidget) {
    HeadlessImGui ui;
    ExtensionRegistry reg;
    reg.MakeActive();
    ext_test::Gadget gadget;
    int calls = 0;
    // Without a drawer: the stock widgets, no change without input.
    ui.Frame([&] { CHECK(!InspectObject(gadget, "g").Changed()); });

    reg.AddPropertyDrawer({"Vec3", [&](void* value, const reflect::Meta&) {
                               ++calls;
                               Vec3* v = static_cast<Vec3*>(value);
                               ImGui::TextUnformatted("custom");
                               v->x = 9.0f;
                               return true;
                           },
                           "me"});
    InspectResult result;
    ui.Frame([&] { result = InspectObject(gadget, "g"); });
    CHECK(calls == 1);
    CHECK(gadget.offset.x == 9.0f);
    CHECK(result.Changed() && std::string(result.changed_field->name) == "offset");

    reg.RemoveOwner("me");
    calls = 0;
    ui.Frame([&] { InspectObject(gadget, "g"); });
    CHECK(calls == 0);
    // The registry going away clears the hook.
    {
        ExtensionRegistry other;
        other.MakeActive();
        CHECK(ExtensionRegistry::Active() == &other);
    }
    CHECK(ExtensionRegistry::Active() == nullptr);
}

AETHER_TEST(Extensions_AssetTypes) {
    ExtensionRegistry reg;
    stdfs::path opened;
    reg.AddAssetType({"Dialogue", "DIALOGUE", "{}\n", [&](const stdfs::path& f) { opened = f; }, "me"});
    CHECK(reg.AssetTypes()[0].extension == ".dialogue"); // normalised
    const ExtensionAssetType* type = reg.FindAssetTypeForPath("Content/Story/intro.DIALOGUE");
    CHECK(type != nullptr && type->name == "Dialogue");
    CHECK(reg.FindAssetTypeByExtension(".png") == nullptr);

    const stdfs::path dir = TempFolder("assets");
    const stdfs::path a = reg.CreateAsset(*type, dir);
    const stdfs::path b = reg.CreateAsset(*type, dir);
    CHECK(a.filename() == "New Dialogue.dialogue" && b.filename() == "New Dialogue 1.dialogue");
    CHECK(stdfs::file_size(a) == 3);

    CHECK(reg.OpenAsset(b));
    CHECK(opened == b);
    CHECK(!reg.OpenAsset(dir / "x.txt"));
}

AETHER_TEST(Extensions_WorkspaceShowsPanelsAsTools) {
    HeadlessImGui ui;
    EditorWorkspace ws;
    ws.Update(0.0f); // the sample project's own editor script panel joins first
    const usize built_in = ws.ToolCount();
    int draws = 0;
    ws.Extensions().AddPanel({"My Panel", "", [&] { ++draws; ImGui::TextUnformatted("hi"); }, "test"});
    ws.Update(0.0f);
    CHECK(ws.ToolCount() == built_in + 1);
    const i64 tool = ws.FindTool("My Panel");
    CHECK(tool == static_cast<i64>(built_in));
    CHECK(std::string(ws.ToolCategory(static_cast<usize>(tool))) == "Extensions");

    ws.SetWindowOpen(static_cast<usize>(tool), true);
    ui.Frame([&] {
        ws.DrawToolsMenu();
        ws.DrawWindows();
    });
    CHECK(draws == 1);

    ws.Extensions().AddPanel({"Second", "", [] {}, "test"}); // the open window survives a rebuild
    ws.Update(0.0f);
    CHECK(ws.IsWindowOpen(static_cast<usize>(ws.FindTool("My Panel"))));
    ws.Extensions().RemoveOwner("test");
    ws.Update(0.0f);
    CHECK(ws.ToolCount() == built_in);
    CHECK(ws.FindTool("My Panel") == -1);
}

#ifdef AETHER_TEST_HAS_SCRIPTING
AETHER_TEST(EditorScripts_RegisterPanelsMenusAndAssetTypes) {
    CHECK(EditorScripts::Available());
    HeadlessImGui ui;
    ExtensionRegistry reg;
    EditorScripts scripts(reg);
    const stdfs::path dir = TempFolder("scripts");
    Write(dir / "good.luau", R"(
local on, n, text = false, 1.5, "x"
editor.AddPanel("Scripted", function()
    ui.Text("hello")
    ui.TextDisabled("dim")
    if ui.Button("Go") then editor.Log("go") end
    ui.SameLine()
    on = ui.Checkbox("On", on)
    n = ui.SliderFloat("N", n, 0, 4)
    text = ui.InputText("T", text)
    if ui.CollapsingHeader("More") then ui.Separator() ui.Spacing() end
end)
editor.AddMenuItem("Tools/Scripted/Make", function()
    editor.AddPanel("Made by menu", function() ui.Text("made") end)
end, "Ctrl+Alt+M")
editor.AddAssetType("Quest", ".quest", "{}", function(path)
    editor.AddPanel("Opened " .. path, function() end)
end)
)");
    Write(dir / "sub" / "broken_load.luau", "this is not luau (");
    Write(dir / "broken_panel.luau", R"(
editor.AddPanel("Broken", function() local x = nil; x.y = 1 end)
editor.AddPanel(42, nil)
)");
    CHECK(scripts.Load(dir) == 1);
    CHECK(scripts.ScriptCount() == 1);
    CHECK(reg.FindPanel("Scripted") != nullptr);
    CHECK(reg.FindPanel("Broken") == nullptr); // its script failed part way, so it left nothing behind
    CHECK(reg.AssetTypes().size() == 1);
    bool saw_load_error = false, saw_bad_args = false;
    for (const std::string& e : scripts.Errors()) {
        if (e.rfind("sub/broken_load.luau", 0) == 0) saw_load_error = true;
        if (e.find("editor.AddPanel(name, function)") != std::string::npos) saw_bad_args = true;
    }
    CHECK(saw_load_error && saw_bad_args);

    ui.Frame([&] { reg.FindPanel("Scripted")->draw(); }); // every widget draws
    CHECK(scripts.Errors().size() == 2);

    // A menu item and an asset opener run Luau, which registers more.
    CHECK(reg.TriggerShortcut("Ctrl+Alt+M"));
    CHECK(reg.FindPanel("Made by menu") != nullptr);
    CHECK(reg.OpenAsset("Content/a.quest"));
    CHECK(reg.FindPanel("Opened Content/a.quest") != nullptr);
    const ExtensionAssetType* quest = reg.FindAssetTypeByExtension(".quest");
    CHECK(quest != nullptr && quest->new_text == "{}");

    // Reload: edits take effect, the old registrations go.
    std::error_code ec;
    stdfs::remove(dir / "broken_panel.luau", ec);
    stdfs::remove(dir / "sub" / "broken_load.luau", ec);
    Write(dir / "good.luau", "editor.AddPanel(\"Renamed\", function() error(\"boom\") end)\n");
    CHECK(scripts.Reload() == 1);
    CHECK(reg.FindPanel("Scripted") == nullptr && reg.FindPanel("Made by menu") == nullptr);
    CHECK(reg.FindPanel("Renamed") != nullptr);
    CHECK(scripts.Errors().empty());
    ui.Frame([&] { reg.FindPanel("Renamed")->draw(); }); // a runtime error shows in the panel, and is reported
    CHECK(scripts.Errors().size() == 1 && scripts.Errors()[0].find("boom") != std::string::npos);

    scripts.Unload();
    CHECK(reg.Panels().empty() && scripts.ScriptCount() == 0);
}

AETHER_TEST(EditorScripts_RunawayScriptIsStopped) {
    ExtensionRegistry reg;
    EditorScripts scripts(reg);
    const stdfs::path dir = TempFolder("runaway");
    Write(dir / "loop.luau", "while true do end\n");
    Write(dir / "ok.luau", "editor.AddMenuItem(\"Tools/Ok\", function() while true do end end, \"Ctrl+K\")\n");
    CHECK(scripts.Load(dir) == 1);
    CHECK(scripts.Errors().size() == 1);
    CHECK(reg.TriggerShortcut("Ctrl+K")); // the action's own loop hits the budget, not a hang
    CHECK(scripts.Errors().size() == 2);
}

AETHER_TEST(EditorScripts_WorkspaceLoadsTheSampleProjectsScript) {
    HeadlessImGui ui;
    EditorWorkspace ws;
    ws.Update(0.0f);
    CHECK(ws.FindTool("Hello Panel") >= 0);
    CHECK(ws.Extensions().FindAssetTypeByExtension(".dialogue") != nullptr);
    ui.Frame([&] {
        ws.SetWindowOpen(static_cast<usize>(ws.FindTool("Hello Panel")), true);
        ws.DrawToolsMenu();
        ws.DrawWindows();
    });
    CHECK(ws.Scripts().Errors().empty());
}
#endif
