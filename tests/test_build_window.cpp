#include "packaging/build_window.h"
#include "packaging/new_project_panel.h"
#include "packaging/plugins_panel.h"
#include "test_framework.h"
#include "workspace/editor_workspace.h"
#include "world/scene_panel.h"

#include <imgui.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
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

AETHER_TEST(BuildWindow_UnsavedSourceBlocksCookAndPackageUntilSaved) {
    HeadlessImGui imgui;
    const stdfs::path project = FreshProject("UnsavedSource");
    EditorWorkspace workspace(project);
    auto* package_window = workspace.PackageWindow();
    CHECK(package_window != nullptr);
    if (!package_window) return;
    auto& window = *package_window;
    window.player_path = AETHER_TEST_PLAYER_PATH;
    if (!workspace.Scene().OpenScene("Scenes/Main.ascene", true)) {
        CHECK(false);
        return;
    }
    workspace.Scene().Document().CreateEntity("Package source was saved");
    imgui.Frame([&] { window.Draw(); });
    CHECK(!window.StartCook());
    CHECK(!window.StartPackage());
    CHECK(!window.Busy() && window.StagedGame().empty() && !stdfs::exists(window.OutputDir()));
    CHECK(workspace.Scene().SaveScene());
    if (!window.StartPackage()) {
        CHECK(false);
        return;
    }
    window.WaitForBuild();
    CHECK(window.LastResult() == BuildPackageWindow::Result::Succeeded);
    CHECK(stdfs::is_regular_file(window.StagedGame()));
}

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

AETHER_TEST(BuildWindow_SmokeTestsTheStagedPlayerWithoutChangingLaunchArguments) {
    HeadlessImGui imgui;
    BuildPackageWindow window(FreshProject("EditorSmoke"));
    window.player_path = AETHER_TEST_PLAYER_PATH;
    window.configuration = cook::BuildConfiguration::Shipping;
    window.strict_validation = true;
    window.launch_args = "--not-a-player-flag";
    CHECK(!window.StartSmoke());
    if (!window.StartPackage()) { CHECK(false); return; }
    window.WaitForBuild();
    const auto game = window.StagedGame();
    if (game.empty()) { CHECK(false); return; }
    if (!window.StartSmoke()) { CHECK(false); return; }
    window.WaitForBuild();
    CHECK(window.LastResult() == BuildPackageWindow::Result::Succeeded);
    CHECK(window.Stage() == "Headless smoke test passed (3 frames)");
    CHECK(window.StagedGame() == game && window.launch_args == "--not-a-player-flag");

    // Exercise a process failure, not just the missing-archive precondition.
    const auto pak = game.parent_path() / "Paks" / "Game.apak";
    std::ofstream(pak, std::ios::binary | std::ios::trunc) << "not a package";
    if (!window.StartSmoke()) { CHECK(false); return; }
    window.WaitForBuild();
    imgui.Frame([&] { window.Draw(); });
    CHECK(window.LastResult() == BuildPackageWindow::Result::Failed && window.StagedGame() == game);
    CHECK(window.Stage().starts_with("Headless smoke test failed: player exited with code"));
    for (const auto& entry : stdfs::directory_iterator(stdfs::temp_directory_path()))
        CHECK(!entry.path().filename().string().starts_with("aether-editor-smoke-"));
}

AETHER_TEST(BuildWindow_ReportsMissingPackageArchiveBeforeLaunching) {
    BuildPackageWindow window(FreshProject("MissingArchive"));
    window.player_path = AETHER_TEST_PLAYER_PATH;
    if (!window.StartPackage()) {
        CHECK(false);
        return;
    }
    window.WaitForBuild();
    CHECK(window.LastResult() == BuildPackageWindow::Result::Succeeded);
    const stdfs::path archive = window.StagedGame().parent_path() / "Paks" / "Game.apak";
    if (!stdfs::is_regular_file(archive)) {
        CHECK(false);
        return;
    }
    const stdfs::path backup = archive.parent_path() / "Game.apak.bak";
    stdfs::rename(archive, backup);
    std::string error;
    window.launch_args = "--headless --frames 2";
    const bool started_without_archive = window.Launch(&error);
    CHECK(!started_without_archive);
    CHECK(error.find("Game.apak") != std::string::npos);
    if (started_without_archive) window.Game().Wait();
    stdfs::rename(backup, archive);
    CHECK(window.Launch(&error));
    int code = -1;
    CHECK(window.Game().Wait(&code) && code == 0);
}

AETHER_TEST(BuildWindow_VerifiesTheStagedPackageNotTheCurrentOutputField) {
    BuildPackageWindow window(FreshProject("VerifyPackage"));
    CHECK(!window.StartVerify());
    window.player_path = AETHER_TEST_PLAYER_PATH;
    if (!window.StartPackage()) {
        CHECK(false);
        return;
    }
    window.WaitForBuild();
    const stdfs::path game = window.StagedGame();
    CHECK(stdfs::is_regular_file(game));
    const stdfs::path archive = game.parent_path() / "Paks" / "Game.apak";
    if (!stdfs::is_regular_file(archive)) {
        CHECK(false);
        return;
    }
    // Staged data still belongs to the actual package, not a typed folder
    // intended for a later build.
    window.output_dir = (game.parent_path() / "not-the-package").string();
    if (!window.StartVerify()) {
        CHECK(false);
        return;
    }
    window.WaitForBuild();
    CHECK(window.LastResult() == BuildPackageWindow::Result::Succeeded);
    CHECK(window.Stage().find("Verified package") == 0 && window.StagedGame() == game);

    const stdfs::path backup = archive.parent_path() / "Game.apak.bak";
    stdfs::rename(archive, backup);
    CHECK(window.StartVerify());
    window.WaitForBuild();
    CHECK(window.LastResult() == BuildPackageWindow::Result::Failed && window.StagedGame() == game);
    bool named_archive = false;
    for (const auto& line : window.Log()) named_archive |= line.warning && line.text.find("Game.apak") != std::string::npos;
    CHECK(named_archive);

    stdfs::rename(backup, archive);
    CHECK(window.StartVerify());
    window.WaitForBuild();
    CHECK(window.LastResult() == BuildPackageWindow::Result::Succeeded);
}

AETHER_TEST(BuildWindow_VerifiesAnExistingPackageAfterEditorReopens) {
    const stdfs::path project = FreshProject("VerifyExistingOutput");
    {
        BuildPackageWindow previous(project);
        previous.player_path = AETHER_TEST_PLAYER_PATH;
        if (!previous.StartPackage()) {
            CHECK(false);
            return;
        }
        previous.WaitForBuild();
        CHECK(previous.LastResult() == BuildPackageWindow::Result::Succeeded);
    }
    BuildPackageWindow reopened(project);
    CHECK(reopened.StagedGame().empty());
    if (!reopened.StartVerify()) {
        CHECK(false);
        return;
    }
    reopened.WaitForBuild();
    CHECK(reopened.LastResult() == BuildPackageWindow::Result::Succeeded);
    CHECK(reopened.StagedGame().empty()); // a file check is not a new build or permission to launch
    std::string error;
    CHECK(!reopened.Launch(&error) && error == "Package the game first");

    reopened.output_dir = (project.parent_path() / "missing-output").string();
    CHECK(!reopened.StartVerify());
    CHECK(reopened.LastResult() == BuildPackageWindow::Result::Failed);
    CHECK(reopened.Stage().find("PackageManifest.json") != std::string::npos);
}

AETHER_TEST(BuildWindow_ReportsThePackagedPlayersExitOnlyOnce) {
    HeadlessImGui imgui;
    BuildPackageWindow window(FreshProject("PlayerExit"));
    window.player_path = AETHER_TEST_PLAYER_PATH;
    if (!window.StartPackage()) {
        CHECK(false);
        return;
    }
    window.WaitForBuild();
    window.launch_args = "--not-an-aether-option";
    if (!window.Launch()) {
        CHECK(false);
        return;
    }
    int code = -1;
    CHECK(window.Game().Wait(&code) && code != 0);
    imgui.Frame([&] { window.Draw(); });
    imgui.Frame([&] { window.Draw(); });
    int failures = 0;
    for (const auto& line : window.Log()) {
        if (line.warning && line.text.find("exited with code " + std::to_string(code)) != std::string::npos) ++failures;
    }
    CHECK(failures == 1);

    window.launch_args = "--headless --frames 2";
    if (!window.Launch()) {
        CHECK(false);
        return;
    }
    CHECK(window.Game().Wait(&code) && code == 0);
    imgui.Frame([&] { window.Draw(); });
    bool clean_exit = false;
    for (const auto& line : window.Log()) clean_exit |= !line.warning && line.text.find("exited normally") != std::string::npos;
    CHECK(clean_exit);
}

AETHER_TEST(BuildWindow_PackagedPlayerWritesRequestedDiagnosticLog) {
    const stdfs::path project = FreshProject("PlayerLog");
    BuildPackageWindow window(project);
    window.player_path = AETHER_TEST_PLAYER_PATH;
    if (!window.StartPackage()) {
        CHECK(false);
        return;
    }
    window.WaitForBuild();
    CHECK(window.LastResult() == BuildPackageWindow::Result::Succeeded);
    const stdfs::path log = project.parent_path() / "first launch.log";
    const std::string prefix = "--log-file \"" + log.string() + "\" ";
    const auto read_log = [&] {
        std::ifstream input(log);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    };

    window.launch_args = prefix + "--not-an-aether-option";
    if (!window.Launch()) {
        CHECK(false);
        return;
    }
    int code = -1;
    CHECK(window.Game().Wait(&code) && code == 2);
    CHECK(read_log().find("Invalid launch arguments") != std::string::npos);

    window.launch_args = prefix + "--headless --frames 2";
    if (!window.Launch()) {
        CHECK(false);
        return;
    }
    CHECK(window.Game().Wait(&code) && code == 0);
    const std::string second = read_log();
    CHECK(second.find("Loaded Scenes/Main.ascene") != std::string::npos);
    CHECK(second.find("Ran 2 frames") != std::string::npos);
    CHECK(second.find("Invalid launch arguments") == std::string::npos);
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
    // The settings describe the next build; changing them must not lose the
    // successful package's key.
    window.encrypt = false;
    window.encryption_key.clear();
    CHECK(window.Launch(&error));
    CHECK(window.Game().Wait(&code) && code == 0);
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

AETHER_TEST(NewProjectPanel_CreatesFromATemplateAndOpensIt) {
    HeadlessImGui imgui;
    const stdfs::path where = stdfs::temp_directory_path() / "aether_new_project_tests";
    stdfs::remove_all(where);
    EditorWorkspace workspace;
    const stdfs::path before = workspace.ProjectFile();
    CHECK(!before.empty());

    NewProjectPanel panel;
    CHECK(panel.location.find("AetherProjects") != std::string::npos && panel.template_id == "first_person");
    for (int i = 0; i < 2; ++i) imgui.Frame([&] { panel.Draw(); });
    panel.location = where.string();
    panel.name = "Racer";
    panel.template_id = "vehicle";
    stdfs::path announced;
    panel.on_created = [&](const stdfs::path& file) { announced = file; };
    std::string error;
    CHECK(panel.Create(&error) && !panel.StatusIsError() && announced == panel.Created());
    CHECK(announced == where / "Racer" / "Racer.aproject" && stdfs::exists(where / "Racer/Content/Scripts/VehicleController.luau"));
    imgui.Frame([&] { panel.Draw(); });

    // The same name again and a bad name each say why.
    CHECK(!panel.Create(&error) && panel.StatusIsError() && error.find("isn't empty") != std::string::npos);
    panel.template_id = "blank";
    panel.name = "no/slash";
    CHECK(!panel.Create(&error) && error.find("valid project name") != std::string::npos);
    imgui.Frame([&] { panel.Draw(); });

    // The workspace's Project tools follow a project it opens.
    CHECK(workspace.OpenProject(announced) && workspace.ProjectFile() == announced);
    CHECK(!workspace.OpenProject(where / "Nope.aproject") && workspace.ProjectFile() == announced);
    const i64 tool = workspace.FindTool("Project Settings");
    CHECK(tool >= 0 && workspace.FindTool("New Project") >= 0);
    workspace.Select(static_cast<usize>(tool));
    for (int i = 0; i < 2; ++i) imgui.Frame([&] { workspace.DrawHub(); });
    workspace.Select(static_cast<usize>(workspace.FindTool("New Project")));
    imgui.Frame([&] { workspace.DrawHub(); });
}
