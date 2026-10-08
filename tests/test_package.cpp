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

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

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

std::string ReadText(const stdfs::path& path) {
    std::ifstream in(path, std::ios::binary);
    AETHER_CHECK(in.good());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void CheckNoStagingFolders(const stdfs::path& directory) {
    for (const stdfs::directory_entry& entry : stdfs::directory_iterator(directory)) {
        AETHER_CHECK(entry.path().filename().string().find(".aether-package-") != 0);
    }
}

json FileManifest(const std::string& path, const std::string& contents) {
    const u32 crc = pak::Crc32(std::span<const u8>(reinterpret_cast<const u8*>(contents.data()), contents.size()));
    char hex[9];
    std::snprintf(hex, sizeof(hex), "%08x", crc);
    return {{"$type", "PackageManifest"}, {"$version", 1}, {"checksum", "crc32"},
            {"files", json::array({{{"path", path}, {"bytes", static_cast<u64>(contents.size())}, {"crc32", hex}}})}};
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

AETHER_TEST(Package_FailedPlayerStagingKeepsThePreviousPackage) {
    const stdfs::path file = MakeProject("Rollback");
    const stdfs::path fake_player = file.parent_path() / "fake_player.bin";
    std::ofstream(fake_player, std::ios::binary) << "player version one";
    cook::PackageOptions options;
    options.cook.project_file = file;
    options.output_dir = file.parent_path() / "Packaged";
    options.player_executable = fake_player;
    const cook::PackageReport first = cook::Package(options);
    CHECK(first.ok);
    std::vector<std::string> problems;
    CHECK(cook::VerifyPackageManifest(options.output_dir, problems));

    // Force a different cook, then remove the player after validation but
    // before its staged copy. No artifact from the new cook may be published.
    Tags tags;
    tags.names = {"Changed"};
    const json scene = {{"$type", "Scene"}, {"$version", 1}, {"entities", json::array({{{"components", {{"Tags", reflect::ToJson(tags)}}}}})}};
    std::ofstream(file.parent_path() / "Content/Scenes/main.ascene", std::ios::binary) << scene.dump();
    options.cook.progress = [&](f32, const std::string& stage) {
        if (stage == "Staging the player") stdfs::remove(fake_player);
    };
    const cook::PackageReport failed = cook::Package(options);
    CHECK(!failed.ok && failed.error.find("Can't stage the player") == 0);
    CHECK(stdfs::exists(first.game_executable));
    CHECK(cook::VerifyPackageManifest(options.output_dir, problems));
    for (const stdfs::directory_entry& entry : stdfs::directory_iterator(options.output_dir)) {
        CHECK(entry.path().filename().string().find(".aether-package-") != 0);
    }
}

AETHER_TEST(Package_InvalidArchiveNameCannotEscapeStaging) {
    const stdfs::path file = MakeProject("ArchiveName");
    const stdfs::path fake_player = file.parent_path() / "fake_player.bin";
    std::ofstream(fake_player, std::ios::binary) << "player";
    cook::PackageOptions options;
    options.cook.project_file = file;
    options.output_dir = file.parent_path() / "Package";
    options.player_executable = fake_player;
    for (const std::string& name : {std::string(""), std::string("../escaped"), std::string("..\\escaped"),
                                   std::string("/absolute"), std::string("C:escaped"), std::string("Game\0suffix", 11)}) {
        options.cook.pak_name = name;
        options.cook.output_dir = file.parent_path() / "Cook";
        const cook::CookReport cooked = cook::Cook(options.cook);
        CHECK(!cooked.ok && cooked.error.find("Invalid pak name") == 0);
        CHECK(!stdfs::exists(options.cook.output_dir));
        const cook::PackageReport packaged = cook::Package(options);
        CHECK(!packaged.ok && packaged.error.find("Invalid pak name") == 0);
        CHECK(!stdfs::exists(options.output_dir / "escaped.apak"));
        CheckNoStagingFolders(options.output_dir);
    }
    CHECK(!stdfs::exists(file.parent_path() / "escaped.apak"));
}

AETHER_TEST(Package_CancelAfterCookKeepsThePreviousPackage) {
    const stdfs::path file = MakeProject("CancelStaging");
    const stdfs::path fake_player = file.parent_path() / "fake_player.bin";
    std::ofstream(fake_player, std::ios::binary) << "previous player";
    cook::PackageOptions options;
    options.cook.project_file = file;
    options.output_dir = file.parent_path() / "Packaged";
    options.player_executable = fake_player;
    const cook::PackageReport first = cook::Package(options);
    CHECK(first.ok);
    const std::string previous_manifest = ReadText(first.manifest_file);
    const std::string previous_pak = ReadText(first.cook.pak_file);
    std::ofstream(fake_player, std::ios::binary) << "replacement player";
    std::atomic<bool> cancel{false};
    options.cook.cancel = &cancel;
    for (const std::string& cancellation_stage : {std::string("Staging the player"), std::string("Installing the package")}) {
        cancel = false;
        options.cook.progress = [&](f32, const std::string& stage) {
            if (stage == cancellation_stage) cancel = true;
        };
        const cook::PackageReport stopped = cook::Package(options);
        CHECK(cancel && !stopped.ok && stopped.error == "Cancelled");
        CHECK(ReadText(first.game_executable) == "previous player");
        CHECK(ReadText(first.manifest_file) == previous_manifest);
        CHECK(ReadText(first.cook.pak_file) == previous_pak);
        std::vector<std::string> problems;
        CHECK(cook::VerifyPackageManifest(options.output_dir, problems));
        CheckNoStagingFolders(options.output_dir);
    }
    // A cancellation arriving in the completion callback is too late to
    // undo the fully installed package and must not turn success into failure.
    cancel = false;
    options.cook.progress = [&](f32 fraction, const std::string&) {
        if (fraction == 1.0f) cancel = true;
    };
    const cook::PackageReport completed = cook::Package(options);
    CHECK(completed.ok && cancel);
    CHECK(ReadText(completed.game_executable) == "replacement player");
    std::vector<std::string> problems;
    CHECK(cook::VerifyPackageManifest(options.output_dir, problems));
    CheckNoStagingFolders(options.output_dir);
}

AETHER_TEST(Package_RefusesDirectoryWhereAFileIsExpected) {
    const stdfs::path file = MakeProject("DirectoryCollision");
    const stdfs::path fake_player = file.parent_path() / "fake_player.bin";
    std::ofstream(fake_player, std::ios::binary) << "player";
    cook::PackageOptions options;
    options.cook.project_file = file;
    options.output_dir = file.parent_path() / "Packaged";
    options.player_executable = fake_player;
    const stdfs::path occupied = options.output_dir / cook::GameExecutableName("DirectoryCollision");
    stdfs::create_directories(occupied);
    std::ofstream(occupied / "keep.txt") << "user data";
    stdfs::create_directories(options.output_dir / "Paks");
    std::ofstream(options.output_dir / "Paks/Old.apak") << "old archive";
    const cook::PackageReport failed = cook::Package(options);
    CHECK(!failed.ok && failed.error.find("Refusing to replace non-file") == 0);
    CHECK(stdfs::is_directory(occupied));
    if (stdfs::is_directory(occupied)) CHECK(ReadText(occupied / "keep.txt") == "user data");
    CHECK(stdfs::exists(options.output_dir / "Paks/Old.apak"));
    CheckNoStagingFolders(options.output_dir);

    // Paks must be a directory; a same-named file is not package content.
    options.output_dir = file.parent_path() / "FileCollision";
    stdfs::create_directories(options.output_dir);
    std::ofstream(options.output_dir / "Paks") << "keep this file";
    CHECK(!cook::Package(options).ok);
    if (stdfs::is_regular_file(options.output_dir / "Paks")) {
        CHECK(ReadText(options.output_dir / "Paks") == "keep this file");
    } else {
        CHECK(false);
    }
    CheckNoStagingFolders(options.output_dir);
}

AETHER_TEST(Package_VerifierRejectsDamagedAndMalformedFiles) {
    const stdfs::path file = MakeProject("ManifestValidation");
    const stdfs::path directory = file.parent_path() / "Package";
    stdfs::create_directories(directory / "Paks");
    const std::string contents = "archive bytes";
    std::ofstream(directory / "Paks/Game.apak", std::ios::binary) << contents;
    const json manifest = FileManifest("Paks/Game.apak", contents);
    const auto verify = [&](const json& value) {
        std::ofstream(directory / "PackageManifest.json", std::ios::binary) << value.dump();
        std::vector<std::string> problems = {"stale diagnostic"};
        const bool ok = cook::VerifyPackageManifest(directory, problems);
        CHECK(ok == problems.empty());
        return ok;
    };
    CHECK(verify(manifest));
    std::ofstream(directory / "Paks/Game.apak", std::ios::binary) << "corrupt";
    CHECK(!verify(manifest));
    std::ofstream(directory / "Paks/Game.apak", std::ios::binary) << contents;
    for (const std::string& path : {std::string("../outside"), std::string("/absolute"),
                                   std::string("Paks\\Game.apak"), std::string("Paks/./Game.apak"),
                                   std::string("Paks/Game.apak\0suffix", 21), std::string("Paks/Game.apak:stream")}) {
        json bad = manifest;
        bad["files"][0]["path"] = path;
        CHECK(!verify(bad));
    }
    json bad = manifest;
    bad["files"].push_back(bad["files"][0]);
    CHECK(!verify(bad));
    bad = manifest;
    bad["files"][0]["bytes"] = -1;
    CHECK(!verify(bad));
    bad = manifest;
    bad["files"][0]["crc32"] = "not a crc";
    CHECK(!verify(bad));
    bad = manifest;
    bad["files"] = json::array();
    CHECK(!verify(bad));
#if defined(_WIN32)
    bad = manifest;
    bad["files"].push_back(bad["files"][0]);
    bad["files"][1]["path"] = "paks/game.apak";
    CHECK(!verify(bad));
#endif
    stdfs::remove(directory / "Paks/Game.apak");
    CHECK(!verify(manifest));
    stdfs::create_directory(directory / "Paks/Game.apak");
    CHECK(!verify(FileManifest("Paks/Game.apak", "")));
}

AETHER_TEST(Package_VerifierRejectsLinkedParentFoldersAndManifest) {
    const stdfs::path file = MakeProject("LinkedManifest");
    const stdfs::path directory = file.parent_path() / "Package";
    const stdfs::path outside = file.parent_path() / "Outside";
    stdfs::create_directories(directory);
    stdfs::create_directories(outside);
    const std::string contents = "external archive";
    std::ofstream(outside / "Game.apak", std::ios::binary) << contents;
    const json manifest = FileManifest("Paks/Game.apak", contents);
    std::ofstream(directory / "PackageManifest.json", std::ios::binary) << manifest.dump();
    std::error_code ec;
    stdfs::create_directory_symlink(outside, directory / "Paks", ec);
    if (ec) {
        std::printf("  Linked-path checks require symlink privileges: %s\n", ec.message().c_str());
        return; // Ordinary Windows accounts cannot create symlinks; Unix CI covers this case.
    }
    std::vector<std::string> problems;
    CHECK(!cook::VerifyPackageManifest(directory, problems));
    CHECK(!problems.empty());
    stdfs::remove(directory / "Paks");
    stdfs::create_directory(directory / "Paks");
    stdfs::copy_file(outside / "Game.apak", directory / "Paks/Game.apak");
    CHECK(cook::VerifyPackageManifest(directory, problems));
    stdfs::remove(directory / "PackageManifest.json");
    std::ofstream(outside / "PackageManifest.json", std::ios::binary) << manifest.dump();
    stdfs::create_symlink(outside / "PackageManifest.json", directory / "PackageManifest.json", ec);
    CHECK(!ec);
    CHECK(!cook::VerifyPackageManifest(directory, problems));
    stdfs::remove(directory / "PackageManifest.json");
    CHECK(ReadText(outside / "Game.apak") == contents);
}

#if defined(_WIN32)
AETHER_TEST(Package_LockedPlayerRollsBackInstalledContent) {
    const stdfs::path file = MakeProject("LockedRollback");
    const stdfs::path fake_player = file.parent_path() / "fake_player.bin";
    std::ofstream(fake_player, std::ios::binary) << "old player";
    cook::PackageOptions options;
    options.cook.project_file = file;
    options.output_dir = file.parent_path() / "Packaged";
    options.player_executable = fake_player;
    const cook::PackageReport first = cook::Package(options);
    CHECK(first.ok);
    const std::string previous_manifest = ReadText(first.manifest_file);
    const std::string previous_pak = ReadText(first.cook.pak_file);
    std::ofstream(fake_player, std::ios::binary) << "new player";
    std::ofstream(file.parent_path() / "Content/Scenes/main.ascene", std::ios::binary)
        << R"({"$type":"Scene","$version":1,"entities":[]})";
    // Permit reading, but deny rename/delete of the installed executable.
    // Paks is installed first, so the later backup failure must roll it back.
    HANDLE locked = CreateFileW(first.game_executable.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(locked != INVALID_HANDLE_VALUE);
    if (locked == INVALID_HANDLE_VALUE) return;
    const cook::PackageReport failed = cook::Package(options);
    CloseHandle(locked);
    CHECK(!failed.ok && failed.error.find("Can't back up") == 0);
    CHECK(ReadText(first.game_executable) == "old player");
    CHECK(ReadText(first.manifest_file) == previous_manifest);
    CHECK(ReadText(first.cook.pak_file) == previous_pak);
    std::vector<std::string> problems;
    CHECK(cook::VerifyPackageManifest(options.output_dir, problems));
    CheckNoStagingFolders(options.output_dir);
}
#endif

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
