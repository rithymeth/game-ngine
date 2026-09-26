#include "aether/platform/filesystem.h"
#include "aether/project/project.h"
#include "test_framework.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace aether;
namespace stdfs = std::filesystem;

namespace {

stdfs::path FreshDir(const char* name) {
    stdfs::path dir = stdfs::temp_directory_path() / name;
    stdfs::remove_all(dir);
    stdfs::create_directories(dir);
    return dir;
}

std::string ReadText(const stdfs::path& p) {
    std::ifstream f(p);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

} // namespace

AETHER_TEST(Project_CreateMakesLayoutAndLoadsBack) {
    stdfs::path parent = FreshDir("aether_test_projects");
    ProjectPaths paths;
    std::string error;
    AETHER_CHECK(CreateProject(parent, "My Game", &paths, &error));
    AETHER_CHECK(paths.file == parent / "My Game" / "My Game.aproject");
    for (const stdfs::path& dir : {paths.content, paths.config, paths.saved, paths.intermediate}) {
        AETHER_CHECK(stdfs::is_directory(dir));
    }
    std::string gitignore = ReadText(paths.root / ".gitignore");
    AETHER_CHECK(gitignore.find("Saved/") != std::string::npos && gitignore.find("Intermediate/") != std::string::npos);

    std::string text = ReadText(paths.file);
    AETHER_CHECK(text.find("\"$type\": \"Project\"") != std::string::npos);
    AETHER_CHECK(text.find(std::string("\"engine_version\": \"") + kEngineVersion + "\"") != std::string::npos);

    ProjectSettings loaded;
    std::vector<std::string> warnings;
    AETHER_CHECK(LoadProject(paths.file, loaded, &error, &warnings) && warnings.empty());
    AETHER_CHECK(loaded.name == "My Game");
    AETHER_CHECK(loaded.layers.size() == 1 && loaded.layers[0] == "Default");
    AETHER_CHECK(loaded.gravity.y == -9.81f && loaded.fixed_timestep_hz == 60.0f);

    // Edit, save, reload.
    loaded.startup_scene = "Maps/Level_01.ascene";
    loaded.plugins = {"Aether.Physics"};
    loaded.layers = {"Default", "Player", "Enemy"};
    AETHER_CHECK(SaveProject(paths.file, loaded, &error));
    ProjectSettings again;
    AETHER_CHECK(LoadProject(paths.file, again, &error));
    AETHER_CHECK(again.startup_scene == "Maps/Level_01.ascene" && again.plugins == loaded.plugins &&
                 again.layers == loaded.layers);
    // Saving the same settings is byte-stable.
    std::string first = ReadText(paths.file);
    AETHER_CHECK(SaveProject(paths.file, again, &error));
    AETHER_CHECK(ReadText(paths.file) == first);

    // A fresh clone is missing the ignored folders; EnsureProjectFolders fixes that.
    stdfs::remove_all(paths.saved);
    stdfs::remove_all(paths.intermediate);
    AETHER_CHECK(EnsureProjectFolders(ProjectPaths::ForFile(paths.file), &error));
    AETHER_CHECK(stdfs::is_directory(paths.saved) && stdfs::is_directory(paths.intermediate));
    stdfs::remove_all(parent);
}

AETHER_TEST(Project_CreateRejectsBadNamesAndNonEmptyFolders) {
    stdfs::path parent = FreshDir("aether_test_projects_bad");
    std::string error;
    AETHER_CHECK(!CreateProject(parent, "", nullptr, &error));
    AETHER_CHECK(!CreateProject(parent, "../escape", nullptr, &error));
    AETHER_CHECK(!CreateProject(parent, "a/b", nullptr, &error));
    AETHER_CHECK(!CreateProject(parent, " padded", nullptr, &error));
    AETHER_CHECK(!error.empty());
    AETHER_CHECK(CreateProject(parent, "Taken", nullptr, &error));
    AETHER_CHECK(!CreateProject(parent, "Taken", nullptr, &error)); // exists, not empty
    AETHER_CHECK(error.find("isn't empty") != std::string::npos);
    stdfs::remove_all(parent);
}

AETHER_TEST(Project_LoadIsTolerantButRejectsNonProjects) {
    stdfs::path dir = FreshDir("aether_test_projects_load");
    stdfs::path file = dir / "Future.aproject";
    const char* future = R"({"$type": "Project", "$v": 1, "name": "Future", "engine_version": "9.3.0",
                             "some_new_setting": true, "layers": ["Player"]})";
    AETHER_CHECK(fs::WriteFileBytes(file.string(), future, std::strlen(future)));
    ProjectSettings loaded;
    std::string error;
    std::vector<std::string> warnings;
    AETHER_CHECK(LoadProject(file, loaded, &error, &warnings));
    AETHER_CHECK(loaded.name == "Future");
    AETHER_CHECK(loaded.layers.size() == 2 && loaded.layers[0] == "Default" && loaded.layers[1] == "Player");
    AETHER_CHECK(warnings.size() == 2); // newer engine + missing Default layer
    AETHER_CHECK(loaded.fixed_timestep_hz == 60.0f); // missing field keeps its default

    const char* scene = R"({"$type": "Scene", "$version": 1, "entities": []})";
    AETHER_CHECK(fs::WriteFileBytes(file.string(), scene, std::strlen(scene)));
    AETHER_CHECK(!LoadProject(file, loaded, &error));
    AETHER_CHECK(error.find("isn't an Aether project") != std::string::npos);
    AETHER_CHECK(!LoadProject(dir / "missing.aproject", loaded, &error));
    stdfs::remove_all(dir);
}
