#include "packaging/build_window.h"
#include "packaging/plugins_panel.h"
#include "test_framework.h"
#include "workspace/editor_workspace.h"

#include <imgui.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <thread>

// Phase 25 step 5: the Build and Package window (cook, package, launch the
// packaged game) and the Project Settings panel, drawn headless.

using namespace aether;
using namespace aether::editor;
namespace stdfs = std::filesystem;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

class HeadlessImGui {
public:
    HeadlessImGui() {
        context_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigMacOSXBehaviors = false;
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
        ImGui::Begin("Build", nullptr, ImGuiWindowFlags_NoMove);
        body();
        ImGui::End();
        ImGui::Render();
    }

private:
    ImGuiContext* context_ = nullptr;
};

// A fresh copy of the workspace's sample project.
stdfs::path FreshProject(const std::string& name) {
    const stdfs::path sample = EditorWorkspace::SampleProject();
    AETHER_CHECK(!sample.empty());
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_build_window_tests" / name;
    stdfs::remove_all(dir);
    stdfs::create_directories(dir.parent_path());
    stdfs::copy(sample.parent_path(), dir, stdfs::copy_options::recursive);
    return dir / sample.filename();
}

} // namespace

AETHER_TEST(BuildWindow_PackagesAndLaunchesTheGame) {
    HeadlessImGui imgui;
    BuildPackageWindow window(FreshProject("Package"));
    window.player_path = AETHER_TEST_PLAYER_PATH;
    window.configuration = cook::BuildConfiguration::Shipping;
    CHECK(window.OutputDir() == ProjectPaths::ForFile(window.ProjectFile()).saved / "Packaged" / "Shipping");
    std::string error;
    CHECK(!window.Launch(&error) && error.find("Package the game first") == 0);

    CHECK(window.StartPackage());
    while (window.Busy()) {
        imgui.Frame([&] { window.Draw(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    window.WaitForBuild();
    imgui.Frame([&] { window.Draw(); });
    CHECK(window.LastResult() == BuildPackageWindow::Result::Succeeded && window.Progress() == 1.0f);
    const stdfs::path game = window.StagedGame();
    CHECK(stdfs::is_regular_file(game) && stdfs::exists(game.parent_path() / "Paks" / "Game.apak"));
    bool cooked_scene = false;
    for (const BuildPackageWindow::LogLine& line : window.Log()) cooked_scene |= line.text == "Cooking Scenes/Main.ascene";
    CHECK(cooked_scene);

    // The packaged game runs on its own: it finds Paks/ beside itself.
    window.launch_args = "--headless --frames 10";
    CHECK(window.Launch(&error));
    int code = -1;
    CHECK(window.Game().Wait(&code) && code == 0);
    imgui.Frame([&] { window.Draw(); });
}

AETHER_TEST(BuildWindow_CooksAndReportsFailures) {
    HeadlessImGui imgui;
    const stdfs::path project = FreshProject("Cook");
    BuildPackageWindow window(project);
    CHECK(window.StartCook());
    window.WaitForBuild();
    CHECK(window.LastResult() == BuildPackageWindow::Result::Succeeded && !window.Busy());
    CHECK(stdfs::exists(ProjectPaths::ForFile(project).saved / "Cooked" / "Development" / "Game.apak"));
    CHECK(window.StagedGame().empty()); // a cook stages no game

    // No startup scene: the cook fails, and says why.
    ProjectSettingsPanel settings(project);
    settings.Settings().startup_scene.clear();
    CHECK(settings.Save());
    CHECK(window.StartCook());
    window.WaitForBuild();
    imgui.Frame([&] { window.Draw(); });
    CHECK(window.LastResult() == BuildPackageWindow::Result::Failed && window.Stage().find("Nothing to cook") != std::string::npos);

    // A missing player fails the package.
    CHECK(settings.Reload());
    settings.Settings().startup_scene = "Scenes/Main.ascene";
    CHECK(settings.Save());
    window.player_path = (project.parent_path() / "missing_player").string();
    CHECK(window.StartPackage());
    window.WaitForBuild();
    CHECK(window.LastResult() == BuildPackageWindow::Result::Failed && window.Stage().find("No player at") != std::string::npos);
}

AETHER_TEST(ProjectSettingsPanel_EditsSavesAndReverts) {
    HeadlessImGui imgui;
    const stdfs::path project = FreshProject("Settings");
    ProjectSettingsPanel panel(project);
    CHECK(panel.Settings().startup_scene == "Scenes/Main.ascene" && panel.Settings().window_title == "Sample Game");
    for (int i = 0; i < 2; ++i) imgui.Frame([&] { panel.Draw(); });
    CHECK(!panel.Dirty());
    panel.Settings().window_width = 1600;
    CHECK(panel.Save() && panel.Status() == "Saved");
    ProjectSettings saved;
    CHECK(LoadProject(project, saved, nullptr) && saved.window_width == 1600);
    panel.Settings().default_quality = "Missing"; // drawn with a warning
    imgui.Frame([&] { panel.Draw(); });
    CHECK(panel.Reload() && panel.Settings().default_quality == "High");
}

AETHER_TEST(BuildWindow_PackagesEncryptedAndLaunchesWithTheKey) {
    BuildPackageWindow window(FreshProject("Encrypted"));
    window.player_path = AETHER_TEST_PLAYER_PATH;
    window.encrypt = true;
    window.encryption_key = "not a key";
    CHECK(!window.StartPackage() && window.LastResult() == BuildPackageWindow::Result::Failed);
    window.encryption_key = pak::PakKey::Generate().ToHex();
    CHECK(window.StartPackage());
    window.WaitForBuild();
    CHECK(window.LastResult() == BuildPackageWindow::Result::Succeeded);
    pak::PakReader reader;
    CHECK(!reader.Open((window.StagedGame().parent_path() / "Paks" / "Game.apak").string()));

    // The game gets the key on its command line; without it, it can't start.
    window.launch_args = "--headless --frames 5";
    std::string error;
    CHECK(window.Launch(&error));
    int code = -1;
    CHECK(window.Game().Wait(&code) && code == 0);
    window.encrypt = false;
    CHECK(window.Launch(&error));
    CHECK(window.Game().Wait(&code) && code != 0);
}

AETHER_TEST(PluginsPanel_EnablesCreatesAndRefuses) {
    HeadlessImGui imgui;
    const stdfs::path project = FreshProject("Plugins");
    PluginsPanel panel(project);
    CHECK(panel.Error().empty() && panel.Manager().Find("AI") && panel.Manager().Find("AI")->enabled);
    for (int i = 0; i < 2; ++i) imgui.Frame([&] { panel.Draw(); });

    // A default plugin can't be turned off from the project.
    std::string error;
    CHECK(!panel.SetEnabled("Navigation", false, &error) && error.find("default") != std::string::npos);

    // A new project plugin, enabled: saved in the project.
    CHECK(panel.CreatePlugin("Lanterns", &error) && panel.Manager().Find("Lanterns") && !panel.Manager().Find("Lanterns")->enabled);
    CHECK(panel.SetEnabled("Lanterns", true, &error) && panel.Manager().Find("Lanterns")->enabled);
    ProjectSettings saved;
    CHECK(LoadProject(project, saved, nullptr) && saved.plugins == std::vector<std::string>{"Lanterns"});
    CHECK(!panel.CreatePlugin("Lanterns", &error) && error.find("already exists") != std::string::npos);

    // One whose dependency is missing doesn't get enabled; the project stays as it was.
    plugin::PluginDescriptor needy;
    needy.name = "Needy";
    needy.dependencies.push_back({"Nowhere", "", false});
    CHECK(plugin::SavePluginDescriptor(project.parent_path() / "Plugins/Needy/Needy.aplugin", needy));
    CHECK(panel.Refresh());
    CHECK(!panel.SetEnabled("Needy", true, &error) && error.find("Nowhere") != std::string::npos);
    CHECK(LoadProject(project, saved, nullptr) && saved.plugins == std::vector<std::string>{"Lanterns"});
    CHECK(panel.SetEnabled("Lanterns", false, &error) && panel.Requested().empty());
    imgui.Frame([&] { panel.Draw(); });
}
