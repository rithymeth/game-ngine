#include "aether/cook/package.h"
#include "aether/platform/process.h"
#include "aether/pak/pak.h"
#include "aether/player/game.h"
#include "aether/project/project.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/gameplay.h"
#include "test_framework.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Phase 25 step 5: the project's window and quality settings, the cook's
// progress and cancel, packaging, and launching processes.

using namespace aether;
using nlohmann::json;
namespace stdfs = std::filesystem;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

// A project with a one-entity startup scene.
stdfs::path MakeProject(const std::string& name) {
    const stdfs::path parent = stdfs::temp_directory_path() / "aether_package_tests";
    stdfs::remove_all(parent / name);
    stdfs::create_directories(parent);
    ProjectPaths paths;
    std::string error;
    AETHER_CHECK(CreateProject(parent, name, &paths, &error));
    Tags tags;
    tags.names = {"A"};
    const json scene = {{"$type", "Scene"}, {"$version", 1}, {"entities", json::array({{{"components", {{"Tags", reflect::ToJson(tags)}}}}})}};
    stdfs::create_directories(paths.content / "Scenes");
    std::ofstream(paths.content / "Scenes/main.ascene", std::ios::binary) << scene.dump();
    ProjectSettings settings;
    AETHER_CHECK(LoadProject(paths.file, settings, &error));
    settings.startup_scene = "Scenes/main.ascene";
    AETHER_CHECK(SaveProject(paths.file, settings, &error));
    return paths.file;
}

json ReadManifest(const stdfs::path& pak) {
    pak::PakReader reader;
    std::vector<u8> bytes;
    AETHER_CHECK(reader.Open(pak.string()));
    AETHER_CHECK(reader.Read("Manifest.json", bytes));
    return json::parse(bytes.begin(), bytes.end());
}

} // namespace

AETHER_TEST(Project_WindowAndQualitySettings) {
    ProjectSettings defaults;
    CHECK(defaults.window_width == 1280 && defaults.window_height == 720 && defaults.vsync);
    CHECK(defaults.quality_presets.size() == 4 && defaults.default_quality == "High");
    CHECK(FindQualityPreset(defaults, "Epic") && FindQualityPreset(defaults, "Epic")->shadow_resolution == 4096);
    CHECK(FindQualityPreset(defaults, "epic") == nullptr);

    const stdfs::path file = MakeProject("Settings");
    ProjectSettings s;
    std::string error;
    CHECK(LoadProject(file, s, &error));
    s.window_title = "My Game";
    s.window_width = 1920;
    s.vsync = false;
    s.quality_presets.resize(2);
    s.quality_presets[1].name = "Potato";
    s.quality_presets[1].resolution_scale = 0.5f;
    s.default_quality = "Potato";
    CHECK(SaveProject(file, s, &error));
    ProjectSettings back;
    CHECK(LoadProject(file, back, &error));
    CHECK(back.window_title == "My Game" && back.window_width == 1920 && back.window_height == 720 && !back.vsync);
    CHECK(back.quality_presets.size() == 2 && back.quality_presets[1].name == "Potato" &&
          back.quality_presets[1].resolution_scale == 0.5f && back.default_quality == "Potato");

    // A project saved before these settings existed gets the defaults.
    std::ifstream in(file);
    json saved = json::parse(in);
    in.close();
    saved.erase("window_width");
    saved.erase("quality_presets");
    std::ofstream(file) << saved.dump();
    ProjectSettings older;
    CHECK(LoadProject(file, older, &error));
    CHECK(older.window_width == 1280 && older.quality_presets.size() == 4);
}

AETHER_TEST(Cook_ReportsProgressCancelsAndShipsSettings) {
    const stdfs::path file = MakeProject("Progress");
    ProjectSettings s;
    CHECK(LoadProject(file, s, nullptr));
    s.window_title = "";
    s.window_height = 900;
    s.default_quality = "Nope";
    CHECK(SaveProject(file, s, nullptr));

    const stdfs::path out = file.parent_path() / "Build";
    cook::CookOptions options;
    options.project_file = file;
    options.output_dir = out;
    std::vector<f32> fractions;
    std::vector<std::string> stages;
    options.progress = [&](f32 f, const std::string& stage) {
        fractions.push_back(f);
        stages.push_back(stage);
    };
    const cook::CookReport report = cook::Cook(options);
    CHECK(report.ok && fractions.size() >= 4 && fractions.front() == 0.0f && fractions.back() == 1.0f);
    for (usize i = 1; i < fractions.size(); ++i) CHECK(fractions[i] >= fractions[i - 1]);
    CHECK(stages[1] == "Cooking Scenes/main.ascene" && stages.back() == "Done");
    bool warned = false;
    for (const std::string& w : report.warnings) warned |= w.find("'Nope' isn't a preset") != std::string::npos;
    CHECK(warned);

    const json m = ReadManifest(report.pak_file);
    CHECK(m["window"]["title"] == "Progress" && m["window"]["height"] == 900 && m["window"]["vsync"] == true);
    CHECK(m["quality_presets"].size() == 4 && m["quality_presets"][3]["name"] == "Epic" && m["default_quality"] == "Nope");
    player::GameManifest g;
    std::string error;
    CHECK(player::ParseGameManifest(m.dump(), g, &error));
    CHECK(g.window_title == "Progress" && g.window_height == 900 && g.quality_presets.size() == 4);
    CHECK(player::ChooseQuality(g)->name == "Low");           // the default isn't a preset: the first
    CHECK(player::ChooseQuality(g, "Epic")->name == "Epic");  // asked for
    g.default_quality = "Medium";
    CHECK(player::ChooseQuality(g, "Ultra")->name == "Medium"); // not a preset: the default

    // Cancelled before it starts: it fails and writes nothing.
    std::atomic<bool> cancel{true};
    options.cancel = &cancel;
    options.output_dir = file.parent_path() / "Cancelled";
    const cook::CookReport stopped = cook::Cook(options);
    CHECK(!stopped.ok && stopped.error == "Cancelled" && !stdfs::exists(options.output_dir));
}

AETHER_TEST(Package_StagesThePlayerWithThePaks) {
    CHECK(cook::GameExecutableName("My Game!") == std::string("MyGame") + (cook::GameExecutableName("x") == "x" ? "" : ".exe"));
    CHECK(cook::GameExecutableName("???").rfind("Game", 0) == 0);

    const stdfs::path file = MakeProject("Staged");
    const stdfs::path fake_player = file.parent_path() / "fake_player.bin";
    std::ofstream(fake_player, std::ios::binary) << "not really a player";
    cook::PackageOptions options;
    options.cook.project_file = file;
    options.output_dir = file.parent_path() / "Packaged";
    options.player_executable = fake_player;
    f32 last = -1.0f;
    options.cook.progress = [&](f32 f, const std::string&) { last = f; };
    // A stale pak from an earlier package is removed.
    stdfs::create_directories(options.output_dir / "Paks");
    std::ofstream(options.output_dir / "Paks" / "Old.apak") << "stale";
    const cook::PackageReport report = cook::Package(options);
    CHECK(report.ok && last == 1.0f);
    CHECK(report.game_executable == options.output_dir / cook::GameExecutableName("Staged"));
    CHECK(stdfs::file_size(report.game_executable) == stdfs::file_size(fake_player));
    CHECK(stdfs::exists(report.paks_dir / "Game.apak") && stdfs::exists(report.paks_dir / "CookManifest.json"));
    CHECK(!stdfs::exists(report.paks_dir / "Old.apak"));
    CHECK(player::GamePackage::FindPaks(report.paks_dir).size() == 1);

    options.player_executable = file.parent_path() / "missing_player";
    const cook::PackageReport missing = cook::Package(options);
    CHECK(!missing.ok && missing.error.find("No player at") == 0);
    options.player_executable = fake_player;
    options.output_dir.clear();
    CHECK(!cook::Package(options).ok);
}

AETHER_TEST(Process_StartsWaitsAndReportsTheExitCode) {
    const stdfs::path self = platform::ExecutablePath();
    CHECK(!self.empty() && stdfs::is_regular_file(self));

#if defined(_WIN32)
    const char* comspec = std::getenv("ComSpec");
    const stdfs::path shell = comspec ? comspec : "C:\\Windows\\System32\\cmd.exe";
    const std::vector<std::string> exit3 = {"/c", "exit 3"};
#else
    const stdfs::path shell = "/bin/sh";
    const std::vector<std::string> exit3 = {"-c", "exit 3"};
#endif
    platform::ChildProcess child;
    std::string error;
    CHECK(child.Start(shell, exit3, {}, &error) && child.Started());
    int code = -1;
    CHECK(child.Wait(&code) && code == 3 && child.ExitCode() == 3 && !child.Running());

    // In another folder: the child sees it as its working directory.
    const stdfs::path dir = stdfs::temp_directory_path() / "aether_process_test";
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
#if defined(_WIN32)
    const std::vector<std::string> touch = {"/c", "echo hi> made.txt"};
#else
    const std::vector<std::string> touch = {"-c", "echo hi > made.txt"};
#endif
    platform::ChildProcess writer;
    CHECK(writer.Start(shell, touch, dir, &error));
    CHECK(writer.Wait(&code) && code == 0 && stdfs::exists(dir / "made.txt"));

    platform::ChildProcess missing;
    CHECK(!missing.Start(dir / "no_such_program", {}, {}, &error) && error.find("No program") == 0);
    CHECK(!missing.Wait());
}
